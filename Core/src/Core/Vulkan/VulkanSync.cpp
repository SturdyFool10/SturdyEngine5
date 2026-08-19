#include <Core/Vulkan/VulkanSync.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanFence::~VulkanFence() { destroy(); }

/// Performs the vulkan fence operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanFence::VulkanFence(VulkanFence &&o) noexcept : device_(o.device_), fence_(o.fence_) {
            ZoneScopedN("VulkanFence::VulkanFence");
            o.device_ = VK_NULL_HANDLE;
            o.fence_ = VK_NULL_HANDLE;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanFence &VulkanFence::operator=(VulkanFence &&o) noexcept {
            ZoneScopedN("VulkanFence::operator=");
            if (this != &o) {
                destroy();
                device_ = o.device_;
                fence_ = o.fence_;
                o.device_ = VK_NULL_HANDLE;
                o.fence_ = VK_NULL_HANDLE;
            }
            return *this;
        }

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param signaled `signaled` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanFence> VulkanFence::create(
            VkDevice device,
            bool signaled) noexcept {
            ZoneScopedN("VulkanFence::create");
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

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkFence VulkanFence::vk_handle() const noexcept { return fence_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanFence::is_valid() const noexcept { return fence_ != VK_NULL_HANDLE; }

/// Reports whether signaled holds for this `Vulkan`.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<bool> VulkanFence::is_signaled() const noexcept {
            ZoneScopedN("VulkanFence::is_signaled");
            VkResult res = vkGetFenceStatus(device_, fence_);
            if (res == VK_SUCCESS)
                return true;
            if (res == VK_NOT_READY)
                return false;
            return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetFenceStatus failed.");
        }

/// Waits for the associated operation or synchronization primitive to complete.
///
/// @param timeout_ns Maximum amount of time to wait before giving up.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanFence::wait(u64 timeout_ns) noexcept {
            ZoneScopedN("VulkanFence::wait");
            VkResult res = vkWaitForFences(device_, 1, &fence_, VK_TRUE, timeout_ns);
            if (res != VK_SUCCESS && res != VK_TIMEOUT)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkWaitForFences failed.");
            return {};
        }

/// Resets the object to its baseline state.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanFence::reset() noexcept {
            ZoneScopedN("VulkanFence::reset");
            if (vkResetFences(device_, 1, &fence_) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkResetFences failed.");
            return {};
        }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanFence::destroy() noexcept {
            ZoneScopedN("VulkanFence::destroy");
            if (fence_ == VK_NULL_HANDLE)
                return;
            vkDestroyFence(device_, fence_, nullptr);
            fence_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanSemaphore::~VulkanSemaphore() { destroy(); }

/// Performs the vulkan semaphore operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanSemaphore::VulkanSemaphore(VulkanSemaphore &&o) noexcept
            : device_(o.device_), semaphore_(o.semaphore_), type_(o.type_) {
            ZoneScopedN("VulkanSemaphore::VulkanSemaphore");
            o.device_ = VK_NULL_HANDLE;
            o.semaphore_ = VK_NULL_HANDLE;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanSemaphore &VulkanSemaphore::operator=(VulkanSemaphore &&o) noexcept {
            ZoneScopedN("VulkanSemaphore::operator=");
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

/// Creates a binary from the supplied parameters.
///
/// @param device Device used or affected by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanSemaphore> VulkanSemaphore::create_binary(VkDevice device) noexcept {
            ZoneScopedN("VulkanSemaphore::create_binary");
            return create(device, VK_SEMAPHORE_TYPE_BINARY, 0);
        }

/// Creates a timeline from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param initial_value Value consumed by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanSemaphore> VulkanSemaphore::create_timeline(
            VkDevice device,
            u64 initial_value) noexcept {
            ZoneScopedN("VulkanSemaphore::create_timeline");
            return create(device, VK_SEMAPHORE_TYPE_TIMELINE, initial_value);
        }

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkSemaphore VulkanSemaphore::vk_handle() const noexcept { return semaphore_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanSemaphore::is_valid() const noexcept { return semaphore_ != VK_NULL_HANDLE; }

/// Returns the runtime or backend type represented by `Vulkan`.
///
/// @return Returns the current type value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkSemaphoreType VulkanSemaphore::type() const noexcept { return type_; }

/// Reports whether timeline holds for this `Vulkan`.
///
/// @return Returns the current is timeline value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanSemaphore::is_timeline() const noexcept { return type_ == VK_SEMAPHORE_TYPE_TIMELINE; }

/// Returns the current or globally available counter value value.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<u64> VulkanSemaphore::counter_value() const noexcept {
            ZoneScopedN("VulkanSemaphore::counter_value");
            u64 value = 0;
            if (vkGetSemaphoreCounterValue(device_, semaphore_, &value) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetSemaphoreCounterValue failed.");
            return value;
        }

/// Signals the associated synchronization primitive or event.
///
/// @param value Value consumed by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanSemaphore::signal(u64 value) noexcept {
            ZoneScopedN("VulkanSemaphore::signal");
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

/// Waits for the associated operation or synchronization primitive to complete.
///
/// @param value Value consumed by the operation.
/// @param timeout_ns Maximum amount of time to wait before giving up.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanSemaphore::wait(u64 value, u64 timeout_ns) noexcept {
            ZoneScopedN("VulkanSemaphore::wait");
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

/// Submits info.
///
/// @param stage `stage` value used by the operation.
/// @param value Value consumed by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] VkSemaphoreSubmitInfo VulkanSemaphore::submit_info(VkPipelineStageFlags2 stage, u64 value) const noexcept {
            ZoneScopedN("VulkanSemaphore::submit_info");
            return VkSemaphoreSubmitInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .pNext = nullptr,
                .semaphore = semaphore_,
                .value = value,
                .stageMask = stage,
                .deviceIndex = 0,
            };
        }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanSemaphore::destroy() noexcept {
            ZoneScopedN("VulkanSemaphore::destroy");
            if (semaphore_ == VK_NULL_HANDLE)
                return;
            vkDestroySemaphore(device_, semaphore_, nullptr);
            semaphore_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param type Type value to inspect, select, or convert.
/// @param initial_value Value consumed by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanSemaphore> VulkanSemaphore::create(
            VkDevice device,
            VkSemaphoreType type,
            u64 initial_value) noexcept {
            ZoneScopedN("VulkanSemaphore::create");
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

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanEvent::~VulkanEvent() { destroy(); }

/// Performs the vulkan event operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanEvent::VulkanEvent(VulkanEvent &&o) noexcept : device_(o.device_), event_(o.event_) {
            ZoneScopedN("VulkanEvent::VulkanEvent");
            o.device_ = VK_NULL_HANDLE;
            o.event_ = VK_NULL_HANDLE;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanEvent &VulkanEvent::operator=(VulkanEvent &&o) noexcept {
            ZoneScopedN("VulkanEvent::operator=");
            if (this != &o) {
                destroy();
                device_ = o.device_;
                event_ = o.event_;
                o.device_ = VK_NULL_HANDLE;
                o.event_ = VK_NULL_HANDLE;
            }
            return *this;
        }

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param device_only Device used or affected by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanEvent> VulkanEvent::create(VkDevice device,
                                                                  bool device_only) noexcept {
            ZoneScopedN("VulkanEvent::create");
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

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkEvent VulkanEvent::vk_handle() const noexcept { return event_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanEvent::is_valid() const noexcept { return event_ != VK_NULL_HANDLE; }

/// Reports whether signaled holds for this `Vulkan`.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<bool> VulkanEvent::is_signaled() const noexcept {
            ZoneScopedN("VulkanEvent::is_signaled");
            VkResult res = vkGetEventStatus(device_, event_);
            if (res == VK_EVENT_SET)
                return true;
            if (res == VK_EVENT_RESET)
                return false;
            return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetEventStatus failed.");
        }

/// Returns the current or globally available set value.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanEvent::set() noexcept {
            ZoneScopedN("VulkanEvent::set");
            if (vkSetEvent(device_, event_) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkSetEvent failed.");
            return {};
        }

/// Resets the object to its baseline state.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanEvent::reset() noexcept {
            ZoneScopedN("VulkanEvent::reset");
            if (vkResetEvent(device_, event_) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkResetEvent failed.");
            return {};
        }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanEvent::destroy() noexcept {
            ZoneScopedN("VulkanEvent::destroy");
            if (event_ == VK_NULL_HANDLE)
                return;
            vkDestroyEvent(device_, event_, nullptr);
            event_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

} // namespace SFT::Core::Vulkan
