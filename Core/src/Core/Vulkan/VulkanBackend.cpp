#include "VulkanBackend.hpp"

namespace SFT::Core::Vulkan {

[[nodiscard]] bool VulkanBackend::hdr_swapchain_colorspace_enabled() const noexcept { return hdr_swapchain_colorspace_enabled_; }

[[nodiscard]] bool VulkanBackend::hdr_metadata_enabled() const noexcept { return hdr_metadata_enabled_; }

} // namespace SFT::Core::Vulkan
