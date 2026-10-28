#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <filesystem>
#include <optional>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <unordered_map>
#include <string_view>
#include <vector>
#pragma endregion

#include <Engine/AssetManager.hpp>
#include <Engine/Docking/DockWindowCoordinator.hpp>
#include <Engine/EcsEvents.hpp>
#include <Engine/EcsAnimation.hpp>
#include <Engine/EcsAudio.hpp>
#include <Engine/FixedTime.hpp>
#include <Engine/DebugDraw.hpp>
#include <Engine/DeviceSwitch.hpp>
#include <Engine/Transform.hpp>
#include <Engine/ModelImport.hpp>
#include <Engine/SpawnImported.hpp>
#include <Engine/EcsRendering.hpp>
#include <Engine/EcsUi.hpp>
#include <Engine/FrameTime.hpp>
#include <Engine/InputState.hpp>
#include <Engine/RenderTarget.hpp>
#include <Engine/TimeScale.hpp>
#include <Engine/UiTree.hpp>
#include <Engine/WindowRequests.hpp>
#include <Engine/WindowState.hpp>
#include <Core/Core.hpp>
#include <Ecs/System.hpp>
#include <Ecs/World.hpp>
#include <WindowManager/WindowManager.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/Renderer.hpp>

using std::optional;
using std::vector;

namespace SFT::Engine {

    /// How the application loop decides when to render.
    enum class FrameMode : u8 {
        /// Render every loop iteration, paced by the presentation settings (games, anything always animating).
        Continuous,
        /// Render only when something asks: window input/resize/close, `request_redraw()`, a due
        /// `request_redraw_after()`, or a device change. Otherwise the loop blocks in the OS event wait, so an idle app
        /// costs ~0% CPU and GPU. For tools, launchers and editors.
        OnDemand,
    };

    /// Shared redraw state. Copy a `RedrawHandle` to any thread (a download worker, a timer) to wake the UI.
    class RedrawHandle {
      public:
        RedrawHandle() = default;

        /// Render a frame as soon as possible. Safe from any thread.
        void request_redraw() const noexcept;
        /// Render a frame no later than `delay` from now (the earliest of all pending requests wins). Any thread.
        void request_redraw_after(std::chrono::nanoseconds delay) const noexcept;

        struct State {
            std::atomic<bool> requested{true};
            /// steady_clock nanoseconds of the earliest scheduled redraw; max = none.
            std::atomic<i64> deadline_ns{std::numeric_limits<i64>::max()};
            /// Wakes the blocked loop (installed by the Application).
            mutable Async::Mutex<std::function<void()>> wake;
        };
        explicit RedrawHandle(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
        [[nodiscard]] const std::shared_ptr<State> &state() const noexcept { return state_; }

      private:
        std::shared_ptr<State> state_;
    };

    struct EngineConfig {
        RHI::BackendType graphics_backend = RHI::BackendType::Vulkan;
        string graphics_physical_device_id;
        Core::RendererFeatureRequest features{};
        const char *app_name = "Sturdy Engine 5";


        std::filesystem::path shaders_directory = "Shaders";


        bool enable_shader_disk_cache = true;
    };


    class Engine {
      public:
        /// Constructs a `Engine` in its default state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        Engine();
        /// Destroys the `Engine` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~Engine();

        /// Disables this construction form for `Engine`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Engine(const Engine &) = delete;
        /// Assigns a new value to this `Engine`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Engine &operator=(const Engine &) = delete;


        /// Initializes the `Engine` for use.
        ///
        /// @param window Window used or affected by the operation.
        /// @param config Configuration values controlling the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        Core::RendererExpected<Core::RenderSurfaceHandle> initialize(WindowManager::Window &window,
                                                                     const EngineConfig &config = {});


        /// Adds window using the supplied arguments and current state.
        ///
        /// @param window Window used or affected by the operation.
        /// @param desired_frames_in_flight `desired_frames_in_flight` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        Core::RendererExpected<Core::RenderSurfaceHandle> add_window(WindowManager::Window &window,
                                                                     u32 desired_frames_in_flight = 2);


        /// Removes the window from its owning collection or system.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void remove_window(Core::RenderSurfaceHandle surface) noexcept;


        /// Creates a offscreen render target from the supplied parameters.
        ///
        /// @param description Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<RenderTargetHandle> create_offscreen_render_target(
            const OffscreenRenderTargetDescription &description);
        /// Destroys the offscreen render target identified by the supplied parameters.
        ///
        /// @param target `target` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_offscreen_render_target(RenderTargetHandle target) noexcept;
        /// Resizes an off-screen render target in place; see `Renderer::resize_offscreen_render_target`.
        [[nodiscard]] Core::RendererResult resize_offscreen_render_target(RenderTargetHandle target, u32 width, u32 height);
        /// Changes a target's size, format and/or sampling in place; the handle and its texture stay valid (materials and UI images using
        /// it keep working). A new size or format starts from an empty image; a sampling change keeps the pixels. Waits for the GPU unless
        /// only the label changed. On failure (unsupported format, zero size) the target is left as it was.
        [[nodiscard]] Core::RendererResult reconfigure_offscreen_render_target(RenderTargetHandle target,
                                                                               const OffscreenRenderTargetDescription &description);
        /// Writes a target with storage access (config `storage`) from a compute kernel (prepare one with `renderer()->prepare_compute_kernel`),
        /// rebuilds its mip chain, and waits for the GPU. The target is bound as a storage texture to the kernel resource named
        /// `target_binding`. See `Renderer::dispatch_into_offscreen_render_target`.
        [[nodiscard]] Core::RendererResult dispatch_into_offscreen_render_target(
            RenderTargetHandle target, SFT::Renderer::ComputeKernelId kernel, std::string_view target_binding, glm::uvec3 groups,
            std::span<const SFT::Renderer::ComputeBinding> bindings = {}, std::span<const std::byte> push_constants = {},
            std::span<const SFT::Renderer::ComputeBufferBinding> buffers = {});


        /// Performs the offscreen render target description operation for `Engine` using the supplied arguments.
        ///
        /// @param target `target` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        [[nodiscard]] optional<OffscreenRenderTargetDescription> offscreen_render_target_description(
            RenderTargetHandle target) const;
        /// What the target's request resolved to on the current device (format, size, mips, anisotropy, storage, MSAA) and any
        /// fallbacks made. Empty for an unknown or destroyed target.
        [[nodiscard]] optional<RenderTargetConfig> offscreen_render_target_config(RenderTargetHandle target) const;


        /// Performs the offscreen render target texture operation for `Engine` using the supplied arguments.
        ///
        /// @param target `target` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SFT::Renderer::TextureHandle offscreen_render_target_texture(
            RenderTargetHandle target) const noexcept;


        /// Recreates window using the supplied arguments and current state.
        ///
        /// @param old_surface Surface used or affected by the operation.
        /// @param new_window Window used or affected by the operation.
        /// @param desired_frames_in_flight `desired_frames_in_flight` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        Core::RendererExpected<Core::RenderSurfaceHandle> recreate_window(Core::RenderSurfaceHandle old_surface,
                                                                          WindowManager::Window &new_window,
                                                                          u32 desired_frames_in_flight = 2);


        /// Handles the surface resize needed event.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param extent `extent` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void on_surface_resize_needed(Core::RenderSurfaceHandle surface, Core::Extent2D extent) noexcept;


        /// Sets the presentation settings for this `Engine`.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param settings Configuration values controlling the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        Core::RendererResult set_presentation_settings(Core::RenderSurfaceHandle surface,
                                                       const Core::PresentationSettings &settings);

        /// Returns `surface`'s current presentation policy (vsync/HDR/transparent-composition/...).
        ///
        /// @param surface Surface to query.
        ///
        /// @return The value last accepted by set_presentation_settings() for this surface, or the
        ///         app-wide default it was created with if never overridden; default-constructed
        ///         Core::PresentationSettings{} if `surface` isn't registered. Lets a caller replacing
        ///         a window (e.g. Application::recreate_primary_window()) read the outgoing surface's
        ///         actual current policy forward onto the replacement.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Core::PresentationSettings presentation_settings(Core::RenderSurfaceHandle surface) const noexcept;

        /// Display-clock feedback for `surface` (refresh cycle, fixed/variable refresh, measured display
        /// intervals); all-unknown when the backend cannot report presentation timing.
        [[nodiscard]] Core::PresentTimingFeedback present_timing_feedback(Core::RenderSurfaceHandle surface) const noexcept;

        /// Applies runtime settings using the supplied arguments and current state.
        ///
        /// @param primary_surface Surface used or affected by the operation.
        /// @param settings Configuration values controlling the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<Core::RuntimeSettingsChangeResult>
        apply_runtime_settings(Core::RenderSurfaceHandle primary_surface,
                               const EngineConfig &settings);


        /// Queries HDR capabilities from the active backend or runtime state.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] RHI::RhiExpected<RHI::SurfaceHdrCapabilityQuery> query_hdr_capabilities(
            Core::RenderSurfaceHandle surface) const;


        /// Updates HDR content light level from the supplied values.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param update `update` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] RHI::RhiResult update_hdr_content_light_level(
            Core::RenderSurfaceHandle surface,
            const RHI::HdrContentLightLevelUpdate &update);


        /// Presents the completed frame to the target surface or swapchain.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::PresentationResolution presentation_resolution(Core::RenderSurfaceHandle surface) const noexcept;

        /// Renders the requested content using the current rendering state.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param frame `frame` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        Core::RendererResult render(Core::RenderSurfaceHandle surface, const Core::FrameInput &frame);


        /// Prepares render frame for a later operation.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param frame `frame` value used by the operation.
        /// @param parameters `parameters` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] PreparedRenderFrame prepare_render_frame(Core::RenderSurfaceHandle surface,
                                                               const Core::FrameInput &frame,
                                                               const RenderFrameParameters &parameters = {});
        /// Forgets the temporal camera history the engine keeps for `surface` (for a camera cut or
        /// teleport). Only relevant to frames prepared with `engine_managed_camera_history`.
        void reset_camera_history(Core::RenderSurfaceHandle surface);

        /// Renders the requested content using the current rendering state.
        ///
        /// @param frame `frame` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        Core::RendererResult render(const PreparedRenderFrame &frame);


        /// Updates the `Engine` state from the supplied values.
        ///
        /// @param delta_seconds `delta_seconds` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void update(f64 delta_seconds);

        /// Queues `action` to run at the start of the next `update`, on the thread that calls `update` and before the update schedule
        /// runs, so it can do what is forbidden while systems run (remove a system, replace a module). Callable from any thread,
        /// including from inside a system or a frame callback. Actions run in the order they were queued; one queued by another
        /// action runs in the following update.
        void defer_to_update_boundary(std::function<void()> action);

        /// Identifies an update hook.
        using UpdateHookId = u64;

        /// Registers `hook` to run at the start of every `update` -- after the queued boundary actions and after the clocks advanced, before
        /// transforms propagate and before the update schedule runs. At that moment nothing else touches the world, so a hook may spawn,
        /// destroy and change entities, and register or remove systems, directly. Hooks run in registration order on the thread that calls
        /// `update`; the argument is the scaled delta of this update. Callable from any thread, and from inside a hook (the new hook first
        /// runs in the following update).
        UpdateHookId add_update_hook(std::function<void(f64 delta_seconds)> hook);
        /// Removes a hook. Safe from inside the hook itself; a hook removed during an update does not run again. Returns false for an unknown id.
        bool remove_update_hook(UpdateHookId id);

        /// Recomputes every `WorldTransform` driven by a `Transform` (parents first). `update` and render extraction already
        /// do this; call it after moving things outside a frame (editor tools, tests) to see the result immediately.
        PropagationStats propagate_transforms();
        /// Draws lines, boxes, spheres, arrows... over the frame (see `Engine/DebugDraw.hpp`).
        [[nodiscard]] DebugDraw &debug_draw() noexcept;
        [[nodiscard]] const DebugDraw &debug_draw() const noexcept;
        /// Performs the queue window event operation for `Engine` using the supplied arguments.
        ///
        /// @param window Window used or affected by the operation.
        /// @param event Event used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void queue_window_event(WindowManager::WindowId window,
                                const WindowManager::WindowEvent &event);

        /// One step of a drag-and-drop onto a window, with the dropped path or text attached.
        struct DropEvent {
            enum class Kind : u8 { Begin, Hover, File, Text, Complete };
            WindowManager::WindowId window{};
            Kind kind = Kind::Begin;
            /// Pointer position in window coordinates (`Hover`; best effort for the others).
            WindowManager::WindowPosition position{};
            /// The file path (`File`) or text (`Text`).
            std::string data;
        };

        /// Called by the application for each drop-related window event; pairs `DropFile`/`DropText` with its string.
        void queue_drop_event(WindowManager::WindowId window, const WindowManager::WindowEvent &event);
        /// Drains the drag-and-drop steps since the last call, in order. Any thread.
        [[nodiscard]] std::vector<DropEvent> take_drop_events();

        /// Synthetic input: each of these builds a platform-shaped event and queues it through
        /// `queue_window_event`, so it takes exactly the path a real device event takes -- the ECS event
        /// channels (`KeyboardEvent`, `MouseMoveEvent`, ...), `InputState`, and the UI pointer/text state
        /// all see it on the next `update`. Use them for replay, tests, remote control and scripting.
        void inject_key_event(WindowManager::WindowId window, WindowManager::KeyboardKey key, bool pressed, u32 modifiers = 0, bool repeat = false);
        /// Injects committed text (what a text-input method delivers after composition).
        void inject_text_event(WindowManager::WindowId window, std::string_view utf8);
        /// Injects a pointer move to window-local (`x`, `y`) with the given held-button mask.
        void inject_mouse_move(WindowManager::WindowId window, f32 x, f32 y, f32 delta_x = 0.0F, f32 delta_y = 0.0F, u32 buttons = 0);
        /// Injects a mouse button press or release at window-local (`x`, `y`).
        void inject_mouse_button(WindowManager::WindowId window, WindowManager::MouseButton button, bool pressed, f32 x, f32 y, u8 clicks = 1);
        /// Injects a wheel scroll (`x`, `y` are the scroll deltas) with the pointer at (`mouse_x`, `mouse_y`).
        void inject_mouse_wheel(WindowManager::WindowId window, f32 x, f32 y, f32 mouse_x = 0.0F, f32 mouse_y = 0.0F);


        /// Returns the current or globally available ECS world value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Ecs::World &ecs_world() noexcept;
        /// Returns the current or globally available ECS world value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const Ecs::World &ecs_world() const noexcept;
        /// Updates schedule from the supplied values.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Ecs::Schedule &update_schedule() noexcept;
        /// Renders extraction schedule using the current rendering state.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Ecs::Schedule &render_extraction_schedule() noexcept;
        /// Renders frame requests using the current rendering state.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RenderFrameRequests &render_frame_requests() noexcept;

        /// The UI trees (entities with a `UiTree` component; see `Engine/UiTree.hpp`). `update` lays them out after the update
        /// schedule, `prepare_render_frame` attaches window trees to their window's frames, and `render_ui_tree_targets` renders
        /// off-screen trees into their targets.
        [[nodiscard]] UiTrees &ui_trees() noexcept { return ui_trees_; }
        /// Shorthand for spawning an entity whose only component is a `UiTree` built from `desc`.
        Ecs::Entity spawn_ui_tree(UiTreeDesc desc);
        /// Renders every off-screen UI tree that has a frame its target has not shown yet. The application loop calls this after
        /// `update`, on the thread that submits frames; an embedder driving frames itself calls it at the same point. Returns how many
        /// targets were rendered.
        usize render_ui_tree_targets();
        /// Returns the current or globally available light frame requests value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] LightFrameRequests &light_frame_requests() noexcept;
        /// Returns the current or globally available assets value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] AssetManager &assets() noexcept;
        /// Returns the current or globally available assets value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const AssetManager &assets() const noexcept;


        /// Returns the current or globally available window state value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowState &window_state() noexcept;
        /// The engine-owned dock coordinator: register each window's `DockWorkspace` with it and it
        /// handles panel transfers between windows and tear-off requests (spawning the OS window,
        /// then moving the panel only once the spawn succeeded). Owned here so embedders -- including
        /// foreign-language ones -- need not keep and wire up a private copy.
        ///
        /// @note Feed it `WindowRequestCompletion`s via `resolve_completion` and call
        ///       `request_empty_window_closes(window_requests())` each frame, as the UI workbench does.
        [[nodiscard]] DockWindowCoordinator &dock_coordinator() noexcept { return dock_coordinator_; }
        [[nodiscard]] const DockWindowCoordinator &dock_coordinator() const noexcept { return dock_coordinator_; }
        /// Returns the current or globally available window state value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const WindowState &window_state() const noexcept;


        /// Returns the current or globally available input state value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] InputState &input_state() noexcept;
        /// Returns the current or globally available input state value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const InputState &input_state() const noexcept;


        /// Returns the current or globally available window requests value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowRequests &window_requests() noexcept;


        /// Returns the current or globally available frame time value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const FrameTime &frame_time() const noexcept;
        /// The audio mixer, device sink and acoustics provider. Call `audio().enable(config)` to turn audio on.
        [[nodiscard]] AudioWorld &audio() noexcept { return audio_world_; }
        [[nodiscard]] const AudioWorld &audio() const noexcept { return audio_world_; }


        /// Returns the current or globally available time scale value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] TimeScale &time_scale() noexcept;
        /// Returns the current or globally available time scale value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const TimeScale &time_scale() const noexcept;






        /// Returns the current or globally available UI image cache value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] UiImageCache &ui_image_cache() noexcept;


        /// Returns the current or globally available UI svg cache value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] UiSvgCache &ui_svg_cache() noexcept;


        /// Returns the current or globally available primary window value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowManager::Window *primary_window() noexcept;


        /// Returns the OS-level handle of any live window the application manages, not just the
        /// primary one, so a secondary window can be embedded or targeted by platform APIs.
        ///
        /// @param window Window whose handle is wanted.
        ///
        /// @return Returns the handle, or `std::nullopt` when the window is unknown or the platform gave none.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::optional<WindowManager::NativeWindowHandle> native_window_handle(WindowManager::WindowId window) const noexcept;

        /// What `window` can do for the effect `kind` on this OS and compositor, route by route (for example which of
        /// ext-background-effect-v1, org_kde_kwin_blur and the X11 property a blur could use). Changes nothing; call it
        /// to decide what to offer in a settings screen before requesting the effect.
        [[nodiscard]] WindowManager::WindowEffectSupport window_effect_support(WindowManager::WindowId window,
                                                                               WindowManager::WindowEffectKind kind) const noexcept;

        /// Sets the primary window for this `Engine`.
        ///
        /// @param window Window used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_primary_window(WindowManager::Window &window) noexcept;

        /// Returns the current or globally available config value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const EngineConfig &config() const noexcept;
        /// Returns the current or globally available capabilities value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const Core::RendererCapabilities &capabilities() const noexcept;
        /// Renders the requested content using the current rendering state.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SFT::Renderer::Renderer *renderer() noexcept;
        /// Renders the requested content using the current rendering state.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const SFT::Renderer::Renderer *renderer() const noexcept;
        /// Returns the current or globally available graphics backend value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Core::EngineBackend *graphics_backend() noexcept;
        /// Returns the current or globally available RHI device value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::RhiDevice *rhi_device() noexcept;


        /// Returns the current GPU info.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        [[nodiscard]] optional<Core::GpuInfo> gpu_info() const;


        /// Returns the current or globally available GPU inventory value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const RHI::GpuInventory &gpu_inventory() const noexcept;

        /// The graphics APIs at least one discovered adapter can be driven through. Reads the startup inventory;
        /// creates no device.
        [[nodiscard]] vector<RHI::BackendType> available_backends() const;

        /// Every discovered GPU with its APIs, driver versions and stable ids, for a settings screen. Reads the
        /// startup inventory; creates no device. The running adapter's API is marked `active` and presentable.
        [[nodiscard]] vector<AdapterDescription> available_adapters();

        /// Replaces the graphics device (API and/or adapter) while the app keeps running, with no relaunch.
        ///
        /// What survives, re-created in place on the new device: meshes, textures, materials, offscreen render
        /// targets, window surfaces and swapchains, renderer pipeline caches, and (through `ScreenUi`) the UI
        /// renderer. Handles that stay valid: every renderer/asset handle the engine hands out, window ids and
        /// `RenderSurfaceHandle`s. What does not survive: GPU objects the caller created itself through the raw
        /// RHI (buffers, pipelines, ...); compare `device_generation()` or watch `take_device_change_events()`.
        ///
        /// A failed switch never leaves a dead window: the previous device is rebuilt (`rolled_back`) and the
        /// result is `Failed` with the reason. Must be called from the thread that owns frame submission, between
        /// frames (not from inside a render callback).
        [[nodiscard]] DeviceSwitchResult switch_device(const DeviceRequest &request);

        /// See `FrameMode`. Takes effect on the next loop iteration.
        void set_frame_mode(FrameMode mode) noexcept;
        [[nodiscard]] FrameMode frame_mode() const noexcept { return frame_mode_.load(std::memory_order_acquire); }
        /// Render a frame as soon as possible (`FrameMode::OnDemand`); a no-op in continuous mode. Any thread.
        void request_redraw() noexcept { redraw_.request_redraw(); }
        /// Render a frame within `delay` (an animation's next tick, a countdown); any thread.
        void request_redraw_after(std::chrono::nanoseconds delay) noexcept { redraw_.request_redraw_after(delay); }
        /// A copyable handle for other threads to wake the UI with.
        [[nodiscard]] RedrawHandle redraw_handle() const noexcept { return redraw_; }

        /// Number of times the graphics device has been replaced (a switch, a rollback, a device-loss recovery).
        [[nodiscard]] u64 device_generation() const noexcept { return renderer_.device_generation(); }

        /// What frame rendering has done since startup (window frames and offscreen views). The frame callback keeps
        /// running when a frame fails to render, so an app that just switched device compares two snapshots (or watches
        /// `consecutive_failures`) to learn that the switch "succeeded" but nothing is being drawn, and rolls back.
        struct RenderHealth {
            u64 frames_rendered = 0;
            u64 frames_failed = 0;
            /// Failures since the last successful frame; 0 means the latest frame rendered.
            u64 consecutive_failures = 0;
            /// Message of the most recent failure ("" until one happens).
            std::string last_error;
        };
        [[nodiscard]] RenderHealth render_health() const;
        /// Records one frame's outcome. Called by the application loop for window frames and by offscreen views; safe
        /// from any thread.
        void record_frame_result(const Core::RendererResult &result) noexcept;

        /// Drains the queue of device changes (switches, rollbacks, automatic device-loss recoveries) since the
        /// last call. Safe from any thread.
        [[nodiscard]] vector<DeviceChangeEvent> take_device_change_events();

        /// Registers an observer called for every device change, including `Switching` while the old device is
        /// still alive. Runs on the thread doing the change and must not call `switch_device`. Returns a token for
        /// `remove_device_observer`.
        usize add_device_observer(std::function<void(const DeviceChangeEvent &)> observer);
        void remove_device_observer(usize token);


        /// Returns the current or globally available shaders value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const vector<Core::Slang::UnCompiledShader> &shaders() const noexcept;


        /// Waits for idle to complete.
        ///
        /// @note This function does not throw exceptions.
        void wait_idle() noexcept;

      private:
        SFT::Renderer::Renderer renderer_;
        /// Declared after `renderer_` (which owns the device) so its pipelines are destroyed first.
        SFT::Renderer::DebugDrawRenderer debug_draw_renderer_;
        DebugDraw debug_draw_;
        RHI::GpuInventory gpu_inventory_{};
        AssetManager assets_{renderer_};
        Ecs::ComponentRegistry ecs_component_registry_;
        RenderFrameRequests render_frame_requests_{assets_};
        LightFrameRequests light_frame_requests_{};
        Ecs::World ecs_world_{ecs_component_registry_};
        PlatformEventInbox platform_event_inbox_{};
        Ecs::Events<WindowEvent> window_events_{};
        Ecs::Events<KeyboardEvent> keyboard_events_{};
        Ecs::Events<TextInputEvent> text_input_events_{};
        Ecs::Events<TextEditingEvent> text_editing_events_{};
        Ecs::Events<MouseMoveEvent> mouse_move_events_{};
        Ecs::Events<MouseButtonEvent> mouse_button_events_{};
        Ecs::Events<MouseWheelEvent> mouse_wheel_events_{};
        Ecs::Events<WindowStateEvent> window_state_events_{};
        Ecs::Events<AnimationEvent> animation_events_{};
        WindowState window_state_{};
        DockWindowCoordinator dock_coordinator_{};
        InputState input_state_{};
        WindowRequests window_requests_{};
        FrameTime frame_time_{};
        FixedTime fixed_time_{};
        AudioWorld audio_world_{};
        Ecs::Events<AudioSourceEvent> audio_events_{};
        TimeScale time_scale_{};
        UiImageCache ui_image_cache_{};
        UiSvgCache ui_svg_cache_{};
        Ecs::Schedule update_schedule_;
        UiTrees ui_trees_;
        /// Previous frame's view-projection per surface, for `engine_managed_camera_history`.
        struct DeviceObservers {
            std::vector<std::pair<usize, std::function<void(const DeviceChangeEvent &)>>> observers;
            std::vector<DeviceChangeEvent> pending;
            usize next_token = 1;
        };
        Async::Mutex<DeviceObservers> device_observers_;
        std::atomic<FrameMode> frame_mode_{FrameMode::Continuous};
        struct DropQueue {
            std::vector<DropEvent> events;
            /// Strings fetched from the window manager's drop store but not yet paired with their event.
            std::unordered_map<WindowManager::WindowId, std::deque<WindowManager::DroppedItem>> unpaired;
        };
        Async::Mutex<DropQueue> drops_;
        RedrawHandle redraw_{std::make_shared<RedrawHandle::State>()};
        void dispatch_device_change(const Core::DeviceChangeNotice &notice);
        Async::Mutex<std::vector<std::function<void()>>> deferred_actions_;
        struct UpdateHook {
            UpdateHookId id;
            std::function<void(f64)> fn;
        };
        Async::Mutex<std::vector<UpdateHook>> update_hooks_;
        UpdateHookId next_update_hook_id_ = 1;
        Async::Mutex<std::unordered_map<usize, glm::mat4>> camera_history_;
        /// Per surface: the temporal upscaler's jitter (scene UV shift) applied last frame.
        Async::Mutex<std::unordered_map<usize, glm::vec2>> jitter_history_;
        mutable Async::Mutex<RenderHealth> render_health_;
        Ecs::Schedule render_extraction_schedule_{Ecs::ScheduleConfig{.clear_events_on_run = false}};
        Core::RendererCapabilities capabilities_{};
        Core::Slang::ShaderCompiler shader_compiler_;
        vector<Core::Slang::UnCompiledShader> shaders_;
        EngineConfig config_{};
        WindowManager::Window *primary_window_ = nullptr;
        bool initialized_ = false;
    };

} // namespace SFT::Engine
