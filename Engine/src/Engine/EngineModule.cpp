#include "EngineModule.hpp"

namespace SFT::Engine {

    [[nodiscard]] const Core::RendererCapabilities &Engine::capabilities() const noexcept { return capabilities_; }

    [[nodiscard]] SFT::Renderer::Renderer *Engine::renderer() noexcept { return &renderer_; }

    [[nodiscard]] const SFT::Renderer::Renderer *Engine::renderer() const noexcept { return &renderer_; }

    [[nodiscard]] Ecs::World &Engine::ecs_world() noexcept { return ecs_world_; }

    [[nodiscard]] const Ecs::World &Engine::ecs_world() const noexcept { return ecs_world_; }

    [[nodiscard]] Ecs::Schedule &Engine::render_extraction_schedule() noexcept { return render_extraction_schedule_; }

    [[nodiscard]] RenderFrameRequests &Engine::render_frame_requests() noexcept { return render_frame_requests_; }

    [[nodiscard]] AssetManager &Engine::assets() noexcept { return assets_; }

    [[nodiscard]] const AssetManager &Engine::assets() const noexcept { return assets_; }

    [[nodiscard]] const vector<Core::Slang::UnCompiledShader> &Engine::shaders() const noexcept { return shaders_; }

} // namespace SFT::Engine
