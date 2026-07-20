#pragma once

#include <Foundation/src/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <span>
#include <variant>
#pragma endregion

#include <Async/src/Mutex.hpp>
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
        // Pure external-synchronization lock for the four Vulkan queue calls below — handle_ itself
        // is never concurrently mutated (moves happen during single-threaded setup), so there's
        // nothing to guard; Async::Mutex<T> still needs some T, so this holds an empty marker purely
        // for its lock()/MutexGuard semantics. Not moved on VulkanQueue's own move (Async::Mutex is
        // itself non-movable, so — like the std::mutex this replaces — a fresh one is implicitly
        // default-constructed in the destination; see the move constructor's doc comment above).
        mutable Async::Mutex<std::monostate> submission_lock_;
    };

} // namespace SFT::Core::Vulkan
