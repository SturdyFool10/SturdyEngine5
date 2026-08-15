#include "EngineBackend.hpp"

namespace SFT::Core {

[[nodiscard]] RHI::RenderThreadingCapabilities EngineBackend::render_threading_capabilities() const noexcept {
            return RHI::RenderThreadingCapabilities{};
        }

[[nodiscard]] RHI::RhiDevice *EngineBackend::rhi_device() noexcept { return nullptr; }

[[nodiscard]] const RHI::RhiDevice *EngineBackend::rhi_device() const noexcept { return nullptr; }

unique_ptr<EngineBackend> create_engine_backend(RHI::BackendType backend) {
            switch (backend) {
                case RHI::BackendType::Vulkan: return create_vulkan_backend();
#if defined(_WIN32)
                case RHI::BackendType::D3D12: return create_d3d12_backend();
#endif
                case RHI::BackendType::Metal:
                case RHI::BackendType::WebGpu: return nullptr;
            }
            return nullptr;
        }

[[nodiscard]] RendererExpected<RHI::SurfaceHandle> EngineBackend::rhi_surface_for(RenderSurfaceHandle surface) {
            (void)surface;
            return graphics_backend_error(GraphicsBackendErrorCode::Unsupported,
                                          "This graphics backend does not expose RHI presentation surfaces.");
        }

} // namespace SFT::Core
