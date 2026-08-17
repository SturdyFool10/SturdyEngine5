#include <Core/src/Core/Vulkan/Rhi/VulkanRhiBridge.hpp>


namespace SFT::Core::Vulkan {

    bool VulkanRhiDeviceBridge::SwapchainRecord::is_composition_present() const noexcept {
        return composition.presenter != nullptr;
    }

} // namespace SFT::Core::Vulkan

