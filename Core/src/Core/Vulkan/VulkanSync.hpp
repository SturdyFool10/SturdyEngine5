#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#include "volk.h"
#pragma endregion

#include <Core/GraphicsBackendError.hpp>

using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;

namespace SFT::Core::Vulkan {

    // ─── VulkanFence ─────────────────────────────────────────────────────────────

    class VulkanFence {
      public:
        VulkanFence() = default;
        ~VulkanFence();

        VulkanFence(const VulkanFence &) = delete;
        VulkanFence &operator=(const VulkanFence &) = delete;

        VulkanFence(VulkanFence &&o) noexcept;
        VulkanFence &operator=(VulkanFence &&o) noexcept;

        [[nodiscard]] static RendererExpected<VulkanFence> create(
            VkDevice device,
            bool signaled = false) noexcept;

        [[nodiscard]] VkFence vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;

        // Returns true if signaled, false if not ready.
        [[nodiscard]] RendererExpected<bool> is_signaled() const noexcept;

        // Returns success for both VK_SUCCESS and VK_TIMEOUT.
        [[nodiscard]] RendererResult wait(u64 timeout_ns = UINT64_MAX) noexcept;

        [[nodiscard]] RendererResult reset() noexcept;

        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkFence fence_ = VK_NULL_HANDLE;
    };

    // ─── VulkanSemaphore ──────────────────────────────────────────────────────────

    class VulkanSemaphore {
      public:
        VulkanSemaphore() = default;
        ~VulkanSemaphore();

        VulkanSemaphore(const VulkanSemaphore &) = delete;
        VulkanSemaphore &operator=(const VulkanSemaphore &) = delete;

        VulkanSemaphore(VulkanSemaphore &&o) noexcept;
        VulkanSemaphore &operator=(VulkanSemaphore &&o) noexcept;

        [[nodiscard]] static RendererExpected<VulkanSemaphore> create_binary(VkDevice device) noexcept;

        [[nodiscard]] static RendererExpected<VulkanSemaphore> create_timeline(
            VkDevice device,
            u64 initial_value = 0) noexcept;

        [[nodiscard]] VkSemaphore vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;
        [[nodiscard]] VkSemaphoreType type() const noexcept;
        [[nodiscard]] bool is_timeline() const noexcept;

        // Timeline semaphore operations — undefined behaviour on a binary semaphore.
        [[nodiscard]] RendererExpected<u64> counter_value() const noexcept;

        [[nodiscard]] RendererResult signal(u64 value) noexcept;

        // Returns success for both VK_SUCCESS and VK_TIMEOUT.
        [[nodiscard]] RendererResult wait(u64 value, u64 timeout_ns = UINT64_MAX) noexcept;

        // Builds this semaphore's wait/signal entry for a VkSubmitInfo2. value is ignored by the
        // driver for binary semaphores — only pass a meaningful one for a timeline semaphore.
        [[nodiscard]] VkSemaphoreSubmitInfo submit_info(VkPipelineStageFlags2 stage, u64 value = 0) const noexcept;

        void destroy() noexcept;

      private:
        [[nodiscard]] static RendererExpected<VulkanSemaphore> create(
            VkDevice device,
            VkSemaphoreType type,
            u64 initial_value) noexcept;

        VkDevice device_ = VK_NULL_HANDLE;
        VkSemaphore semaphore_ = VK_NULL_HANDLE;
        VkSemaphoreType type_ = VK_SEMAPHORE_TYPE_BINARY;
    };

    // ─── VulkanEvent ──────────────────────────────────────────────────────────────

    // Owns a VkEvent — the fine-grained *split-barrier* primitive: signal a dependency at one point in
    // a command stream (set_event2) and wait on it later (wait_events2), letting unrelated GPU work
    // overlap the gap. Create with `device_only` when the event is only ever set/waited on the GPU
    // (the synchronization2-recommended fast path; host set/reset/status then become invalid).
    class VulkanEvent {
      public:
        VulkanEvent() = default;
        ~VulkanEvent();

        VulkanEvent(const VulkanEvent &) = delete;
        VulkanEvent &operator=(const VulkanEvent &) = delete;

        VulkanEvent(VulkanEvent &&o) noexcept;
        VulkanEvent &operator=(VulkanEvent &&o) noexcept;

        [[nodiscard]] static RendererExpected<VulkanEvent> create(VkDevice device,
                                                                  bool device_only = true) noexcept;

        [[nodiscard]] VkEvent vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;

        // Host-side operations — only valid for a non-`device_only` event.
        [[nodiscard]] RendererExpected<bool> is_signaled() const noexcept;
        [[nodiscard]] RendererResult set() noexcept;
        [[nodiscard]] RendererResult reset() noexcept;

        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkEvent event_ = VK_NULL_HANDLE;
    };

} // namespace SFT::Core::Vulkan
