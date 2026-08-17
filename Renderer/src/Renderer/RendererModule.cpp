#include <Renderer/src/Renderer/RendererModule.hpp>
#include "RendererModule.hpp"

namespace SFT::Renderer {

/// Returns the current or globally available capabilities value.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const Core::RendererCapabilities &Renderer::capabilities() const noexcept { return capabilities_; }

/// Returns the current or globally available graphics backend value.
///
/// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
/// @note This function does not throw exceptions.
[[nodiscard]] Core::EngineBackend *Renderer::graphics_backend() noexcept { return graphics_backend_.get(); }

/// Returns the current or globally available graphics backend value.
///
/// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
/// @note This function does not throw exceptions.
[[nodiscard]] const Core::EngineBackend *Renderer::graphics_backend() const noexcept { return graphics_backend_.get(); }

} // namespace SFT::Renderer

namespace SFT::Renderer {

    /// Resolves the presentation coordinator associated with the supplied key, handle, or resource.
    ///
    /// @param present_via_compute `present_via_compute` value used by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    PresentationCoordinator &Renderer::presentation_coordinator_for(bool present_via_compute) noexcept {
        return present_via_compute ? compute_presentation_coordinator_ : graphics_presentation_coordinator_;
    }

} // namespace SFT::Renderer

