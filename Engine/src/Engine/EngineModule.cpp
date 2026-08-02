#include "EngineModule.hpp"

namespace SFT::Engine {

    [[nodiscard]] const EngineConfig &Engine::config() const noexcept { return config_; }

    [[nodiscard]] const Core::RendererCapabilities &Engine::capabilities() const noexcept { return capabilities_; }

    [[nodiscard]] SFT::Renderer::Renderer *Engine::renderer() noexcept { return &renderer_; }

    [[nodiscard]] const SFT::Renderer::Renderer *Engine::renderer() const noexcept { return &renderer_; }

    [[nodiscard]] Ecs::World &Engine::ecs_world() noexcept { return ecs_world_; }

    [[nodiscard]] const Ecs::World &Engine::ecs_world() const noexcept { return ecs_world_; }

    void Engine::queue_window_event(Platform::Windowing::WindowId window,
                                    const Platform::Windowing::WindowEvent &event) {
        platform_event_inbox_.push(window, event);
    }

    void Engine::update(f64 delta_seconds) {
        frame_time_.advance(delta_seconds, time_scale_.value());
        update_schedule_.run(ecs_world_);
    }

    [[nodiscard]] const FrameTime &Engine::frame_time() const noexcept { return frame_time_; }

    [[nodiscard]] TimeScale &Engine::time_scale() noexcept { return time_scale_; }

    [[nodiscard]] const TimeScale &Engine::time_scale() const noexcept { return time_scale_; }

    [[nodiscard]] Ecs::Schedule &Engine::update_schedule() noexcept { return update_schedule_; }

    [[nodiscard]] Ecs::Schedule &Engine::render_extraction_schedule() noexcept { return render_extraction_schedule_; }

    [[nodiscard]] RenderFrameRequests &Engine::render_frame_requests() noexcept { return render_frame_requests_; }

    [[nodiscard]] LightFrameRequests &Engine::light_frame_requests() noexcept { return light_frame_requests_; }

    [[nodiscard]] AssetManager &Engine::assets() noexcept { return assets_; }

    [[nodiscard]] const AssetManager &Engine::assets() const noexcept { return assets_; }

    [[nodiscard]] WindowState &Engine::window_state() noexcept { return window_state_; }

    [[nodiscard]] const WindowState &Engine::window_state() const noexcept { return window_state_; }

    [[nodiscard]] WindowRequests &Engine::window_requests() noexcept { return window_requests_; }

    [[nodiscard]] UiContext &Engine::ui_context() noexcept { return ui_context_; }

    [[nodiscard]] UiPointerState &Engine::ui_pointer_state() noexcept { return ui_pointer_state_; }
    [[nodiscard]] const UiPointerState &Engine::ui_pointer_state() const noexcept { return ui_pointer_state_; }
    [[nodiscard]] UiImageCache &Engine::ui_image_cache() noexcept { return ui_image_cache_; }
    [[nodiscard]] UiSvgCache &Engine::ui_svg_cache() noexcept { return ui_svg_cache_; }

    [[nodiscard]] Platform::Windowing::Window *Engine::primary_window() noexcept { return primary_window_; }

    [[nodiscard]] const vector<Core::Slang::UnCompiledShader> &Engine::shaders() const noexcept { return shaders_; }

} // namespace SFT::Engine
