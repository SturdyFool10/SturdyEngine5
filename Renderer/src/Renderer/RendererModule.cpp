#include <Renderer/src/Renderer/RendererModule.hpp>
#include "RendererModule.hpp"

namespace SFT::Renderer {

[[nodiscard]] const Core::RendererCapabilities &Renderer::capabilities() const noexcept { return capabilities_; }

[[nodiscard]] Core::EngineBackend *Renderer::graphics_backend() noexcept { return graphics_backend_.get(); }

[[nodiscard]] const Core::EngineBackend *Renderer::graphics_backend() const noexcept { return graphics_backend_.get(); }

} // namespace SFT::Renderer

namespace SFT::Renderer {

    PresentationCoordinator &Renderer::presentation_coordinator_for(bool present_via_compute) noexcept {
        return present_via_compute ? compute_presentation_coordinator_ : graphics_presentation_coordinator_;
    }

} // namespace SFT::Renderer

