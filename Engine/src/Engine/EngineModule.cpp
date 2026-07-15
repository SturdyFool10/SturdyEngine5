#include "EngineModule.hpp"

namespace SFT::Engine {

[[nodiscard]] const Core::RendererCapabilities &Engine::capabilities() const noexcept { return capabilities_; }

[[nodiscard]] SFT::Renderer::Renderer *Engine::renderer() noexcept { return &renderer_; }

[[nodiscard]] const SFT::Renderer::Renderer *Engine::renderer() const noexcept { return &renderer_; }

[[nodiscard]] const vector<Core::Slang::UnCompiledShader> &Engine::shaders() const noexcept { return shaders_; }

} // namespace SFT::Engine
