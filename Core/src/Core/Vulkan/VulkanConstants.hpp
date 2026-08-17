#pragma once

#include <Foundation/src/Foundation.hpp>
#pragma region Imports
#include "volk.h"
#pragma endregion

namespace SFT::Core::Vulkan {

    inline constexpr u32 VULKAN_API_VERSION = VK_API_VERSION_1_4;
    inline constexpr VkFormat SWAPCHAIN_FORMAT = VK_FORMAT_B8G8R8A8_SRGB;
    inline constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;


    inline constexpr u32 DEFAULT_FRAMES_IN_FLIGHT = 2;


    inline constexpr const char *PORTABILITY_SUBSET_EXTENSION_NAME = "VK_KHR_portability_subset";

    /// Performs the sanitize frames in flight operation using the supplied arguments.
    ///
    /// @param requested `requested` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr u32 sanitize_frames_in_flight(u32 requested) noexcept {
        return requested == 0 ? DEFAULT_FRAMES_IN_FLIGHT : requested;
    }

} // namespace SFT::Core::Vulkan
