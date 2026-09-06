#pragma once

#include <Foundation/Foundation.hpp>

#include <webgpu/webgpu.h>

#include <RHI/RHI.hpp>

namespace SFT::Core::WebGpu {

    /// The raw-handle escape hatch for the WebGPU backend, matching the shape of
    /// `Vulkan::VulkanNativeAccessExtension`/`D3D12::D3D12NativeAccessExtension`: a caller that needs
    /// to do something the RHI has no concept of can take the backend's own objects and go direct.
    ///
    /// Unlike the Vulkan/D3D12 hatches, this deliberately stops at the *WebGPU* objects (`WGPUInstance`/
    /// `WGPUAdapter`/`WGPUDevice`/`WGPUQueue`) rather than reaching beneath them to whatever native API
    /// Dawn chose (Vulkan/Metal/D3D12) -- that reach-through is fundamentally unavailable on Web (a
    /// browser's WebGPU implementation exposes no native handles to any embedder, sandboxed or not),
    /// and even on native Dawn it would defeat the entire point of choosing the portable backend. What
    /// *is* portable across both native Dawn and Emscripten's in-browser port is the WebGPU C API
    /// itself, which is exactly what this hands out -- e.g. to interoperate with another library built
    /// against the same `webgpu.h`, or to pair with `Sturdy::Web`'s JS bridge on Web specifically.
    ///
    /// WebGPU has exactly one queue (see the "Notes" section of the WebGPU backend's own design
    /// writeup: no semaphores/fences/barriers, one in-order queue, dependencies inferred), so unlike
    /// the Vulkan/D3D12 hatches there is no per-lane lookup to model here.
    class WebGpuNativeAccessExtension final : public RHI::RhiDeviceExtension {
      public:
        /// Returns the current or globally available ID value.
        ///
        /// @return Returns the current ID value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr RHI::ExtensionId id() noexcept {
            return RHI::ExtensionId{"sturdy", "webgpu-native-access", 1};
        }

        /// Constructs a `WebGpuNativeAccessExtension` from the supplied initialization values.
        ///
        /// @param instance `instance` value used by the operation.
        /// @param adapter `adapter` value used by the operation.
        /// @param device `device` value used by the operation.
        /// @param queue `queue` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        WebGpuNativeAccessExtension(WGPUInstance instance, WGPUAdapter adapter, WGPUDevice device,
                                    WGPUQueue queue) noexcept;

        /// Returns the current or globally available extension ID value.
        ///
        /// @return Returns the current extension ID value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::ExtensionId extension_id() const noexcept override;

        /// Returns the current or globally available native instance value.
        ///
        /// @return Returns the current native instance value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPUInstance native_instance() const noexcept;
        /// Returns the current or globally available native adapter value.
        ///
        /// @return Returns the current native adapter value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPUAdapter native_adapter() const noexcept;
        /// Returns the current or globally available native device value.
        ///
        /// @return Returns the current native device value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPUDevice native_device() const noexcept;
        /// Returns the current or globally available native queue value.
        ///
        /// @return Returns the current native queue value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPUQueue native_queue() const noexcept;

      private:
        WGPUInstance instance_ = nullptr;
        WGPUAdapter adapter_ = nullptr;
        WGPUDevice device_ = nullptr;
        WGPUQueue queue_ = nullptr;
    };

} // namespace SFT::Core::WebGpu
