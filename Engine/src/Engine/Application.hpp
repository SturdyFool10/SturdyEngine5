#pragma once

#include <Foundation/Foundation.hpp>
#include <RHI/Threading.hpp>

#pragma region Imports
#include <Async/Async.hpp>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <vector>
#pragma endregion

#include <Engine/EngineModule.hpp>
#include <Engine/GameLogic.hpp>
#include <Engine/WindowState.hpp>
#include <Core/Core.hpp>
#include <WindowManager/WindowManager.hpp>

using std::atomic;
using std::optional;
using std::shared_ptr;
using std::unique_ptr;
using std::vector;

namespace SFT::Engine {

    struct ApplicationConfig {
        WindowManager::WindowConfig primary_window;


        WindowManager::WindowFactory primary_window_factory = nullptr;
        EngineConfig engine;

        optional<f64> primary_window_title_update_interval_seconds;


        bool enable_runtime_window_management = false;
    };

    struct ApplicationFrameStats {
        f64 frame_seconds = 0.0;
        u64 frame_index = 0;
        usize window_count = 0;
    };


    using ApplicationError = GameLogicError;
    using ApplicationResult = GameLogicResult;


    class ApplicationClient : public GameLogic {
      public:
        /// Destroys the `ApplicationClient` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~ApplicationClient() override = default;

        /// Returns the current or globally available application config value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual const ApplicationConfig &application_config() const noexcept = 0;
        /// Performs the primary window title operation for `ApplicationClient` using the supplied arguments.
        ///
        /// @param engine `engine` value used by the operation.
        /// @param stats `stats` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] virtual UString primary_window_title(
            Engine &engine,
            const ApplicationFrameStats &stats) = 0;
    };


    class Application {
      public:
        /// Constructs a `Application` from the supplied initialization values.
        ///
        /// @param client `client` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit Application(ApplicationClient &client) noexcept;
        /// Destroys the `Application` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~Application();

        /// Initializes the `Application` for use.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        bool initialize();
        /// Runs the requested work.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void run();


        /// Spawns secondary window.
        ///
        /// @param config Configuration values controlling the operation.
        /// @param factory `factory` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        [[nodiscard]] optional<Core::RenderSurfaceHandle> spawn_secondary_window(
            const WindowManager::WindowConfig &config,
            WindowManager::WindowFactory factory = nullptr);


        /// Recreates primary window using the supplied arguments and current state.
        ///
        /// @param config Configuration values controlling the operation.
        /// @param factory `factory` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        [[nodiscard]] optional<Core::RenderSurfaceHandle> recreate_primary_window(
            const WindowManager::WindowConfig &config,
            WindowManager::WindowFactory factory = nullptr);


        /// Requests close window using the supplied arguments and current state.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void request_close_window(WindowManager::WindowId id) noexcept;

      private:


        struct LiveResizeState {
            /// Performs the publish operation for `LiveResizeState` using the supplied arguments.
            ///
            /// @param extent `extent` value used by the operation.
            ///
            /// @note This function does not throw exceptions.
            void publish(WindowManager::WindowExtent extent) noexcept;

            /// Returns the current or globally available consume value.
            ///
            /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
            /// @note Normal inability to produce a value is represented by an empty optional.
            /// @note This function does not throw exceptions.
            [[nodiscard]] optional<WindowManager::WindowExtent> consume() noexcept;

          private:
            atomic<u64> pending_extent{0};
        };


        struct ManagedWindow {
            WindowManager::WindowId window_id{};
            optional<Core::RenderSurfaceHandle> surface;
            bool primary = false;
            bool closing = false;


            bool focused = false;
            WindowSnapshot window_snapshot{};


            optional<WindowManager::CursorIcon> applied_cursor_icon;


            atomic<u64> pending_resize_extent{0};
            shared_ptr<LiveResizeState> live_resize;
            optional<WindowManager::WindowExtent> pending_live_resize;


            optional<WindowManager::WindowExtent> submitted_live_resize;


            optional<WindowManager::WindowExtent> attempted_live_resize;
            std::chrono::high_resolution_clock::time_point next_live_resize_submission{};


            std::chrono::high_resolution_clock::time_point live_resize_nonblocking_until{};
            bool live_resize_active = false;
            std::deque<Async::TaskHandle<Core::RendererResult>> in_flight_frames;
            optional<Async::TaskHandle<void>> remove_surface_task;


            optional<WindowRequestId> pending_close_completion;
            // Every native window effect (blur/acrylic/mica/transparent/...) currently requested for
            // this window, one entry per WindowEffectKind (a later SetBlurRequest/SetTransparentRequest
            // for the same kind replaces its entry rather than appending) — see
            // process_window_requests()'s SetBlurRequest/SetTransparentRequest handlers, the only place
            // these mutate. Window effects have no persistent record anywhere else (a live Window only
            // tracks its own applied state internally, e.g. SDL3Window::active_blur_effect_, which is
            // discarded along with the window itself on teardown), so this is what lets
            // recreate_primary_window() below carry a window's current effects forward onto its
            // replacement instead of silently dropping them.
            vector<WindowManager::WindowEffect> applied_effects;
            // Each window gets its own dedicated render thread (rather than every window sharing one
            // Application-wide thread) so recording/submitting/presenting for separate OS windows —
            // most commonly several torn-off docking panels — actually runs concurrently instead of
            // serializing on a single CPU thread. Null under the same conditions the old shared
            // render_thread_ was null (see use_render_threading_'s doc comment): the inline
            // single-threaded fallback in render_managed_window() applies per window exactly as before.
            unique_ptr<Async::DedicatedThread> render_thread;
            u64 frame_index = 0;
            std::chrono::high_resolution_clock::time_point last_frame_time{};
            f64 last_delta_seconds = 0.0;
        };


        /// Drains render thread using the supplied arguments and current state.
        ///
        /// @note This function does not throw exceptions.
        void drain_render_thread() noexcept;


        /// Shuts down client and releases associated runtime state.
        ///
        /// @note This function does not throw exceptions.
        void shutdown_client() noexcept;

        /// Finds managed window in the available state.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ManagedWindow *find_managed_window(WindowManager::WindowId id) noexcept;
        /// Performs the process window requests operation for `Application` using the supplied arguments.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void process_window_requests();

        /// Updates `id`'s ManagedWindow::applied_effects with `effect` (replacing any existing entry
        /// for the same WindowEffectKind), if `id` is currently a managed window.
        ///
        /// @param id Managed window to update; a no-op if this isn't currently a managed window.
        /// @param effect Window effect to record, replacing any existing entry with the same
        ///        WindowEffectKind rather than appending.
        ///
        /// @note This is the only place that record is written — see ManagedWindow::applied_effects'
        ///       own doc comment for why it exists.
        /// @note This function does not throw exceptions.
        void record_applied_effect(WindowManager::WindowId id, WindowManager::WindowEffect effect) noexcept;

        /// Spawns one window through an explicitly supplied optional provider factory, or SDL3 when
        /// `factory` is null, then registers it with the engine's render-surface set and repaint path.
        ///
        /// @param config Configuration values controlling the new window.
        /// @param factory Explicit provider factory, or null to use the built-in SDL3 path — kept
        ///        indirect (rather than resolved here) so Application never retains optional provider
        ///        symbols merely by linking their static archives.
        /// @param is_primary Whether this is the bootstrap primary window (routes through
        ///        Engine::initialize()) or an additional window (routes through Engine::add_window()).
        ///
        /// @return true on success; false if window creation or render-surface registration failed.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        bool spawn_managed_window(
            const WindowManager::WindowConfig &config,
            WindowManager::WindowFactory factory,
            bool is_primary);


        /// Renders managed window using the current rendering state.
        ///
        /// @param managed `managed` value used by the operation.
        /// @param extent `extent` value used by the operation.
        /// @param resized `resized` value used by the operation.
        /// @param coalesce_if_backpressured `coalesce_if_backpressured` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool render_managed_window(ManagedWindow &managed,
                                                 WindowManager::WindowExtent extent,
                                                 bool resized,
                                                 bool coalesce_if_backpressured = false);
        /// Performs the report frame result operation for `Application` using the supplied arguments.
        ///
        /// @param result `result` value used by the operation.
        ///
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        void report_frame_result(const Core::RendererResult &result) noexcept;


        /// Performs the sync window state operation for `Application` using the supplied arguments.
        ///
        /// @param window_events Window used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void sync_window_state(const vector<WindowManager::ManagedWindowEvents> &window_events);

        /// Runs one iteration of the main loop's body: pumps platform events, settles any live
        /// resizes, ticks the engine, submits a frame per window, retires windows finishing
        /// close, and refreshes the primary window's title on its configured interval.
        ///
        /// Factored out of run() so a browser build can drive it from one requestAnimationFrame
        /// callback at a time (via emscripten_set_main_loop_arg in ApplicationImpl.cpp) instead
        /// of a blocking `while` loop, which would never yield back to the browser's single JS
        /// thread and freeze the page.
        ///
        /// @param window_events Scratch buffer reused across calls to avoid a per-frame allocation.
        /// @param last_title_update In/out timestamp of the last primary-window title refresh.
        /// @param last_tick_time In/out timestamp of the previous call, used to compute this
        ///        frame's tick delta.
        ///
        /// @return false when the loop should stop (a fatal platform event-pump error); true to
        ///         keep going. Callers must also stop once `windows_` is empty.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool run_frame(
            vector<WindowManager::ManagedWindowEvents> &window_events,
            std::chrono::high_resolution_clock::time_point &last_title_update,
            std::chrono::high_resolution_clock::time_point &last_tick_time);

        WindowManager::WindowManager window_manager_{
            WindowManager::WindowManagerPolicy{.event_pump_mode = WindowManager::WindowEventPumpMode::DedicatedEventThread,
                                                     .platform_allows_threads = true}};
        vector<unique_ptr<ManagedWindow>> windows_;
        unique_ptr<Engine> engine_;
        ApplicationClient *client_ = nullptr;
        bool client_initialized_ = false;
        bool client_shutdown_ = false;


        bool use_render_threading_ = false;
        u32 max_frames_in_flight_ = 2;
        u32 consecutive_render_errors_ = 0;
    };

} // namespace SFT::Engine
