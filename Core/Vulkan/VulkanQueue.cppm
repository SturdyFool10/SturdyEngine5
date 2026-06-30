module;
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <mutex>
#include <span>

export module Sturdy.Core:VulkanQueue;

import :RendererError;
import Sturdy.Foundation;

using SFT::Core::renderer_error;
using SFT::Core::RendererErrorCode;
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

        [[nodiscard]] RendererResult present(const VkPresentInfoKHR &info) noexcept {
            std::lock_guard lock(mutex_);
            VkResult res = vkQueuePresentKHR(handle_, &info);
            if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
                return renderer_error(RendererErrorCode::OperationFailed, "vkQueuePresentKHR failed.");
            return {};
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
