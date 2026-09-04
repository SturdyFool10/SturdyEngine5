#include <Core/BackendInventory.hpp>

#if !defined(STURDY_PLATFORM_WEB)
#include <Core/Vulkan/VulkanInventory.hpp>
#endif
#if defined(STURDY_PLATFORM_WINDOWS)
#include <Core/D3D12/RHI/D3D12Adapter.hpp>
#endif
#if defined(STURDY_ENABLE_WEBGPU)
#include <Core/WebGPU/RHI/WebGpuAdapter.hpp>
#endif

namespace SFT::Core {

    RHI::GpuInventory enumerate_gpu_inventory(const RHI::InstanceDesc &instance_desc) {
        RHI::BackendRegistry backends;
#if !defined(STURDY_PLATFORM_WEB)
        backends.register_backend(Vulkan::vulkan_inventory_backend_registration());
#endif
#if defined(STURDY_PLATFORM_WINDOWS)
        backends.register_backend(::SFT::D3D12::d3d12_backend_registration());
#endif
#if defined(STURDY_ENABLE_WEBGPU)
        // Registered last, and last in default_backend_priority too: WebGPU here is a second path
        // onto the same drivers the native backends already use, so it is only selected when asked
        // for explicitly or when nothing else is available.
        backends.register_backend(WebGpu::webgpu_backend_registration());
#endif
        return RHI::enumerate_gpu_inventory(backends, instance_desc);
    }

} // namespace SFT::Core
