#include <Core/EngineBackend.hpp>

namespace SFT::Core {

/// Returns the current render threading capabilities.
///
/// @return Returns the current render threading capabilities value.
/// @note This function does not throw exceptions.
[[nodiscard]] RHI::RenderThreadingCapabilities EngineBackend::render_threading_capabilities() const noexcept {
            return RHI::RenderThreadingCapabilities{};
        }

/// Returns the current or globally available RHI device value.
///
/// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
/// @note Absence is represented by a null pointer rather than an exception.
/// @note This function does not throw exceptions.
[[nodiscard]] RHI::RhiDevice *EngineBackend::rhi_device() noexcept { return nullptr; }

/// Returns the current or globally available RHI device value.
///
/// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
/// @note Absence is represented by a null pointer rather than an exception.
/// @note This function does not throw exceptions.
[[nodiscard]] const RHI::RhiDevice *EngineBackend::rhi_device() const noexcept { return nullptr; }

/// Creates a engine backend from the supplied parameters.
///
/// @param backend Backend value to inspect, select, or convert.
///
/// @return Returns exclusive ownership of the created object; destroying or resetting the returned pointer releases it.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

/// Resolves the RHI surface associated with the supplied key, handle, or resource.
///
/// @param surface Surface used or affected by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::Unsupported`.
[[nodiscard]] RendererExpected<RHI::SurfaceHandle> EngineBackend::rhi_surface_for(RenderSurfaceHandle surface) {
            (void)surface;
            return graphics_backend_error(GraphicsBackendErrorCode::Unsupported,
                                          "This graphics backend does not expose RHI presentation surfaces.");
        }

} // namespace SFT::Core
