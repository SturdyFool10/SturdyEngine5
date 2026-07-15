#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <mutex>
#include <span>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using std::span;

namespace SFT::Core::Vulkan {

    // Wraps a VkQueue with a per-queue mutex. All Vulkan queue commands require external
    // synchronization on the queue handle; this type provides it automatically.
    // Move-only (the mutex is not transferred on move — a fresh one is created in the destination).
    class VulkanQueue {
      public:
        VulkanQueue() = default;

        VulkanQueue(VkQueue handle, u32 family_index) noexcept;

        VulkanQueue(const VulkanQueue &) = delete;
        VulkanQueue &operator=(const VulkanQueue &) = delete;

        VulkanQueue(VulkanQueue &&o) noexcept;

        VulkanQueue &operator=(VulkanQueue &&o) noexcept;

        [[nodiscard]] VkQueue vk_handle() const noexcept;
        [[nodiscard]] u32 family_index() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;

        [[nodiscard]] RendererResult submit(span<const VkSubmitInfo2> submits,
                                            VkFence fence = VK_NULL_HANDLE) noexcept;

        // Convenience for the common one-command-buffer submission.
        [[nodiscard]] RendererResult submit(
            const VkCommandBufferSubmitInfo &command_buffer,
            span<const VkSemaphoreSubmitInfo> waits,
            span<const VkSemaphoreSubmitInfo> signals,
            VkFence fence = VK_NULL_HANDLE) noexcept;

        // Returns true if the swapchain is stale (suboptimal or out-of-date) and should be rebuilt
        // before the next frame, false if presentation is fully up to date. Both are treated as
        // success — only failures other than staleness are reported as an error.
        [[nodiscard]] RendererExpected<bool> present(const VkPresentInfoKHR &info) noexcept;

        [[nodiscard]] RendererResult wait_idle() noexcept;

        [[nodiscard]] RendererResult bind_sparse(span<const VkBindSparseInfo> infos,
                                                 VkFence fence = VK_NULL_HANDLE) noexcept;

      private:
        VkQueue handle_ = VK_NULL_HANDLE;
        u32 family_index_ = 0;
        mutable std::mutex mutex_;
    };

} // namespace SFT::Core::Vulkan
