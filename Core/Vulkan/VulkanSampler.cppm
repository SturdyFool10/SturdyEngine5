module;
#include <Foundation/Foundation.hpp>
#pragma region Imports
#include "volk.h"
#pragma endregion

export module Sturdy.Core:VulkanSampler;

import :GraphicsBackendError;

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;

export namespace SFT::Core::Vulkan {

    class VulkanSampler {
      public:
        VulkanSampler() = default;
        ~VulkanSampler() { destroy(); }

        VulkanSampler(const VulkanSampler &) = delete;
        VulkanSampler &operator=(const VulkanSampler &) = delete;

        VulkanSampler(VulkanSampler &&o) noexcept : device_(o.device_), sampler_(o.sampler_) {
            o.device_ = VK_NULL_HANDLE;
            o.sampler_ = VK_NULL_HANDLE;
        }
        VulkanSampler &operator=(VulkanSampler &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                sampler_ = o.sampler_;
                o.device_ = VK_NULL_HANDLE;
                o.sampler_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        [[nodiscard]] static RendererExpected<VulkanSampler> create(
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

        [[nodiscard]] VkSampler vk_handle() const noexcept { return sampler_; }
        [[nodiscard]] bool is_valid() const noexcept { return sampler_ != VK_NULL_HANDLE; }

        void destroy() noexcept {
            if (sampler_ == VK_NULL_HANDLE)
                return;
            vkDestroySampler(device_, sampler_, nullptr);
            sampler_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkSampler sampler_ = VK_NULL_HANDLE;
    };

} // namespace SFT::Core::Vulkan
