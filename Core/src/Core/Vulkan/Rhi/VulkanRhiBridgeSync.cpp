// RHI submission and explicit synchronization objects backed by Vulkan queues/fences/timeline semaphores.
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <span>
#include <utility>
#include <vector>
#pragma endregion

#include <Foundation/src/Foundation.hpp>

#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/VulkanQueue.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridge.hpp>
#include <Core/Vulkan/VulkanRhiConvert.hpp>
#include <Core/Vulkan/VulkanSync.hpp>
#include <RHI/RHI.hpp>

using std::span;
using std::vector;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    namespace {

        [[nodiscard]] constexpr bool same_queue_lane(rhi::QueueLane lhs, rhi::QueueLane rhs) noexcept {
            return lhs.queue == rhs.queue && lhs.index == rhs.index;
        }

    } // namespace

    rhi::RhiResult VulkanRhiDeviceBridge::submit(const rhi::SubmitDesc &desc) {
        if (graphics_queue_ == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                                  "Vulkan RHI bridge cannot run submit: device resources are not ready.");
        }
        if (auto queue_valid = validate_queue_lane(desc.queue, "submit"); !queue_valid.has_value()) {
            return queue_valid;
        }
        VulkanQueue *queue = queue_for_lane(desc.queue);
        if (queue == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "submit: queue lane is not available.");
        }

        vector<VkCommandBufferSubmitInfo> command_buffers;
        command_buffers.reserve(desc.command_buffers.size());
        for (const rhi::CommandBufferHandle handle : desc.command_buffers) {
            CommandBufferRecord *record = command_buffers_.find(handle);
            if (record == nullptr) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "submit: unknown command buffer handle.");
            }
            if (!same_queue_lane(record->queue, desc.queue)) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "submit: command buffer was recorded for a different queue lane than SubmitDesc::queue.");
            }
            command_buffers.push_back(record->command_buffer.submit_info());
        }

        vector<VkSemaphoreSubmitInfo> waits;
        waits.reserve(desc.waits.size() + desc.presented_textures.size());
        for (const rhi::QueueSemaphoreWait &wait : desc.waits) {
            VulkanSemaphore *semaphore = semaphores_.find(wait.semaphore);
            if (semaphore == nullptr) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "submit: unknown wait semaphore handle.");
            }
            waits.push_back(semaphore->submit_info(to_vk(wait.stages), wait.value));
        }

        vector<VkSemaphoreSubmitInfo> signals;
        signals.reserve(desc.signals.size() + desc.presented_textures.size());
        for (const rhi::QueueSemaphoreSignal &signal : desc.signals) {
            VulkanSemaphore *semaphore = semaphores_.find(signal.semaphore);
            if (semaphore == nullptr) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "submit: unknown signal semaphore handle.");
            }
            signals.push_back(semaphore->submit_info(to_vk(signal.stages), signal.value));
        }

        for (const rhi::SurfaceTexture &texture : desc.presented_textures) {
            SwapchainRecord *swapchain = swapchains_.find(texture.swapchain);
            if (swapchain == nullptr) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "submit: presented texture references an unknown swapchain.");
            }
            if (texture.image_index >= swapchain->image_available_semaphores.size() ||
                texture.image_index >= swapchain->render_finished_semaphores.size()) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "submit: presented texture image index is out of range.");
            }
            const u32 image_available_index = swapchain->image_available_signal_indices[texture.image_index];
            waits.push_back(swapchain->image_available_semaphores[image_available_index].submit_info(
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0));
            signals.push_back(swapchain->render_finished_semaphores[texture.image_index].submit_info(
                VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, 0));
        }

        VkFence fence = VK_NULL_HANDLE;
        if (desc.fence.is_valid()) {
            VulkanFence *record = fences_.find(desc.fence);
            if (record == nullptr) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "submit: unknown fence handle.");
            }
            fence = record->vk_handle();
        }

        const VkSubmitInfo2 submit_info{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .waitSemaphoreInfoCount = static_cast<u32>(waits.size()),
            .pWaitSemaphoreInfos = waits.empty() ? nullptr : waits.data(),
            .commandBufferInfoCount = static_cast<u32>(command_buffers.size()),
            .pCommandBufferInfos = command_buffers.empty() ? nullptr : command_buffers.data(),
            .signalSemaphoreInfoCount = static_cast<u32>(signals.size()),
            .pSignalSemaphoreInfos = signals.empty() ? nullptr : signals.data(),
        };

        if (auto submitted = queue->submit(span{&submit_info, 1}, fence); !submitted) {
            return rhi_error_from_graphics(submitted.error());
        }
        return {};
    }

    rhi::RhiExpected<rhi::SemaphoreHandle> VulkanRhiDeviceBridge::create_semaphore(const rhi::SemaphoreDesc &desc) {
        if (logical_device_ == nullptr) {
            return device_not_ready<rhi::SemaphoreHandle>("create_semaphore");
        }
        auto semaphore = VulkanSemaphore::create_timeline(logical_device_->vk_handle(), desc.initial_value);
        if (!semaphore) {
            return rhi_error_from_graphics(semaphore.error());
        }
        return semaphores_.insert(std::move(*semaphore));
    }

    void VulkanRhiDeviceBridge::destroy_semaphore(rhi::SemaphoreHandle handle) noexcept {
        semaphores_.erase(handle);
    }

    rhi::RhiExpected<u64> VulkanRhiDeviceBridge::semaphore_value(rhi::SemaphoreHandle handle) const {
        const VulkanSemaphore *semaphore = semaphores_.find(handle);
        if (semaphore == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "semaphore_value: unknown semaphore handle.");
        }
        auto value = semaphore->counter_value();
        if (!value) {
            return rhi_error_from_graphics(value.error());
        }
        return *value;
    }

    rhi::RhiResult VulkanRhiDeviceBridge::wait_semaphore(rhi::SemaphoreHandle handle, u64 value, u64 timeout_ns) {
        VulkanSemaphore *semaphore = semaphores_.find(handle);
        if (semaphore == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "wait_semaphore: unknown semaphore handle.");
        }
        if (auto waited = semaphore->wait(value, timeout_ns); !waited) {
            return rhi_error_from_graphics(waited.error());
        }
        return {};
    }

    rhi::RhiResult VulkanRhiDeviceBridge::signal_semaphore(rhi::SemaphoreHandle handle, u64 value) {
        VulkanSemaphore *semaphore = semaphores_.find(handle);
        if (semaphore == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "signal_semaphore: unknown semaphore handle.");
        }
        if (auto signaled = semaphore->signal(value); !signaled) {
            return rhi_error_from_graphics(signaled.error());
        }
        return {};
    }

    rhi::RhiExpected<rhi::FenceHandle> VulkanRhiDeviceBridge::create_fence(const rhi::FenceDesc &desc) {
        if (logical_device_ == nullptr) {
            return device_not_ready<rhi::FenceHandle>("create_fence");
        }
        auto fence = VulkanFence::create(logical_device_->vk_handle(), desc.signaled);
        if (!fence) {
            return rhi_error_from_graphics(fence.error());
        }
        return fences_.insert(std::move(*fence));
    }

    void VulkanRhiDeviceBridge::destroy_fence(rhi::FenceHandle handle) noexcept {
        fences_.erase(handle);
    }

    rhi::RhiResult VulkanRhiDeviceBridge::wait_fences(span<const rhi::FenceHandle> fences, bool wait_all, u64 timeout_ns) {
        if (logical_device_ == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                                  "Vulkan RHI bridge cannot run wait_fences: device resources are not ready.");
        }
        vector<VkFence> vk_fences;
        vk_fences.reserve(fences.size());
        for (const rhi::FenceHandle handle : fences) {
            VulkanFence *fence = fences_.find(handle);
            if (fence == nullptr) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "wait_fences: unknown fence handle.");
            }
            vk_fences.push_back(fence->vk_handle());
        }
        const VkResult result = vkWaitForFences(logical_device_->vk_handle(), static_cast<u32>(vk_fences.size()),
                                                vk_fences.data(), wait_all ? VK_TRUE : VK_FALSE, timeout_ns);
        if (result != VK_SUCCESS && result != VK_TIMEOUT) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed, "wait_fences: vkWaitForFences failed.");
        }
        return {};
    }

    rhi::RhiResult VulkanRhiDeviceBridge::reset_fences(span<const rhi::FenceHandle> fences) {
        if (logical_device_ == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                                  "Vulkan RHI bridge cannot run reset_fences: device resources are not ready.");
        }
        vector<VkFence> vk_fences;
        vk_fences.reserve(fences.size());
        for (const rhi::FenceHandle handle : fences) {
            VulkanFence *fence = fences_.find(handle);
            if (fence == nullptr) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "reset_fences: unknown fence handle.");
            }
            vk_fences.push_back(fence->vk_handle());
        }
        if (vkResetFences(logical_device_->vk_handle(), static_cast<u32>(vk_fences.size()), vk_fences.data()) != VK_SUCCESS) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed, "reset_fences: vkResetFences failed.");
        }
        return {};
    }

} // namespace SFT::Core::Vulkan
