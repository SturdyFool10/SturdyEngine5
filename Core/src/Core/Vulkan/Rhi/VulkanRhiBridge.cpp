#include <Core/src/Core/Vulkan/Rhi/VulkanRhiBridge.hpp>


namespace SFT::Core::Vulkan {

    /// Reports whether composition present holds for this `Vulkan`.
    ///
    /// @return Returns the current is composition present value.
    /// @note This function does not throw exceptions.
    bool VulkanRhiDeviceBridge::SwapchainRecord::is_composition_present() const noexcept {
        return composition.presenter != nullptr;
    }

} // namespace SFT::Core::Vulkan

