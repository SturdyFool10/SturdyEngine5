/// C ABI implementation of the application-hosting seam: configuration, the `Engine::GameLogic`
/// bridge that forwards virtual calls to caller-supplied function pointers, and the blocking
/// `sturdy_runtime_run` entry point.

#include <Foundation/Foundation.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <Runtime/Runtime.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::HandleKind;
    using SFT::Ffi::ScopedHandle;
    using SFT::Ffi::guarded;
    using SFT::Ffi::set_error;

    /// Forwards `Engine::GameLogic`'s virtual calls to the caller's function pointers.
    ///
    /// Owns the responsibility for invoking `destroy` on `user_data` exactly once: whichever path
    /// tears this object down — normal shutdown, a failed startup, an exception unwinding out of
    /// the frame loop — runs the destructor, so a binding's language-side object is released at a
    /// defined point rather than leaking.
    class ForeignGameLogic final : public SFT::Engine::GameLogic {
      public:
        /// Constructs a `ForeignGameLogic` from the supplied callback table.
        ///
        /// @param interface_table Caller-supplied callbacks, copied by value. The caller's own
        ///        struct need not outlive this object.
        /// @param failure_flag Set when a callback reports failure, so `sturdy_runtime_run` can
        ///        distinguish "your callback said no" from "the engine failed on its own".
        ///
        /// @note This function does not throw exceptions.
        ForeignGameLogic(const SturdyGameLogic &interface_table,
                         std::atomic<bool> &failure_flag,
                         std::atomic<bool> &initialized_flag) noexcept
            : interface_(interface_table), failure_flag_(failure_flag), initialized_flag_(initialized_flag) {}

        /// Destroys the `ForeignGameLogic` and releases the caller's user data.
        ///
        /// @note This function does not throw exceptions.
        ~ForeignGameLogic() override {
            if (interface_.destroy != nullptr) {
                interface_.destroy(interface_.user_data);
            }
        }

        /// Handles the engine initialized event by forwarding to the caller.
        ///
        /// @param engine Engine to expose for the duration of the callback.
        ///
        /// @return Returns success, or an error when the caller's callback reported failure.
        /// @note Normal failures are returned through the type-specific error/status state.
        [[nodiscard]] SFT::Engine::GameLogicResult on_engine_initialized(SFT::Engine::Engine &engine) override {
            // Recorded before the null check: reaching this point at all means the engine came up,
            // which is the fact sturdy_runtime_run needs, whether or not the caller wanted a hook.
            initialized_flag_.store(true, std::memory_order_relaxed);

            // Before the caller's hook, so anything it spawns is already extractable, and here
            // rather than lazily from a render setter because binding resources and adding systems
            // is illegal once a schedule is running.
            SFT::Ffi::install_render_extraction(engine);
            if (interface_.on_engine_initialized == nullptr) {
                return {};
            }

            const ScopedHandle engine_handle{HandleKind::Engine, &engine};
            const SturdyBool accepted = interface_.on_engine_initialized(
                SturdyEngine{engine_handle.token()}, interface_.user_data);
            if (accepted == STURDY_FALSE) {
                failure_flag_.store(true, std::memory_order_relaxed);
                return std::unexpected(SFT::Engine::GameLogicError{
                    SFT::UString{"the C ABI on_engine_initialized callback reported failure"}});
            }
            return {};
        }

        /// Requests render frame parameters from the caller.
        ///
        /// @param engine Engine to expose for the duration of the callback.
        /// @param surface Surface the frame is being produced for.
        /// @param frame Timing and geometry for this frame.
        ///
        /// @return The configured parameters, or `std::nullopt` when the caller declined to render.
        /// @note Concrete implementations define backend-specific failure details.
        [[nodiscard]] std::optional<SFT::Engine::RenderFrameParameters> request_render_frame(
            SFT::Engine::Engine &engine,
            SFT::Core::RenderSurfaceHandle surface,
            const SFT::Core::FrameInput &frame) override {
            if (interface_.request_render_frame == nullptr) {
                return std::nullopt;
            }

            SFT::Engine::RenderFrameParameters parameters{};
            // Start from the standard pipeline rather than a default-constructed (empty) graph, so
            // a caller that only positions a camera still gets a complete, sensible frame. An
            // empty graph would present nothing, which reads as a broken binding rather than as
            // the deliberate choice it would be in C++.
            parameters.render_graph = SFT::Engine::RenderGraph::standard();
            // Pre-match the camera to the surface so the aspect ratio is correct without the
            // caller having to notice. Overridable via sturdy_frame_set_camera_viewport.
            if (frame.framebuffer_width != 0 && frame.framebuffer_height != 0) {
                parameters.camera.set_viewport_size(frame.framebuffer_width, frame.framebuffer_height);
            }

            SturdyFrameInput input{};
            input.struct_size = static_cast<uint32_t>(sizeof(SturdyFrameInput));
            input.delta_seconds = frame.delta_seconds;
            input.frame_index = frame.frame_index;
            input.framebuffer_width = frame.framebuffer_width;
            input.framebuffer_height = frame.framebuffer_height;
            input.live_resize = frame.live_resize ? STURDY_TRUE : STURDY_FALSE;

            const ScopedHandle engine_handle{HandleKind::Engine, &engine};
            const ScopedHandle frame_handle{HandleKind::Frame, &parameters};
            const SturdyBool render = interface_.request_render_frame(
                SturdyEngine{engine_handle.token()},
                SturdySurface{static_cast<uint64_t>(surface.window_id)},
                &input,
                SturdyFrame{frame_handle.token()},
                interface_.user_data);

            if (render == STURDY_FALSE) {
                return std::nullopt;
            }
            return parameters;
        }

        /// Handles the shutdown event by forwarding to the caller.
        ///
        /// @param engine Engine to expose for the duration of the callback.
        ///
        /// @note This function does not throw exceptions.
        void on_shutdown(SFT::Engine::Engine &engine) noexcept override {
            if (interface_.on_shutdown == nullptr) {
                return;
            }
            const ScopedHandle engine_handle{HandleKind::Engine, &engine};
            interface_.on_shutdown(SturdyEngine{engine_handle.token()}, interface_.user_data);
        }

      private:
        SturdyGameLogic interface_;
        std::atomic<bool> &failure_flag_;
        std::atomic<bool> &initialized_flag_;
    };

    /// State shared between `sturdy_runtime_run` and the factory function it hands to
    /// `Runtime::run`.
    ///
    /// `Engine::GameLogicFactory` is a bare `unique_ptr<GameLogic>(*)()` with no context
    /// parameter, so the callback table has to reach the factory through a static. That is only
    /// sound because `g_runtime_active` below admits exactly one runtime at a time.
    struct ActiveRuntime {
        SturdyGameLogic interface_table{};
        std::atomic<bool> callback_failed{false};
        // Whether the engine got far enough to hand game logic a live engine. Distinguishes "the
        // device or window could not be created" from "the application ran and chose to exit
        // nonzero", which Runtime::run collapses into a single exit code.
        std::atomic<bool> engine_initialized{false};
        bool logic_constructed = false;
    };

    ActiveRuntime g_active;

    /// Whether a runtime is currently executing in this process.
    ///
    /// Guards both `g_active` and the engine's own process-wide state (window manager, graphics
    /// device), neither of which supports two concurrent applications.
    std::atomic<bool> g_runtime_active{false};

    /// Builds the game logic for the runtime described by `g_active`.
    ///
    /// @return Returns the newly constructed game logic.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] std::unique_ptr<SFT::Engine::GameLogic> create_foreign_game_logic() {
        g_active.logic_constructed = true;
        return std::make_unique<ForeignGameLogic>(g_active.interface_table, g_active.callback_failed,
                                                  g_active.engine_initialized);
    }

    /// Releases the runtime claim taken by `sturdy_runtime_run`.
    ///
    /// Also invokes `destroy` when the factory never ran — otherwise a startup that fails before
    /// game logic is constructed would leak the caller's user data.
    ///
    /// @note This function does not throw exceptions.
    void release_runtime() noexcept {
        if (!g_active.logic_constructed && g_active.interface_table.destroy != nullptr) {
            g_active.interface_table.destroy(g_active.interface_table.user_data);
        }
        g_active.interface_table = SturdyGameLogic{};
        g_active.callback_failed.store(false, std::memory_order_relaxed);
        g_active.engine_initialized.store(false, std::memory_order_relaxed);
        g_active.logic_constructed = false;
        g_runtime_active.store(false, std::memory_order_release);
    }

    /// Releases the runtime claim when its scope ends, on every path including an exception
    /// unwinding out of `Runtime::run`.
    class RuntimeClaim {
      public:
        /// Constructs a `RuntimeClaim`.
        ///
        /// @note This function does not throw exceptions.
        RuntimeClaim() noexcept = default;

        /// Destroys the `RuntimeClaim` and releases the runtime.
        ///
        /// @note This function does not throw exceptions.
        ~RuntimeClaim() { release_runtime(); }

        /// Disables this construction form for `RuntimeClaim`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        RuntimeClaim(const RuntimeClaim &) = delete;
        /// Assigns a new value to this `RuntimeClaim`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        RuntimeClaim &operator=(const RuntimeClaim &) = delete;
    };

    /// Resolves the requested backend into the engine's backend and window-surface enumerations.
    ///
    /// @param backend Value received from the caller.
    /// @param out_backend Receives the RHI backend.
    /// @param out_graphics_api Receives the matching window graphics API, which must agree with
    ///        the backend or the window will be created with the wrong native surface type.
    ///
    /// @return Returns `true` when `backend` is available on this platform; otherwise `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool resolve_backend(SturdyBackend backend,
                                       SFT::RHI::BackendType *out_backend,
                                       SFT::WindowManager::WindowGraphicsApi *out_graphics_api) noexcept {
        SturdyBackend selected = backend;
        if (selected == STURDY_BACKEND_DEFAULT) {
#if defined(_WIN32)
            selected = STURDY_BACKEND_D3D12;
#else
            selected = STURDY_BACKEND_VULKAN;
#endif
        }

        switch (selected) {
        case STURDY_BACKEND_VULKAN:
            *out_backend = SFT::RHI::BackendType::Vulkan;
            *out_graphics_api = SFT::WindowManager::WindowGraphicsApi::Vulkan;
            return true;
        case STURDY_BACKEND_D3D12:
#if defined(_WIN32)
            *out_backend = SFT::RHI::BackendType::D3D12;
            *out_graphics_api = SFT::WindowManager::WindowGraphicsApi::Direct3D;
            return true;
#else
            // D3D12 sources are excluded from non-Windows builds entirely, so this is not a
            // runtime capability question — the backend does not exist in this binary.
            return false;
#endif
        case STURDY_BACKEND_DEFAULT:
        case STURDY_BACKEND_FORCE_U32:
        default:
            return false;
        }
    }

    /// Translates an ABI vsync mode to the engine's own enumeration.
    ///
    /// @param vsync Value received from the caller.
    /// @param out_mode Receives the translated value.
    ///
    /// @return Returns `true` when `vsync` is a value this build recognizes; otherwise `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool translate_vsync(SturdyVSync vsync, SFT::Core::VSyncMode *out_mode) noexcept {
        switch (vsync) {
        case STURDY_VSYNC_OFF:
            *out_mode = SFT::Core::VSyncMode::Off;
            return true;
        case STURDY_VSYNC_ON:
            *out_mode = SFT::Core::VSyncMode::On;
            return true;
        case STURDY_VSYNC_ADAPTIVE:
            *out_mode = SFT::Core::VSyncMode::Adaptive;
            return true;
        case STURDY_VSYNC_FORCE_U32:
        default:
            return false;
        }
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_runtime_config_init(SturdyRuntimeConfig *config) {
    return guarded([&]() -> SturdyResult {
        if (config == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "config must not be null");
        }

        *config = SturdyRuntimeConfig{};
        config->struct_size = static_cast<uint32_t>(sizeof(SturdyRuntimeConfig));
        config->window_width = 1280;
        config->window_height = 720;
        config->window_resizable = STURDY_TRUE;
        config->graphics_backend = STURDY_BACKEND_DEFAULT;
        config->vsync = STURDY_VSYNC_ON;
        config->enable_raytracing = STURDY_FALSE;
        config->enable_shader_disk_cache = STURDY_TRUE;
        // Off by default: publishing raw backend objects is an explicit opt-in, not something a
        // caller should acquire by forgetting to set a field.
        config->enable_native_access = STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_runtime_run(const SturdyRuntimeConfig *config,
                                                const SturdyGameLogic *game_logic,
                                                int32_t argc,
                                                const char *const *argv,
                                                int32_t *out_exit_code) {
    return guarded([&]() -> SturdyResult {
        if (config == nullptr || game_logic == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "config and game_logic must not be null");
        }
        // Layout history: v0.1 is the only layout of either struct, so these are equality checks
        // today. When fields are appended they become range checks against the oldest supported
        // layout, with the unsupplied tail left at its default — that is the whole story for how
        // an older binding keeps working against a newer engine.
        if (config->struct_size != sizeof(SturdyRuntimeConfig)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE,
                             "SturdyRuntimeConfig size does not match this engine build");
        }
        if (game_logic->struct_size != sizeof(SturdyGameLogic)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE,
                             "SturdyGameLogic size does not match this engine build");
        }
        if (argc < 0 || (argc > 0 && argv == nullptr)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "argv must not be null when argc is greater than zero");
        }
        if (config->window_width == 0 || config->window_height == 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "window dimensions must be nonzero");
        }

        SFT::RHI::BackendType backend{};
        SFT::WindowManager::WindowGraphicsApi graphics_api{};
        if (!resolve_backend(config->graphics_backend, &backend, &graphics_api)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "the requested graphics backend is not available on this platform");
        }

        SFT::Core::VSyncMode vsync{};
        if (!translate_vsync(config->vsync, &vsync)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized vsync mode");
        }

        // Claim the process-wide runtime slot before touching g_active. acquire/release rather
        // than relaxed so a second thread that loses this race also sees the winner's writes.
        bool expected = false;
        if (!g_runtime_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return set_error(STURDY_ERROR_ALREADY_RUNNING,
                             "a Sturdy runtime is already running in this process");
        }

        g_active.interface_table = *game_logic;
        g_active.callback_failed.store(false, std::memory_order_relaxed);
        g_active.engine_initialized.store(false, std::memory_order_relaxed);
        g_active.logic_constructed = false;
        const RuntimeClaim claim;

        SFT::Foundation::CliArgs args;
        args.reserve(static_cast<std::size_t>(argc));
        for (int32_t index = 0; index < argc; ++index) {
            args.emplace_back(argv[index] != nullptr ? argv[index] : "");
        }

        // ApplicationConfig borrows its string fields as `const char *`, so these owning copies
        // must outlive the Runtime::run() call below. The caller's own pointers are only
        // guaranteed for the duration of this function, which is not the same lifetime.
        const std::string app_name{config->app_name != nullptr ? config->app_name : "Sturdy Engine 5"};
        const std::string window_title{config->window_title != nullptr ? config->window_title
                                                                       : "Sturdy Engine 5"};
        const std::string shaders_directory{
            config->shaders_directory != nullptr ? config->shaders_directory : "Shaders"};
        const std::string physical_device_id{
            config->physical_device_id != nullptr ? config->physical_device_id : ""};

        SFT::Runtime::RuntimeConfig runtime_config{};
        runtime_config.application.primary_window.title = window_title.c_str();
        runtime_config.application.primary_window.extent = {config->window_width, config->window_height};
        runtime_config.application.primary_window.resizable = config->window_resizable != STURDY_FALSE;
        runtime_config.application.primary_window.graphics_api = graphics_api;
        runtime_config.application.engine.graphics_backend = backend;
        runtime_config.application.engine.graphics_physical_device_id = physical_device_id;
        runtime_config.application.engine.app_name = app_name.c_str();
        runtime_config.application.engine.shaders_directory = shaders_directory;
        runtime_config.application.engine.enable_shader_disk_cache =
            config->enable_shader_disk_cache != STURDY_FALSE;
        runtime_config.application.engine.features.raytracing = config->enable_raytracing != STURDY_FALSE;
        runtime_config.application.engine.features.enable_native_access_extension =
            config->enable_native_access != STURDY_FALSE;
        runtime_config.application.engine.features.presentation.vsync = vsync;
        runtime_config.primary_window_title = SFT::UString{window_title.c_str()};

        const SFT::i32 exit_code =
            SFT::Runtime::run(args, std::move(runtime_config), &create_foreign_game_logic);

        if (out_exit_code != nullptr) {
            *out_exit_code = exit_code;
        }

        // A callback that declined startup is reported as such, rather than as a generic nonzero
        // exit code, so a binding can tell its own rejection apart from an engine failure.
        if (g_active.callback_failed.load(std::memory_order_relaxed)) {
            return set_error(STURDY_ERROR_CALLBACK_FAILED,
                             "a game logic callback reported failure");
        }
        // Nonzero exit without the engine ever reaching game logic means startup itself failed —
        // no window, no device, or a requested GPU that this machine does not have. Reporting that
        // as STURDY_OK would leave a caller inspecting an exit code with no idea what went wrong.
        if (exit_code != 0 && !g_active.engine_initialized.load(std::memory_order_relaxed)) {
            return set_error(STURDY_ERROR_INITIALIZATION_FAILED,
                             "the engine failed to initialize; see the engine log for the cause");
        }
        return STURDY_OK;
    });
}

} // extern "C"
