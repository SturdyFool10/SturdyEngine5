module;
#include "volk.h"

export module Sturdy.Core:VulkanSync;

import :RendererError;
import Sturdy.Foundation;

using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using SFT::Core::RendererErrorCode;
using SFT::Core::renderer_error;

export namespace SFT::Core::Vulkan {

// ─── VulkanFence ─────────────────────────────────────────────────────────────

class VulkanFence {
  public:
    VulkanFence() = default;
    ~VulkanFence() { destroy(); }

    VulkanFence(const VulkanFence&)            = delete;
    VulkanFence& operator=(const VulkanFence&) = delete;

    VulkanFence(VulkanFence&& o) noexcept : device_(o.device_), fence_(o.fence_) {
        o.device_ = VK_NULL_HANDLE;
        o.fence_  = VK_NULL_HANDLE;
    }
    VulkanFence& operator=(VulkanFence&& o) noexcept {
        if (this != &o) { destroy(); device_ = o.device_; fence_ = o.fence_;
            o.device_ = VK_NULL_HANDLE; o.fence_ = VK_NULL_HANDLE; }
        return *this;
    }

    [[nodiscard]] static RendererExpected<VulkanFence> create(
        VkDevice device, bool signaled = false
    ) noexcept {
        VkFenceCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : VkFenceCreateFlags{0},
        };
        VkFence fence = VK_NULL_HANDLE;
        if (vkCreateFence(device, &info, nullptr, &fence) != VK_SUCCESS)
            return renderer_error(RendererErrorCode::OperationFailed, "vkCreateFence failed.");
        VulkanFence out;
        out.device_ = device;
        out.fence_  = fence;
        return out;
    }

    [[nodiscard]] VkFence vk_handle() const noexcept { return fence_; }
    [[nodiscard]] bool    is_valid()  const noexcept { return fence_ != VK_NULL_HANDLE; }

    // Returns true if signaled, false if not ready.
    [[nodiscard]] RendererExpected<bool> is_signaled() const noexcept {
        VkResult res = vkGetFenceStatus(device_, fence_);
        if (res == VK_SUCCESS)   return true;
        if (res == VK_NOT_READY) return false;
        return renderer_error(RendererErrorCode::OperationFailed, "vkGetFenceStatus failed.");
    }

    // Returns success for both VK_SUCCESS and VK_TIMEOUT.
    [[nodiscard]] RendererResult wait(u64 timeout_ns = UINT64_MAX) noexcept {
        VkResult res = vkWaitForFences(device_, 1, &fence_, VK_TRUE, timeout_ns);
        if (res != VK_SUCCESS && res != VK_TIMEOUT)
            return renderer_error(RendererErrorCode::OperationFailed, "vkWaitForFences failed.");
        return {};
    }

    [[nodiscard]] RendererResult reset() noexcept {
        if (vkResetFences(device_, 1, &fence_) != VK_SUCCESS)
            return renderer_error(RendererErrorCode::OperationFailed, "vkResetFences failed.");
        return {};
    }

    void destroy() noexcept {
        if (fence_ == VK_NULL_HANDLE) return;
        vkDestroyFence(device_, fence_, nullptr);
        fence_  = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
    }

  private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkFence  fence_  = VK_NULL_HANDLE;
};

// ─── VulkanSemaphore ──────────────────────────────────────────────────────────

class VulkanSemaphore {
  public:
    VulkanSemaphore() = default;
    ~VulkanSemaphore() { destroy(); }

    VulkanSemaphore(const VulkanSemaphore&)            = delete;
    VulkanSemaphore& operator=(const VulkanSemaphore&) = delete;

    VulkanSemaphore(VulkanSemaphore&& o) noexcept
        : device_(o.device_), semaphore_(o.semaphore_), type_(o.type_) {
        o.device_    = VK_NULL_HANDLE;
        o.semaphore_ = VK_NULL_HANDLE;
    }
    VulkanSemaphore& operator=(VulkanSemaphore&& o) noexcept {
        if (this != &o) { destroy();
            device_ = o.device_; semaphore_ = o.semaphore_; type_ = o.type_;
            o.device_ = VK_NULL_HANDLE; o.semaphore_ = VK_NULL_HANDLE; }
        return *this;
    }

    [[nodiscard]] static RendererExpected<VulkanSemaphore> create_binary(VkDevice device) noexcept {
        return create(device, VK_SEMAPHORE_TYPE_BINARY, 0);
    }

    [[nodiscard]] static RendererExpected<VulkanSemaphore> create_timeline(
        VkDevice device, u64 initial_value = 0
    ) noexcept {
        return create(device, VK_SEMAPHORE_TYPE_TIMELINE, initial_value);
    }

    [[nodiscard]] VkSemaphore      vk_handle() const noexcept { return semaphore_; }
    [[nodiscard]] bool             is_valid()  const noexcept { return semaphore_ != VK_NULL_HANDLE; }
    [[nodiscard]] VkSemaphoreType  type()      const noexcept { return type_; }
    [[nodiscard]] bool             is_timeline() const noexcept { return type_ == VK_SEMAPHORE_TYPE_TIMELINE; }

    // Timeline semaphore operations — undefined behaviour on a binary semaphore.
    [[nodiscard]] RendererExpected<u64> counter_value() const noexcept {
        u64 value = 0;
        if (vkGetSemaphoreCounterValue(device_, semaphore_, &value) != VK_SUCCESS)
            return renderer_error(RendererErrorCode::OperationFailed, "vkGetSemaphoreCounterValue failed.");
        return value;
    }

    [[nodiscard]] RendererResult signal(u64 value) noexcept {
        VkSemaphoreSignalInfo info{
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
            .pNext     = nullptr,
            .semaphore = semaphore_,
            .value     = value,
        };
        if (vkSignalSemaphore(device_, &info) != VK_SUCCESS)
            return renderer_error(RendererErrorCode::OperationFailed, "vkSignalSemaphore failed.");
        return {};
    }

    // Returns success for both VK_SUCCESS and VK_TIMEOUT.
    [[nodiscard]] RendererResult wait(u64 value, u64 timeout_ns = UINT64_MAX) noexcept {
        VkSemaphoreWaitInfo info{
            .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .pNext          = nullptr,
            .flags          = 0,
            .semaphoreCount = 1,
            .pSemaphores    = &semaphore_,
            .pValues        = &value,
        };
        VkResult res = vkWaitSemaphores(device_, &info, timeout_ns);
        if (res != VK_SUCCESS && res != VK_TIMEOUT)
            return renderer_error(RendererErrorCode::OperationFailed, "vkWaitSemaphores failed.");
        return {};
    }

    void destroy() noexcept {
        if (semaphore_ == VK_NULL_HANDLE) return;
        vkDestroySemaphore(device_, semaphore_, nullptr);
        semaphore_ = VK_NULL_HANDLE;
        device_    = VK_NULL_HANDLE;
    }

  private:
    [[nodiscard]] static RendererExpected<VulkanSemaphore> create(
        VkDevice device, VkSemaphoreType type, u64 initial_value
    ) noexcept {
        VkSemaphoreTypeCreateInfo type_info{
            .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .pNext         = nullptr,
            .semaphoreType = type,
            .initialValue  = initial_value,
        };
        VkSemaphoreCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &type_info,
            .flags = 0,
        };
        VkSemaphore sem = VK_NULL_HANDLE;
        if (vkCreateSemaphore(device, &info, nullptr, &sem) != VK_SUCCESS)
            return renderer_error(RendererErrorCode::OperationFailed, "vkCreateSemaphore failed.");
        VulkanSemaphore out;
        out.device_    = device;
        out.semaphore_ = sem;
        out.type_      = type;
        return out;
    }

    VkDevice        device_    = VK_NULL_HANDLE;
    VkSemaphore     semaphore_ = VK_NULL_HANDLE;
    VkSemaphoreType type_      = VK_SEMAPHORE_TYPE_BINARY;
};

} // namespace SFT::Core::Vulkan
