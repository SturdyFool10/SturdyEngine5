module;
#include "volk.h"
#include <vector>

export module Sturdy.Core:VulkanSwapchain;

import :RendererError;
import Sturdy.Foundation;

using SFT::Core::renderer_error;
using SFT::Core::RendererErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using std::vector;

export namespace SFT::Core::Vulkan {

    // Owns a VkSwapchainKHR and caches the swapchain images (non-owning — the
    // driver owns swapchain images). Destroyed via destroy() or destructor.
    class VulkanSwapchain {
      public:
        VulkanSwapchain() = default;
        ~VulkanSwapchain() { destroy(); }

        VulkanSwapchain(const VulkanSwapchain &) = delete;
        VulkanSwapchain &operator=(const VulkanSwapchain &) = delete;

        VulkanSwapchain(VulkanSwapchain &&o) noexcept
            : device_(o.device_), swapchain_(o.swapchain_), images_(std::move(o.images_)),
              format_(o.format_), color_space_(o.color_space_),
              extent_(o.extent_), present_mode_(o.present_mode_) {
            o.device_ = VK_NULL_HANDLE;
            o.swapchain_ = VK_NULL_HANDLE;
        }

        VulkanSwapchain &operator=(VulkanSwapchain &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                swapchain_ = o.swapchain_;
                images_ = std::move(o.images_);
                format_ = o.format_;
                color_space_ = o.color_space_;
                extent_ = o.extent_;
                present_mode_ = o.present_mode_;
                o.device_ = VK_NULL_HANDLE;
                o.swapchain_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        [[nodiscard]] static RendererExpected<VulkanSwapchain> create(
            VkDevice device,
            const VkSwapchainCreateInfoKHR &info) noexcept {
            VkSwapchainKHR sc = VK_NULL_HANDLE;
            if (vkCreateSwapchainKHR(device, &info, nullptr, &sc) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateSwapchainKHR failed.");

            u32 count = 0;
            if (vkGetSwapchainImagesKHR(device, sc, &count, nullptr) != VK_SUCCESS) {
                vkDestroySwapchainKHR(device, sc, nullptr);
                return renderer_error(RendererErrorCode::OperationFailed, "vkGetSwapchainImagesKHR (count) failed.");
            }
            vector<VkImage> images(count, VK_NULL_HANDLE);
            if (vkGetSwapchainImagesKHR(device, sc, &count, images.data()) != VK_SUCCESS) {
                vkDestroySwapchainKHR(device, sc, nullptr);
                return renderer_error(RendererErrorCode::OperationFailed, "vkGetSwapchainImagesKHR (populate) failed.");
            }

            VulkanSwapchain out;
            out.device_ = device;
            out.swapchain_ = sc;
            out.images_ = std::move(images);
            out.format_ = info.imageFormat;
            out.color_space_ = info.imageColorSpace;
            out.extent_ = info.imageExtent;
            out.present_mode_ = info.presentMode;
            return out;
        }

        [[nodiscard]] VkSwapchainKHR vk_handle() const noexcept { return swapchain_; }
        [[nodiscard]] bool is_valid() const noexcept { return swapchain_ != VK_NULL_HANDLE; }
        [[nodiscard]] VkFormat format() const noexcept { return format_; }
        [[nodiscard]] VkColorSpaceKHR color_space() const noexcept { return color_space_; }
        [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }
        [[nodiscard]] VkPresentModeKHR present_mode() const noexcept { return present_mode_; }
        [[nodiscard]] u32 image_count() const noexcept { return static_cast<u32>(images_.size()); }
        [[nodiscard]] const vector<VkImage> &images() const noexcept { return images_; }
        [[nodiscard]] VkImage image(u32 i) const noexcept { return images_[i]; }

        // VK_SUBOPTIMAL_KHR is treated as success — caller should rebuild the swapchain after the frame.
        [[nodiscard]] RendererExpected<u32> acquire_next_image(
            VkSemaphore signal_semaphore,
            VkFence fence = VK_NULL_HANDLE,
            u64 timeout_ns = UINT64_MAX) noexcept {
            VkAcquireNextImageInfoKHR info{
                .sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,
                .pNext = nullptr,
                .swapchain = swapchain_,
                .timeout = timeout_ns,
                .semaphore = signal_semaphore,
                .fence = fence,
                .deviceMask = 1,
            };
            u32 index = 0;
            VkResult res = vkAcquireNextImage2KHR(device_, &info, &index);
            if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
                return renderer_error(RendererErrorCode::OperationFailed, "vkAcquireNextImage2KHR failed.");
            return index;
        }

        void destroy() noexcept {
            if (swapchain_ == VK_NULL_HANDLE)
                return;
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
            images_.clear();
            format_ = VK_FORMAT_UNDEFINED;
            extent_ = {};
        }

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
        vector<VkImage> images_;
        VkFormat format_ = VK_FORMAT_UNDEFINED;
        VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        VkExtent2D extent_ = {};
        VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    };

} // namespace SFT::Core::Vulkan
