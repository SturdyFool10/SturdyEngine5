#include "VulkanResourceStates.hpp"

namespace SFT::Core::Vulkan {

[[nodiscard]] VkImageSubresourceRange VulkanImageSubresourceRange::to_vk() const noexcept {
            return VkImageSubresourceRange{
                .aspectMask = aspects,
                .baseMipLevel = base_mip,
                .levelCount = mip_count,
                .baseArrayLayer = base_layer,
                .layerCount = layer_count,
            };
        }

} // namespace SFT::Core::Vulkan
