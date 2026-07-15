#include "VulkanSync.hpp"

namespace SFT::Core::Vulkan {

VulkanFence::~VulkanFence() { destroy(); }

VulkanFence::VulkanFence(VulkanFence &&o) noexcept : device_(o.device_), fence_(o.fence_) {
            o.device_ = VK_NULL_HANDLE;
            o.fence_ = VK_NULL_HANDLE;
        }

VulkanFence &VulkanFence::operator=(VulkanFence &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                fence_ = o.fence_;
                o.device_ = VK_NULL_HANDLE;
                o.fence_ = VK_NULL_HANDLE;
            }
            return *this;
        }

[[nodiscard]] RendererExpected<VulkanFence> VulkanFence::create(
            VkDevice device,
            bool signaled) noexcept {
            VkFenceCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .pNext = nullptr,
                .flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : VkFenceCreateFlags{0},
            };
            VkFence fence = VK_NULL_HANDLE;
            if (vkCreateFence(device, &info, nullptr, &fence) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateFence failed.");
            VulkanFence out;
            out.device_ = device;
            out.fence_ = fence;
            return out;
        }

[[nodiscard]] VkFence VulkanFence::vk_handle() const noexcept { return fence_; }

[[nodiscard]] bool VulkanFence::is_valid() const noexcept { return fence_ != VK_NULL_HANDLE; }

[[nodiscard]] RendererExpected<bool> VulkanFence::is_signaled() const noexcept {
            VkResult res = vkGetFenceStatus(device_, fence_);
            if (res == VK_SUCCESS)
                return true;
            if (res == VK_NOT_READY)
                return false;
            return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetFenceStatus failed.");
        }

[[nodiscard]] RendererResult VulkanFence::wait(u64 timeout_ns) noexcept {
            VkResult res = vkWaitForFences(device_, 1, &fence_, VK_TRUE, timeout_ns);
            if (res != VK_SUCCESS && res != VK_TIMEOUT)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkWaitForFences failed.");
            return {};
        }

[[nodiscard]] RendererResult VulkanFence::reset() noexcept {
            if (vkResetFences(device_, 1, &fence_) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkResetFences failed.");
            return {};
        }

void VulkanFence::destroy() noexcept {
            if (fence_ == VK_NULL_HANDLE)
                return;
            vkDestroyFence(device_, fence_, nullptr);
            fence_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

VulkanSemaphore::~VulkanSemaphore() { destroy(); }

VulkanSemaphore::VulkanSemaphore(VulkanSemaphore &&o) noexcept
            : device_(o.device_), semaphore_(o.semaphore_), type_(o.type_) {
            o.device_ = VK_NULL_HANDLE;
            o.semaphore_ = VK_NULL_HANDLE;
        }

VulkanSemaphore &VulkanSemaphore::operator=(VulkanSemaphore &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                semaphore_ = o.semaphore_;
                type_ = o.type_;
                o.device_ = VK_NULL_HANDLE;
                o.semaphore_ = VK_NULL_HANDLE;
            }
            return *this;
        }

[[nodiscard]] RendererExpected<VulkanSemaphore> VulkanSemaphore::create_binary(VkDevice device) noexcept {
            return create(device, VK_SEMAPHORE_TYPE_BINARY, 0);
        }

[[nodiscard]] RendererExpected<VulkanSemaphore> VulkanSemaphore::create_timeline(
            VkDevice device,
            u64 initial_value) noexcept {
            return create(device, VK_SEMAPHORE_TYPE_TIMELINE, initial_value);
        }

[[nodiscard]] VkSemaphore VulkanSemaphore::vk_handle() const noexcept { return semaphore_; }

[[nodiscard]] bool VulkanSemaphore::is_valid() const noexcept { return semaphore_ != VK_NULL_HANDLE; }

[[nodiscard]] VkSemaphoreType VulkanSemaphore::type() const noexcept { return type_; }

[[nodiscard]] bool VulkanSemaphore::is_timeline() const noexcept { return type_ == VK_SEMAPHORE_TYPE_TIMELINE; }

[[nodiscard]] RendererExpected<u64> VulkanSemaphore::counter_value() const noexcept {
            u64 value = 0;
            if (vkGetSemaphoreCounterValue(device_, semaphore_, &value) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetSemaphoreCounterValue failed.");
            return value;
        }

[[nodiscard]] RendererResult VulkanSemaphore::signal(u64 value) noexcept {
            VkSemaphoreSignalInfo info{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                .pNext = nullptr,
                .semaphore = semaphore_,
                .value = value,
            };
            if (vkSignalSemaphore(device_, &info) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkSignalSemaphore failed.");
            return {};
        }

[[nodiscard]] RendererResult VulkanSemaphore::wait(u64 value, u64 timeout_ns) noexcept {
            VkSemaphoreWaitInfo info{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                .pNext = nullptr,
                .flags = 0,
                .semaphoreCount = 1,
                .pSemaphores = &semaphore_,
                .pValues = &value,
            };
            VkResult res = vkWaitSemaphores(device_, &info, timeout_ns);
            if (res != VK_SUCCESS && res != VK_TIMEOUT)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkWaitSemaphores failed.");
            return {};
        }

[[nodiscard]] VkSemaphoreSubmitInfo VulkanSemaphore::submit_info(VkPipelineStageFlags2 stage, u64 value) const noexcept {
            return VkSemaphoreSubmitInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .pNext = nullptr,
                .semaphore = semaphore_,
                .value = value,
                .stageMask = stage,
                .deviceIndex = 0,
            };
        }

void VulkanSemaphore::destroy() noexcept {
            if (semaphore_ == VK_NULL_HANDLE)
                return;
            vkDestroySemaphore(device_, semaphore_, nullptr);
            semaphore_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

[[nodiscard]] RendererExpected<VulkanSemaphore> VulkanSemaphore::create(
            VkDevice device,
            VkSemaphoreType type,
            u64 initial_value) noexcept {
            VkSemaphoreTypeCreateInfo type_info{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                .pNext = nullptr,
                .semaphoreType = type,
                .initialValue = initial_value,
            };
            VkSemaphoreCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                .pNext = &type_info,
                .flags = 0,
            };
            VkSemaphore sem = VK_NULL_HANDLE;
            if (vkCreateSemaphore(device, &info, nullptr, &sem) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateSemaphore failed.");
            VulkanSemaphore out;
            out.device_ = device;
            out.semaphore_ = sem;
            out.type_ = type;
            return out;
        }

VulkanEvent::~VulkanEvent() { destroy(); }

VulkanEvent::VulkanEvent(VulkanEvent &&o) noexcept : device_(o.device_), event_(o.event_) {
            o.device_ = VK_NULL_HANDLE;
            o.event_ = VK_NULL_HANDLE;
        }

VulkanEvent &VulkanEvent::operator=(VulkanEvent &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                event_ = o.event_;
                o.device_ = VK_NULL_HANDLE;
                o.event_ = VK_NULL_HANDLE;
            }
            return *this;
        }

[[nodiscard]] RendererExpected<VulkanEvent> VulkanEvent::create(VkDevice device,
                                                                  bool device_only) noexcept {
            VkEventCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO,
                .pNext = nullptr,
                .flags = device_only ? VK_EVENT_CREATE_DEVICE_ONLY_BIT : VkEventCreateFlags{0},
            };
            VkEvent event = VK_NULL_HANDLE;
            if (vkCreateEvent(device, &info, nullptr, &event) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateEvent failed.");
            VulkanEvent out;
            out.device_ = device;
            out.event_ = event;
            return out;
        }

[[nodiscard]] VkEvent VulkanEvent::vk_handle() const noexcept { return event_; }

[[nodiscard]] bool VulkanEvent::is_valid() const noexcept { return event_ != VK_NULL_HANDLE; }

[[nodiscard]] RendererExpected<bool> VulkanEvent::is_signaled() const noexcept {
            VkResult res = vkGetEventStatus(device_, event_);
            if (res == VK_EVENT_SET)
                return true;
            if (res == VK_EVENT_RESET)
                return false;
            return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetEventStatus failed.");
        }

[[nodiscard]] RendererResult VulkanEvent::set() noexcept {
            if (vkSetEvent(device_, event_) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkSetEvent failed.");
            return {};
        }

[[nodiscard]] RendererResult VulkanEvent::reset() noexcept {
            if (vkResetEvent(device_, event_) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkResetEvent failed.");
            return {};
        }

void VulkanEvent::destroy() noexcept {
            if (event_ == VK_NULL_HANDLE)
                return;
            vkDestroyEvent(device_, event_, nullptr);
            event_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

} // namespace SFT::Core::Vulkan
