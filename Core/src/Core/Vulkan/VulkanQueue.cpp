#include "VulkanQueue.hpp"

namespace SFT::Core::Vulkan {

VulkanQueue::VulkanQueue(VkQueue handle, u32 family_index) noexcept
            : handle_(handle), family_index_(family_index) {}

VulkanQueue::VulkanQueue(VulkanQueue &&o) noexcept
            : handle_(o.handle_), family_index_(o.family_index_) {
            o.handle_ = VK_NULL_HANDLE;
        }

VulkanQueue &VulkanQueue::operator=(VulkanQueue &&o) noexcept {
            if (this != &o) {
                handle_ = o.handle_;
                family_index_ = o.family_index_;
                o.handle_ = VK_NULL_HANDLE;
            }
            return *this;
        }

[[nodiscard]] VkQueue VulkanQueue::vk_handle() const noexcept { return handle_; }

[[nodiscard]] u32 VulkanQueue::family_index() const noexcept { return family_index_; }

[[nodiscard]] bool VulkanQueue::is_valid() const noexcept { return handle_ != VK_NULL_HANDLE; }

[[nodiscard]] RendererResult VulkanQueue::submit(span<const VkSubmitInfo2> submits,
                                            VkFence fence) noexcept {
            auto lock = submission_lock_.lock();
            const VkResult result = vkQueueSubmit2(handle_, static_cast<u32>(submits.size()), submits.data(), fence);
            if (result == VK_ERROR_DEVICE_LOST)
                return graphics_backend_error(GraphicsBackendErrorCode::DeviceLost, "vkQueueSubmit2 reported device loss.");
            if (result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkQueueSubmit2 failed.");
            return {};
        }

[[nodiscard]] RendererResult VulkanQueue::submit(
            const VkCommandBufferSubmitInfo &command_buffer,
            span<const VkSemaphoreSubmitInfo> waits,
            span<const VkSemaphoreSubmitInfo> signals,
            VkFence fence) noexcept {
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

[[nodiscard]] RendererExpected<bool> VulkanQueue::present(const VkPresentInfoKHR &info) noexcept {
            auto lock = submission_lock_.lock();
            VkResult res = vkQueuePresentKHR(handle_, &info);
            if (res == VK_SUCCESS)
                return false;
            if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
                return true;
            if (res == VK_ERROR_DEVICE_LOST)
                return graphics_backend_error(GraphicsBackendErrorCode::DeviceLost, "vkQueuePresentKHR reported device loss.");
            return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkQueuePresentKHR failed.");
        }

[[nodiscard]] RendererResult VulkanQueue::wait_idle() noexcept {
            auto lock = submission_lock_.lock();
            const VkResult result = vkQueueWaitIdle(handle_);
            if (result == VK_ERROR_DEVICE_LOST)
                return graphics_backend_error(GraphicsBackendErrorCode::DeviceLost, "vkQueueWaitIdle reported device loss.");
            if (result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkQueueWaitIdle failed.");
            return {};
        }

[[nodiscard]] RendererResult VulkanQueue::bind_sparse(span<const VkBindSparseInfo> infos,
                                                 VkFence fence) noexcept {
            auto lock = submission_lock_.lock();
            const VkResult result = vkQueueBindSparse(handle_, static_cast<u32>(infos.size()), infos.data(), fence);
            if (result == VK_ERROR_DEVICE_LOST)
                return graphics_backend_error(GraphicsBackendErrorCode::DeviceLost, "vkQueueBindSparse reported device loss.");
            if (result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkQueueBindSparse failed.");
            return {};
        }

} // namespace SFT::Core::Vulkan
