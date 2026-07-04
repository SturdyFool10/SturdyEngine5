module;
#pragma region Imports
#include "volk.h"
#pragma endregion

export module Sturdy.Core:VulkanConstants;

#pragma region Imports
import Sturdy.Foundation;
#pragma endregion

export namespace SFT::Core::Vulkan {

    inline constexpr u32 VULKAN_API_VERSION = VK_API_VERSION_1_4;
    inline constexpr VkFormat SWAPCHAIN_FORMAT = VK_FORMAT_B8G8R8A8_SRGB;
    inline constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

    // Frame-pacing default: a request of 0 means "backend's choice", which is double buffering.
    inline constexpr u32 DEFAULT_FRAMES_IN_FLIGHT = 2;

    [[nodiscard]] constexpr u32 sanitize_frames_in_flight(u32 requested) noexcept {
        return requested == 0 ? DEFAULT_FRAMES_IN_FLIGHT : requested;
    }

} // namespace SFT::Core::Vulkan
