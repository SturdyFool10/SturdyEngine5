#include <Core/Vulkan/VulkanBackend.hpp>

namespace SFT::Core::Vulkan {

/// Returns the current or globally available HDR swapchain colorspace enabled value.
///
/// @return Returns the current HDR swapchain colorspace enabled value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanBackend::hdr_swapchain_colorspace_enabled() const noexcept { return hdr_swapchain_colorspace_enabled_; }

/// Returns the current or globally available HDR metadata enabled value.
///
/// @return Returns the current HDR metadata enabled value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanBackend::hdr_metadata_enabled() const noexcept { return hdr_metadata_enabled_; }

} // namespace SFT::Core::Vulkan
