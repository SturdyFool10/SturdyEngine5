

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

#include <Foundation/Foundation.hpp>

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

#include <tracy/Tracy.hpp>

using std::span;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    /// Creates a buffer from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::BufferHandle> VulkanRhiDeviceBridge::create_buffer(const rhi::BufferDesc &desc) {
        ZoneScopedN("VulkanRhiDeviceBridge::create_buffer");
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

        // Queried once here (rather than on every write_buffer()/map_buffer() call) — see
        // BufferRecord::host_visible's own doc comment for why a DeviceLocal buffer's placement isn't
        // knowable ahead of time (opportunistic Resizable BAR vs. plain device-local fallback).
        const bool host_visible = buffer->is_host_visible();
        return buffers_.insert(BufferRecord{std::move(*buffer), desc.memory, host_visible});
    }

    /// Destroys the buffer identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void VulkanRhiDeviceBridge::destroy_buffer(rhi::BufferHandle handle) noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::destroy_buffer");
        buffers_.erase(handle);
    }

    /// Writes buffer to the associated destination.
    ///
    /// @param buffer Buffer used or affected by the operation.
    /// @param offset Offset from the beginning of the relevant range or buffer.
    /// @param data Data consumed or referenced by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::InvalidArgument`.
    rhi::RhiResult VulkanRhiDeviceBridge::write_buffer(rhi::BufferHandle buffer, u64 offset, span<const std::byte> data) {
        ZoneScopedN("VulkanRhiDeviceBridge::write_buffer");
        BufferRecord *record = buffers_.find(buffer);
        if (record == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "write_buffer: unknown buffer handle.");
        }

        // A DeviceLocal buffer that opportunistically landed in HOST_VISIBLE memory (Resizable BAR —
        // see VulkanRhiConvert.hpp's to_vma() and BufferRecord::host_visible's own doc comment) writes
        // directly below, exactly like HostUpload, skipping the staging-buffer-and-copy round trip (a
        // fresh buffer allocation, a GPU submit, and a synchronous fence wait) entirely. Only a
        // DeviceLocal buffer that landed in plain device-local memory — always true without Resizable
        // BAR — needs that path.
        if (record->memory == rhi::MemoryLocation::DeviceLocal && !record->host_visible) {
            return upload_via_staging(record->buffer, offset, data);
        }

        if (auto uploaded = record->buffer.upload(data.data(), data.size(), offset); !uploaded) {
            return rhi_error_from_graphics(uploaded.error());
        }
        return {};
    }

    /// Maps buffer for access.
    ///
    /// @param buffer Buffer used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::InvalidArgument`.
    rhi::RhiExpected<span<std::byte>> VulkanRhiDeviceBridge::map_buffer(rhi::BufferHandle buffer) {
        ZoneScopedN("VulkanRhiDeviceBridge::map_buffer");
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

    /// Unmaps buffer.
    ///
    /// @param buffer Buffer used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void VulkanRhiDeviceBridge::unmap_buffer(rhi::BufferHandle buffer) noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::unmap_buffer");
        if (BufferRecord *record = buffers_.find(buffer)) {
            record->buffer.unmap();
        }
    }

    /// Acquires upload resources.
    ///
    /// @return Returns the current acquire upload resources value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    VulkanRhiDeviceBridge::UploadResources VulkanRhiDeviceBridge::acquire_upload_resources() {
        ZoneScopedN("VulkanRhiDeviceBridge::acquire_upload_resources");
        {
            auto pool = upload_pool_.lock();
            if (!pool->empty()) {
                UploadResources resources = std::move(pool->back());
                pool->pop_back();
                return resources;
            }
        }
        UploadResources resources{};
        if (auto pool_created = VulkanCommandPool::create(logical_device_->vk_handle(), graphics_queue_->family_index());
            pool_created.has_value()) {
            resources.command_pool = std::move(*pool_created);
        }
        if (auto fence = VulkanFence::create(logical_device_->vk_handle()); fence.has_value()) {
            resources.fence = std::move(*fence);
        }
        return resources;
    }

    /// Releases upload resources using the supplied arguments and current state.
    ///
    /// @param resources `resources` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void VulkanRhiDeviceBridge::release_upload_resources(UploadResources resources) noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::release_upload_resources");
        if (!resources.command_pool.is_valid() || !resources.fence.is_valid()) {
            return;
        }
        upload_pool_.lock()->push_back(std::move(resources));
    }

    /// Uploads via staging using the supplied arguments and current state.
    ///
    /// @param destination Destination value or resource.
    /// @param offset Offset from the beginning of the relevant range or buffer.
    /// @param data Data consumed or referenced by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::OperationFailed`.
    rhi::RhiResult VulkanRhiDeviceBridge::upload_via_staging(VulkanBuffer &destination, u64 offset, span<const std::byte> data) {
        ZoneScopedN("VulkanRhiDeviceBridge::upload_via_staging");
        if (allocator_ == nullptr || logical_device_ == nullptr || graphics_queue_ == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                                  "Vulkan RHI bridge cannot run upload_via_staging: device resources are not ready.");
        }

        UploadResources upload = acquire_upload_resources();
        if (!upload.command_pool.is_valid() || !upload.fence.is_valid()) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                                  "upload_via_staging: failed to create a staging command pool/fence.");
        }


        struct ReleaseGuard {
            VulkanRhiDeviceBridge &bridge;
            UploadResources &resources;
            ~ReleaseGuard() { bridge.release_upload_resources(std::move(resources)); }
        } release_guard{*this, upload};

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

        auto command_buffer = VulkanCommandBuffer::allocate(logical_device_->vk_handle(), upload.command_pool.vk_handle());
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

        if (auto reset = upload.fence.reset(); !reset) {
            return rhi_error_from_graphics(reset.error());
        }
        if (auto submitted = graphics_queue_->submit(command_buffer->submit_info(), {}, {}, upload.fence.vk_handle());
            !submitted) {
            return rhi_error_from_graphics(submitted.error());
        }
        if (auto waited = upload.fence.wait(); !waited) {
            return rhi_error_from_graphics(waited.error());
        }

        if (auto pool_reset = upload.command_pool.reset(); !pool_reset) {
            return rhi_error_from_graphics(pool_reset.error());
        }
        return {};
    }

} // namespace SFT::Core::Vulkan
