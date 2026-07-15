#include "RendererModule.hpp"

namespace SFT::Renderer {

[[nodiscard]] const Core::RendererCapabilities &Renderer::capabilities() const noexcept { return capabilities_; }

[[nodiscard]] Core::EngineBackend *Renderer::graphics_backend() noexcept { return graphics_backend_.get(); }

[[nodiscard]] const Core::EngineBackend *Renderer::graphics_backend() const noexcept { return graphics_backend_.get(); }

} // namespace SFT::Renderer
