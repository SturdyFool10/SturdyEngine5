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

    // VK_KHR_portability_subset's extension-name macro lives behind the VK_ENABLE_BETA_EXTENSIONS
    // guard in the Vulkan headers (vulkan_beta.h), and pulling that macro in would also unlock
    // unrelated in-development vendor extensions (AMDX, NV). The extension itself is not
    // vendor-specific — it's how MoltenVK and other non-conformant implementations report which
    // core features they can't fully provide — so the literal name is used directly instead.
    inline constexpr const char *PORTABILITY_SUBSET_EXTENSION_NAME = "VK_KHR_portability_subset";

    [[nodiscard]] constexpr u32 sanitize_frames_in_flight(u32 requested) noexcept {
        return requested == 0 ? DEFAULT_FRAMES_IN_FLIGHT : requested;
    }

} // namespace SFT::Core::Vulkan
