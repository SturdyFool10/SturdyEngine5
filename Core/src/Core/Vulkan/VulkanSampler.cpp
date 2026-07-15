#include "VulkanSampler.hpp"

namespace SFT::Core::Vulkan {

VulkanSampler::~VulkanSampler() { destroy(); }

VulkanSampler::VulkanSampler(VulkanSampler &&o) noexcept : device_(o.device_), sampler_(o.sampler_) {
            o.device_ = VK_NULL_HANDLE;
            o.sampler_ = VK_NULL_HANDLE;
        }

VulkanSampler &VulkanSampler::operator=(VulkanSampler &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                sampler_ = o.sampler_;
                o.device_ = VK_NULL_HANDLE;
                o.sampler_ = VK_NULL_HANDLE;
            }
            return *this;
        }

[[nodiscard]] RendererExpected<VulkanSampler> VulkanSampler::create(
            VkDevice device,
            const VkSamplerCreateInfo &info) noexcept {
            VkSampler sampler = VK_NULL_HANDLE;
            if (vkCreateSampler(device, &info, nullptr, &sampler) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateSampler failed.");
            VulkanSampler out;
            out.device_ = device;
            out.sampler_ = sampler;
            return out;
        }

[[nodiscard]] VkSampler VulkanSampler::vk_handle() const noexcept { return sampler_; }

[[nodiscard]] bool VulkanSampler::is_valid() const noexcept { return sampler_ != VK_NULL_HANDLE; }

void VulkanSampler::destroy() noexcept {
            if (sampler_ == VK_NULL_HANDLE)
                return;
            vkDestroySampler(device_, sampler_, nullptr);
            sampler_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

} // namespace SFT::Core::Vulkan
