// RhiDevice buffer resource creation/destruction plus the two upload paths: a direct mapped write for
// host-visible memory, and a staged blocking copy for DeviceLocal memory (see upload_via_staging).
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-extension"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <vk_mem_alloc.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <cstddef>
#include <mutex>
#include <span>
#include <utility>
#pragma endregion

#include <Foundation/src/Foundation.hpp>

#include <Core/GraphicsBackendError.hpp>
#include <Core/Vulkan/VulkanAllocator.hpp>
#include <Core/Vulkan/VulkanBuffer.hpp>
#include <Core/Vulkan/VulkanCommandBuffer.hpp>
#include <Core/Vulkan/VulkanCommandPool.hpp>
#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/VulkanQueue.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridge.hpp>
#include <Core/Vulkan/VulkanRhiConvert.hpp>
#include <Core/Vulkan/VulkanSync.hpp>
#include <RHI/RHI.hpp>

using std::span;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    rhi::RhiExpected<rhi::BufferHandle> VulkanRhiDeviceBridge::create_buffer(const rhi::BufferDesc &desc) {
        if (allocator_ == nullptr || logical_device_ == nullptr) {
            return device_not_ready<rhi::BufferHandle>("create_buffer");
        }

        const VkBufferCreateInfo buffer_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = static_cast<VkDeviceSize>(desc.size),
            .usage = to_vk(desc.usage),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VmaMapping mapping = to_vma(desc.memory);
        const VmaAllocationCreateInfo alloc_info{
            .flags = mapping.flags,
            .usage = mapping.usage,
        };

        auto buffer = allocator_->create_buffer(logical_device_->vk_handle(), buffer_info, alloc_info);
        if (!buffer) {
            return rhi_error_from_graphics(buffer.error());
        }

        return buffers_.insert(BufferRecord{std::move(*buffer), desc.memory});
    }

    void VulkanRhiDeviceBridge::destroy_buffer(rhi::BufferHandle handle) noexcept {
        buffers_.erase(handle);
    }

    rhi::RhiResult VulkanRhiDeviceBridge::write_buffer(rhi::BufferHandle buffer, u64 offset, span<const std::byte> data) {
        BufferRecord *record = buffers_.find(buffer);
        if (record == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "write_buffer: unknown buffer handle.");
        }

        if (record->memory == rhi::MemoryLocation::DeviceLocal) {
            return upload_via_staging(record->buffer, offset, data);
        }

        if (auto uploaded = record->buffer.upload(data.data(), data.size(), offset); !uploaded) {
            return rhi_error_from_graphics(uploaded.error());
        }
        return {};
    }

    rhi::RhiExpected<span<std::byte>> VulkanRhiDeviceBridge::map_buffer(rhi::BufferHandle buffer) {
        BufferRecord *record = buffers_.find(buffer);
        if (record == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "map_buffer: unknown buffer handle.");
        }
        if (record->memory == rhi::MemoryLocation::DeviceLocal) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                  "map_buffer: buffer is DeviceLocal memory, which is not host-mappable.");
        }

        auto mapped = record->buffer.map();
        if (!mapped) {
            return rhi_error_from_graphics(mapped.error());
        }
        return span<std::byte>(static_cast<std::byte *>(*mapped), static_cast<usize>(record->buffer.size()));
    }

    void VulkanRhiDeviceBridge::unmap_buffer(rhi::BufferHandle buffer) noexcept {
        if (BufferRecord *record = buffers_.find(buffer)) {
            record->buffer.unmap();
        }
    }

    rhi::RhiResult VulkanRhiDeviceBridge::upload_via_staging(VulkanBuffer &destination, u64 offset, span<const std::byte> data) {
        auto upload = upload_.lock();
        if (allocator_ == nullptr || logical_device_ == nullptr || graphics_queue_ == nullptr ||
            !upload->command_pool.is_valid() || !upload->fence.is_valid()) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                                  "Vulkan RHI bridge cannot run upload_via_staging: device resources are not ready.");
        }

        const VkBufferCreateInfo staging_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = static_cast<VkDeviceSize>(data.size()),
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VmaAllocationCreateInfo staging_alloc_info{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        auto staging = allocator_->create_buffer(logical_device_->vk_handle(), staging_info, staging_alloc_info);
        if (!staging) {
            return rhi_error_from_graphics(staging.error());
        }
        if (auto uploaded = staging->upload(data.data(), data.size()); !uploaded) {
            return rhi_error_from_graphics(uploaded.error());
        }

        auto command_buffer = VulkanCommandBuffer::allocate(logical_device_->vk_handle(), upload->command_pool.vk_handle());
        if (!command_buffer) {
            return rhi_error_from_graphics(command_buffer.error());
        }
        if (auto began = command_buffer->begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT); !began) {
            return rhi_error_from_graphics(began.error());
        }
        command_buffer->copy_buffer(staging->vk_handle(), destination.vk_handle(), data.size(), 0, offset);
        if (auto ended = command_buffer->end(); !ended) {
            return rhi_error_from_graphics(ended.error());
        }

        if (auto reset = upload->fence.reset(); !reset) {
            return rhi_error_from_graphics(reset.error());
        }
        if (auto submitted = graphics_queue_->submit(command_buffer->submit_info(), {}, {}, upload->fence.vk_handle());
            !submitted) {
            return rhi_error_from_graphics(submitted.error());
        }
        if (auto waited = upload->fence.wait(); !waited) {
            return rhi_error_from_graphics(waited.error());
        }

        if (auto pool_reset = upload->command_pool.reset(); !pool_reset) {
            return rhi_error_from_graphics(pool_reset.error());
        }
        return {};
    }

} // namespace SFT::Core::Vulkan
