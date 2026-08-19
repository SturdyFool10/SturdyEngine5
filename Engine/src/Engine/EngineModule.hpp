#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <filesystem>
#include <optional>
#include <vector>
#pragma endregion

#include <Engine/AssetManager.hpp>
#include <Engine/EcsEvents.hpp>
#include <Engine/EcsRendering.hpp>
#include <Engine/EcsUi.hpp>
#include <Engine/FrameTime.hpp>
#include <Engine/InputState.hpp>
#include <Engine/RenderTarget.hpp>
#include <Engine/TimeScale.hpp>
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


        /// Performs the offscreen render target description operation for `Engine` using the supplied arguments.
        ///
        /// @param target `target` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        [[nodiscard]] optional<OffscreenRenderTargetDescription> offscreen_render_target_description(
            RenderTargetHandle target) const;


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
        /// Performs the queue window event operation for `Engine` using the supplied arguments.
        ///
        /// @param window Window used or affected by the operation.
        /// @param event Event used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void queue_window_event(WindowManager::WindowId window,
                                const WindowManager::WindowEvent &event);


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


        /// Returns the current or globally available UI context value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] UiContext &ui_context() noexcept;
        /// Returns the current or globally available UI pointer state value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] UiPointerState &ui_pointer_state() noexcept;
        /// Returns the current or globally available UI pointer state value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const UiPointerState &ui_pointer_state() const noexcept;


        /// Returns the current or globally available UI text input state value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] UiTextInputState &ui_text_input_state() noexcept;
        /// Returns the current or globally available UI text input state value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const UiTextInputState &ui_text_input_state() const noexcept;


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
        WindowState window_state_{};
        InputState input_state_{};
        WindowRequests window_requests_{};
        FrameTime frame_time_{};
        TimeScale time_scale_{};
        UiPointerState ui_pointer_state_{};
        UiTextInputState ui_text_input_state_{};
        UiContext ui_context_{};
        UiImageCache ui_image_cache_{};
        UiSvgCache ui_svg_cache_{};
        Ecs::Schedule update_schedule_;
        Ecs::Schedule render_extraction_schedule_{Ecs::ScheduleConfig{.clear_events_on_run = false}};
        Core::RendererCapabilities capabilities_{};
        Core::Slang::ShaderCompiler shader_compiler_;
        vector<Core::Slang::UnCompiledShader> shaders_;
        EngineConfig config_{};
        WindowManager::Window *primary_window_ = nullptr;
        bool initialized_ = false;
    };

} // namespace SFT::Engine
