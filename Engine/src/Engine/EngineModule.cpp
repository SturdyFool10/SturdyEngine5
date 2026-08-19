#include <Engine/EngineModule.hpp>

namespace SFT::Engine {

    /// Returns the current or globally available config value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const EngineConfig &Engine::config() const noexcept { return config_; }

    /// Returns the current or globally available capabilities value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const Core::RendererCapabilities &Engine::capabilities() const noexcept { return capabilities_; }

    /// Renders the requested content using the current rendering state.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SFT::Renderer::Renderer *Engine::renderer() noexcept { return &renderer_; }

    /// Renders the requested content using the current rendering state.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const SFT::Renderer::Renderer *Engine::renderer() const noexcept { return &renderer_; }

    /// Returns the current or globally available ECS world value.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] Ecs::World &Engine::ecs_world() noexcept { return ecs_world_; }

    /// Returns the current or globally available ECS world value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const Ecs::World &Engine::ecs_world() const noexcept { return ecs_world_; }

    /// Performs the queue window event operation for `Engine` using the supplied arguments.
    ///
    /// @param window Window used or affected by the operation.
    /// @param event Event used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Engine::queue_window_event(WindowManager::WindowId window,
                                    const WindowManager::WindowEvent &event) {
        platform_event_inbox_.push(window, event);
    }

    /// Updates the `Engine` state from the supplied values.
    ///
    /// @param delta_seconds `delta_seconds` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Engine::update(f64 delta_seconds) {
        frame_time_.advance(delta_seconds, time_scale_.value());
        update_schedule_.run(ecs_world_);
    }

    /// Returns the current or globally available frame time value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const FrameTime &Engine::frame_time() const noexcept { return frame_time_; }

    /// Returns the current or globally available time scale value.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] TimeScale &Engine::time_scale() noexcept { return time_scale_; }

    /// Returns the current or globally available time scale value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const TimeScale &Engine::time_scale() const noexcept { return time_scale_; }

    /// Updates schedule from the supplied values.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] Ecs::Schedule &Engine::update_schedule() noexcept { return update_schedule_; }

    /// Renders extraction schedule using the current rendering state.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] Ecs::Schedule &Engine::render_extraction_schedule() noexcept { return render_extraction_schedule_; }

    /// Renders frame requests using the current rendering state.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] RenderFrameRequests &Engine::render_frame_requests() noexcept { return render_frame_requests_; }

    /// Returns the current or globally available light frame requests value.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] LightFrameRequests &Engine::light_frame_requests() noexcept { return light_frame_requests_; }

    /// Returns the current or globally available assets value.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] AssetManager &Engine::assets() noexcept { return assets_; }

    /// Returns the current or globally available assets value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const AssetManager &Engine::assets() const noexcept { return assets_; }

    /// Returns the current or globally available window state value.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WindowState &Engine::window_state() noexcept { return window_state_; }

    /// Returns the current or globally available window state value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const WindowState &Engine::window_state() const noexcept { return window_state_; }

    /// Returns the current or globally available input state value.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] InputState &Engine::input_state() noexcept { return input_state_; }

    /// Returns the current or globally available input state value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const InputState &Engine::input_state() const noexcept { return input_state_; }

    /// Returns the current or globally available window requests value.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WindowRequests &Engine::window_requests() noexcept { return window_requests_; }

    /// Returns the current or globally available UI context value.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] UiContext &Engine::ui_context() noexcept { return ui_context_; }

    /// Returns the current or globally available UI pointer state value.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] UiPointerState &Engine::ui_pointer_state() noexcept { return ui_pointer_state_; }
    /// Returns the current or globally available UI pointer state value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const UiPointerState &Engine::ui_pointer_state() const noexcept { return ui_pointer_state_; }
    /// Returns the current or globally available UI text input state value.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] UiTextInputState &Engine::ui_text_input_state() noexcept { return ui_text_input_state_; }
    /// Returns the current or globally available UI text input state value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const UiTextInputState &Engine::ui_text_input_state() const noexcept { return ui_text_input_state_; }
    /// Returns the current or globally available UI image cache value.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] UiImageCache &Engine::ui_image_cache() noexcept { return ui_image_cache_; }
    /// Returns the current or globally available UI svg cache value.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] UiSvgCache &Engine::ui_svg_cache() noexcept { return ui_svg_cache_; }

    /// Returns the current or globally available primary window value.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WindowManager::Window *Engine::primary_window() noexcept { return primary_window_; }

    /// Sets the primary window for this `Engine`.
    ///
    /// @param window Window used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Engine::set_primary_window(WindowManager::Window &window) noexcept { primary_window_ = &window; }

    /// Returns the current or globally available shaders value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const vector<Core::Slang::UnCompiledShader> &Engine::shaders() const noexcept { return shaders_; }

} // namespace SFT::Engine
