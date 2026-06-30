module;
#include "volk.h"

export module Sturdy.Core:VulkanConstants;

import Sturdy.Foundation;

export namespace SFT::Core::Vulkan {

    inline constexpr u32 VULKAN_API_VERSION = VK_API_VERSION_1_4;
    inline constexpr VkFormat SWAPCHAIN_FORMAT = VK_FORMAT_B8G8R8A8_SRGB;
    inline constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

} // namespace SFT::Core::Vulkan
