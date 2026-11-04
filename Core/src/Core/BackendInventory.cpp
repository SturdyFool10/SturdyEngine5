#include <Core/BackendInventory.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <Foundation/Foundation.hpp>

#include <Core/Slang/Shader.hpp>

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
        RHI::GpuInventory inventory = RHI::enumerate_gpu_inventory(backends, instance_desc);
#if defined(STURDY_PLATFORM_WINDOWS)
        // D3D12 takes DXIL, which Slang can only make with DXC beside the program. Without it every shader fails to build and the window
        // never draws, so D3D12 is not offered at all: an application asking for it falls back to another API (Vulkan) instead.
        if (!SFT::Core::Slang::dxil_generation_available()) {
            bool removed = false;
            for (RHI::PhysicalGpu &gpu : inventory.gpus) {
                removed |= std::erase_if(gpu.api_support, [](const RHI::GpuApiSupport &support) {
                               return support.adapter.backend == RHI::BackendType::D3D12;
                           }) > 0;
            }
            std::erase_if(inventory.gpus, [](const RHI::PhysicalGpu &gpu) { return gpu.api_support.empty(); });
            if (removed) {
                const std::string reason =
                    "D3D12 is not offered: shaders cannot be compiled to DXIL here (dxcompiler.dll and dxil.dll are missing).";
                Foundation::log_warn("{}", reason);
                inventory.failures.push_back(RHI::BackendDiscoveryFailure{.backend = RHI::BackendType::D3D12, .message = reason});
            }
        }
#endif
        return inventory;
    }

} // namespace SFT::Core
