#include <Core/Vulkan/VulkanResourceStates.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Converts the supplied engine/RHI value to its Vulkan representation.
///
/// @return Returns the current to Vulkan value.
/// @note This function does not throw exceptions.
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
