module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <mutex>
#include <span>
#pragma endregion

export module Sturdy.Core:VulkanQueue;

import :RendererError;
import Sturdy.Foundation;

using SFT::Core::renderer_error;
using SFT::Core::RendererErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using std::span;

export namespace SFT::Core::Vulkan {

    // Wraps a VkQueue with a per-queue mutex. All Vulkan queue commands require external
    // synchronization on the queue handle; this type provides it automatically.
    // Move-only (the mutex is not transferred on move — a fresh one is created in the destination).
    class VulkanQueue {
      public:
        VulkanQueue() = default;

        VulkanQueue(VkQueue handle, u32 family_index) noexcept
            : handle_(handle), family_index_(family_index) {}

        VulkanQueue(const VulkanQueue &) = delete;
        VulkanQueue &operator=(const VulkanQueue &) = delete;

        VulkanQueue(VulkanQueue &&o) noexcept
            : handle_(o.handle_), family_index_(o.family_index_) {
            o.handle_ = VK_NULL_HANDLE;
        }

        VulkanQueue &operator=(VulkanQueue &&o) noexcept {
            if (this != &o) {
                handle_ = o.handle_;
                family_index_ = o.family_index_;
                o.handle_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        [[nodiscard]] VkQueue vk_handle() const noexcept { return handle_; }
        [[nodiscard]] u32 family_index() const noexcept { return family_index_; }
        [[nodiscard]] bool is_valid() const noexcept { return handle_ != VK_NULL_HANDLE; }

        [[nodiscard]] RendererResult submit(span<const VkSubmitInfo2> submits,
                                            VkFence fence = VK_NULL_HANDLE) noexcept {
            std::lock_guard lock(mutex_);
            if (vkQueueSubmit2(handle_, static_cast<u32>(submits.size()), submits.data(), fence) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkQueueSubmit2 failed.");
            return {};
        }

        // Convenience for the common one-command-buffer submission.
        [[nodiscard]] RendererResult submit(
            const VkCommandBufferSubmitInfo &command_buffer,
            span<const VkSemaphoreSubmitInfo> waits,
            span<const VkSemaphoreSubmitInfo> signals,
            VkFence fence = VK_NULL_HANDLE) noexcept {
            VkSubmitInfo2 submit_info{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                .pNext = nullptr,
                .flags = 0,
                .waitSemaphoreInfoCount = static_cast<u32>(waits.size()),
                .pWaitSemaphoreInfos = waits.data(),
                .commandBufferInfoCount = 1,
                .pCommandBufferInfos = &command_buffer,
                .signalSemaphoreInfoCount = static_cast<u32>(signals.size()),
                .pSignalSemaphoreInfos = signals.data(),
            };
            return submit(span{&submit_info, 1}, fence);
        }

        // Returns true if the swapchain is stale (suboptimal or out-of-date) and should be rebuilt
        // before the next frame, false if presentation is fully up to date. Both are treated as
        // success — only failures other than staleness are reported as an error.
        [[nodiscard]] RendererExpected<bool> present(const VkPresentInfoKHR &info) noexcept {
            std::lock_guard lock(mutex_);
            VkResult res = vkQueuePresentKHR(handle_, &info);
            if (res == VK_SUCCESS)
                return false;
            if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
                return true;
            return renderer_error(RendererErrorCode::OperationFailed, "vkQueuePresentKHR failed.");
        }

        [[nodiscard]] RendererResult wait_idle() noexcept {
            std::lock_guard lock(mutex_);
            if (vkQueueWaitIdle(handle_) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkQueueWaitIdle failed.");
            return {};
        }

        [[nodiscard]] RendererResult bind_sparse(span<const VkBindSparseInfo> infos,
                                                 VkFence fence = VK_NULL_HANDLE) noexcept {
            std::lock_guard lock(mutex_);
            if (vkQueueBindSparse(handle_, static_cast<u32>(infos.size()), infos.data(), fence) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkQueueBindSparse failed.");
            return {};
        }

      private:
        VkQueue handle_ = VK_NULL_HANDLE;
        u32 family_index_ = 0;
        mutable std::mutex mutex_;
    };

} // namespace SFT::Core::Vulkan
