#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#include "volk.h"
#pragma endregion

#include <Core/GraphicsBackendError.hpp>

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;

namespace SFT::Core::Vulkan {

    class VulkanSampler {
      public:
        VulkanSampler() = default;
        ~VulkanSampler();

        VulkanSampler(const VulkanSampler &) = delete;
        VulkanSampler &operator=(const VulkanSampler &) = delete;

        VulkanSampler(VulkanSampler &&o) noexcept;
        VulkanSampler &operator=(VulkanSampler &&o) noexcept;

        [[nodiscard]] static RendererExpected<VulkanSampler> create(
            VkDevice device,
            const VkSamplerCreateInfo &info) noexcept;

        [[nodiscard]] VkSampler vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;

        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkSampler sampler_ = VK_NULL_HANDLE;
    };

} // namespace SFT::Core::Vulkan
