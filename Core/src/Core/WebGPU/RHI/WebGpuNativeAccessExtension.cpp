#include <Core/WebGPU/RHI/WebGpuNativeAccessExtension.hpp>

namespace SFT::Core::WebGpu {

    WebGpuNativeAccessExtension::WebGpuNativeAccessExtension(WGPUInstance instance, WGPUAdapter adapter,
                                                              WGPUDevice device, WGPUQueue queue) noexcept
        : instance_(instance), adapter_(adapter), device_(device), queue_(queue) {}

    RHI::ExtensionId WebGpuNativeAccessExtension::extension_id() const noexcept { return id(); }

    WGPUInstance WebGpuNativeAccessExtension::native_instance() const noexcept { return instance_; }

    WGPUAdapter WebGpuNativeAccessExtension::native_adapter() const noexcept { return adapter_; }

    WGPUDevice WebGpuNativeAccessExtension::native_device() const noexcept { return device_; }

    WGPUQueue WebGpuNativeAccessExtension::native_queue() const noexcept { return queue_; }

} // namespace SFT::Core::WebGpu
