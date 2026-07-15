#include "EngineBackend.hpp"

namespace SFT::Core {

[[nodiscard]] RHI::RenderThreadingCapabilities EngineBackend::render_threading_capabilities() const noexcept {
            return RHI::RenderThreadingCapabilities{};
        }

[[nodiscard]] RHI::RhiDevice *EngineBackend::rhi_device() noexcept { return nullptr; }

[[nodiscard]] const RHI::RhiDevice *EngineBackend::rhi_device() const noexcept { return nullptr; }

[[nodiscard]] RendererExpected<RHI::SurfaceHandle> EngineBackend::rhi_surface_for(RenderSurfaceHandle surface) {
            (void)surface;
            return graphics_backend_error(GraphicsBackendErrorCode::Unsupported,
                                          "This graphics backend does not expose RHI presentation surfaces.");
        }

} // namespace SFT::Core
