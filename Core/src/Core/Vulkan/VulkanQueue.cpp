#include "VulkanQueue.hpp"

#include <chrono>

#include <tracy/Tracy.hpp>

using std::chrono::duration;
using std::chrono::steady_clock;

namespace SFT::Core::Vulkan {

/// Performs the vulkan queue operation for `Vulkan` using the supplied arguments.
///
/// @param handle Handle identifying the target object or resource.
/// @param family_index Zero-based index of the target element or entry.
///
/// @note This function does not throw exceptions.
VulkanQueue::VulkanQueue(VkQueue handle, u32 family_index) noexcept
            : handle_(handle), family_index_(family_index) {}

/// Performs the vulkan queue operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanQueue::VulkanQueue(VulkanQueue &&o) noexcept
            : handle_(o.handle_), family_index_(o.family_index_) {
            ZoneScopedN("VulkanQueue::VulkanQueue");
            o.handle_ = VK_NULL_HANDLE;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanQueue &VulkanQueue::operator=(VulkanQueue &&o) noexcept {
            ZoneScopedN("VulkanQueue::operator=");
            if (this != &o) {
                handle_ = o.handle_;
                family_index_ = o.family_index_;
                o.handle_ = VK_NULL_HANDLE;
            }
            return *this;
        }

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkQueue VulkanQueue::vk_handle() const noexcept { return handle_; }

/// Computes the family index required by the supplied values.
///
/// @return Returns the current family index value.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 VulkanQueue::family_index() const noexcept { return family_index_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanQueue::is_valid() const noexcept { return handle_ != VK_NULL_HANDLE; }

/// Submits the requested work.
///
/// @param submits `submits` value used by the operation.
/// @param fence Fence used or affected by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::DeviceLost`, `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanQueue::submit(span<const VkSubmitInfo2> submits,
                                            VkFence fence) noexcept {
            ZoneScopedN("VulkanQueue::submit");
            auto lock = submission_lock_.lock();
            const VkResult result = vkQueueSubmit2(handle_, static_cast<u32>(submits.size()), submits.data(), fence);
            if (result == VK_ERROR_DEVICE_LOST)
                return graphics_backend_error(GraphicsBackendErrorCode::DeviceLost, "vkQueueSubmit2 reported device loss.");
            if (result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkQueueSubmit2 failed.");
            return {};
        }

/// Submits the requested work.
///
/// @param command_buffer Buffer used or affected by the operation.
/// @param waits `waits` value used by the operation.
/// @param signals `signals` value used by the operation.
/// @param fence Fence used or affected by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanQueue::submit(
            const VkCommandBufferSubmitInfo &command_buffer,
            span<const VkSemaphoreSubmitInfo> waits,
            span<const VkSemaphoreSubmitInfo> signals,
            VkFence fence) noexcept {
            ZoneScopedN("VulkanQueue::submit");
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

/// Presents the completed frame to the target surface or swapchain.
///
/// @param info Description of the resource or operation to perform.
/// @param lock_wait_ms `lock_wait_ms` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::DeviceLost`, `GraphicsBackendErrorCode::SurfaceLost`, `GraphicsBackendErrorCode::FullScreenExclusiveLost`, `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<PresentOutcome> VulkanQueue::present(const VkPresentInfoKHR &info, f64 *lock_wait_ms) noexcept {
            ZoneScopedN("VulkanQueue::present");
            const auto before_lock = steady_clock::now();
            auto lock = submission_lock_.lock();
            if (lock_wait_ms != nullptr) [[likely]] {
                *lock_wait_ms = duration<f64>(steady_clock::now() - before_lock).count() * 1000.0;
            }
            VkResult res;
            {
                ZoneScopedN("vkQueuePresentKHR");
                res = vkQueuePresentKHR(handle_, &info);
            }
            if (res == VK_SUCCESS) [[unlikely]]
                return PresentOutcome::Success;
            if (res == VK_SUBOPTIMAL_KHR) [[unlikely]]
                return PresentOutcome::Suboptimal;
            if (res == VK_ERROR_OUT_OF_DATE_KHR) [[unlikely]]
                return PresentOutcome::OutOfDate;
            if (res == VK_ERROR_DEVICE_LOST) [[unlikely]]
                return graphics_backend_error(GraphicsBackendErrorCode::DeviceLost, "vkQueuePresentKHR reported device loss.");
            if (res == VK_ERROR_SURFACE_LOST_KHR) [[unlikely]]
                return graphics_backend_error(GraphicsBackendErrorCode::SurfaceLost, "vkQueuePresentKHR reported surface loss.");
            if (res == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) [[unlikely]]
                return graphics_backend_error(GraphicsBackendErrorCode::FullScreenExclusiveLost,
                                              "vkQueuePresentKHR reported loss of exclusive-fullscreen ownership.");
            return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkQueuePresentKHR failed.");
        }

/// Waits for idle to complete.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::DeviceLost`, `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanQueue::wait_idle() noexcept {
            VkResult result;
            {
                ZoneScopedN("VulkanQueue::wait_idle");
                auto lock = submission_lock_.lock();
                result = vkQueueWaitIdle(handle_);
            }
            if (result == VK_ERROR_DEVICE_LOST) [[unlikely]]
                return graphics_backend_error(GraphicsBackendErrorCode::DeviceLost, "vkQueueWaitIdle reported device loss.");
            if (result != VK_SUCCESS) [[unlikely]]
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkQueueWaitIdle failed.");
            return {};
        }

/// Binds sparse for subsequent operations.
///
/// @param infos Description of the resource or operation to perform.
/// @param fence Fence used or affected by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::DeviceLost`, `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanQueue::bind_sparse(span<const VkBindSparseInfo> infos,
                                                 VkFence fence) noexcept {
            VkResult result;
            {
                ZoneScopedN("VulkanQueue::bind_sparse");
                auto lock = submission_lock_.lock();
                result = vkQueueBindSparse(handle_, static_cast<u32>(infos.size()), infos.data(), fence);
            }
            if (result == VK_ERROR_DEVICE_LOST) [[unlikely]]
                return graphics_backend_error(GraphicsBackendErrorCode::DeviceLost, "vkQueueBindSparse reported device loss.");
            if (result != VK_SUCCESS) [[unlikely]]
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkQueueBindSparse failed.");
            return {};
        }

} // namespace SFT::Core::Vulkan
