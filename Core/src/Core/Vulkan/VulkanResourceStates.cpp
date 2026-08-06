#include "VulkanResourceStates.hpp"

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

[[nodiscard]] VkImageSubresourceRange VulkanImageSubresourceRange::to_vk() const noexcept {
            ZoneScopedN("VulkanImageSubresourceRange::to_vk");
            return VkImageSubresourceRange{
                .aspectMask = aspects,
                .baseMipLevel = base_mip,
                .levelCount = mip_count,
                .baseArrayLayer = base_layer,
                .layerCount = layer_count,
            };
        }

} // namespace SFT::Core::Vulkan
