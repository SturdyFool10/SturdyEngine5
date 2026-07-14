#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-extension"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <vk_mem_alloc.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <cstddef>
#include <cstring>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;

namespace SFT::Core::Vulkan {

    // Owns a VkBuffer. When created through VMA, the matching VmaAllocation is owned here too —
    // otherwise memory is intentionally not managed here; bind it via
    // VulkanDevice::bind_buffer_memory or through VMA instead.
    class VulkanBuffer {
      public:
        VulkanBuffer() = default;
        ~VulkanBuffer() { destroy(); }

        VulkanBuffer(const VulkanBuffer &) = delete;
        VulkanBuffer &operator=(const VulkanBuffer &) = delete;

        VulkanBuffer(VulkanBuffer &&o) noexcept
            : device_(o.device_), allocator_(o.allocator_), buffer_(o.buffer_), allocation_(o.allocation_),
              size_(o.size_), usage_(o.usage_) {
            o.device_ = VK_NULL_HANDLE;
            o.allocator_ = VK_NULL_HANDLE;
            o.buffer_ = VK_NULL_HANDLE;
            o.allocation_ = VK_NULL_HANDLE;
            o.size_ = 0;
        }
        VulkanBuffer &operator=(VulkanBuffer &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                allocator_ = o.allocator_;
                buffer_ = o.buffer_;
                allocation_ = o.allocation_;
                size_ = o.size_;
                usage_ = o.usage_;
                o.device_ = VK_NULL_HANDLE;
                o.allocator_ = VK_NULL_HANDLE;
                o.buffer_ = VK_NULL_HANDLE;
                o.allocation_ = VK_NULL_HANDLE;
                o.size_ = 0;
            }
            return *this;
        }

        [[nodiscard]] static RendererExpected<VulkanBuffer> create(
            VkDevice device,
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            VkBufferCreateFlags flags = 0,
            VkSharingMode sharing = VK_SHARING_MODE_EXCLUSIVE) noexcept {
            VkBufferCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = flags,
                .size = size,
                .usage = usage,
                .sharingMode = sharing,
            };
            VkBuffer buf = VK_NULL_HANDLE;
            if (vkCreateBuffer(device, &info, nullptr, &buf) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateBuffer failed.");
            VulkanBuffer out;
            out.device_ = device;
            out.buffer_ = buf;
            out.size_ = size;
            out.usage_ = usage;
            return out;
        }

        [[nodiscard]] static RendererExpected<VulkanBuffer> create(
            VkDevice device,
            VmaAllocator allocator,
            const VkBufferCreateInfo &buffer_info,
            const VmaAllocationCreateInfo &allocation_info) noexcept {
            VkBuffer buf = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            if (vmaCreateBuffer(allocator, &buffer_info, &allocation_info, &buf, &allocation, nullptr) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaCreateBuffer failed.");
            VulkanBuffer out;
            out.device_ = device;
            out.allocator_ = allocator;
            out.buffer_ = buf;
            out.allocation_ = allocation;
            out.size_ = buffer_info.size;
            out.usage_ = buffer_info.usage;
            return out;
        }

        [[nodiscard]] VkBuffer vk_handle() const noexcept { return buffer_; }
        [[nodiscard]] VmaAllocation allocation() const noexcept { return allocation_; }
        [[nodiscard]] bool is_valid() const noexcept { return buffer_ != VK_NULL_HANDLE; }
        [[nodiscard]] bool owns_allocation() const noexcept { return allocation_ != VK_NULL_HANDLE; }
        [[nodiscard]] VkDeviceSize size() const noexcept { return size_; }
        [[nodiscard]] VkBufferUsageFlags usage() const noexcept { return usage_; }

        // Maps, memcpy's `bytes` from `data` at `offset`, then unmaps. Only valid for a VMA-backed
        // buffer created with a host-visible `VmaAllocationCreateInfo` (e.g. a staging buffer) —
        // there is no allocation to map otherwise.
        [[nodiscard]] RendererResult upload(const void *data, VkDeviceSize bytes, VkDeviceSize offset = 0) noexcept {
            void *mapped = nullptr;
            if (vmaMapMemory(allocator_, allocation_, &mapped) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaMapMemory failed.");
            std::memcpy(static_cast<std::byte *>(mapped) + offset, data, bytes);
            vmaUnmapMemory(allocator_, allocation_);
            return {};
        }

        // Symmetric readback: maps, copies `bytes` out into `dst` at `offset`, then unmaps. Only valid
        // for a host-visible VMA allocation (a HostReadback buffer the GPU copied results into).
        [[nodiscard]] RendererResult download(void *dst, VkDeviceSize bytes, VkDeviceSize offset = 0) noexcept {
            void *mapped = nullptr;
            if (vmaMapMemory(allocator_, allocation_, &mapped) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaMapMemory failed.");
            std::memcpy(dst, static_cast<const std::byte *>(mapped) + offset, bytes);
            vmaUnmapMemory(allocator_, allocation_);
            return {};
        }

        // Persistent map/unmap for direct CPU access — the backing of the RHI's map_buffer. VMA
        // reference-counts nested maps, so pair each map() with an unmap(). The pointer is valid for
        // the whole allocation; the caller offsets into it. Only valid for a host-visible allocation.
        [[nodiscard]] RendererExpected<void *> map() noexcept {
            void *mapped = nullptr;
            if (vmaMapMemory(allocator_, allocation_, &mapped) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaMapMemory failed.");
            return mapped;
        }
        void unmap() noexcept {
            if (allocation_ != VK_NULL_HANDLE) {
                vmaUnmapMemory(allocator_, allocation_);
            }
        }

        // Flush/invalidate a mapped range for non-coherent host memory (no-op cost on coherent memory,
        // which VMA reports; call around non-coherent readback/upload to make writes visible).
        [[nodiscard]] RendererResult flush(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept {
            if (vmaFlushAllocation(allocator_, allocation_, offset, size) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaFlushAllocation failed.");
            return {};
        }
        [[nodiscard]] RendererResult invalidate(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept {
            if (vmaInvalidateAllocation(allocator_, allocation_, offset, size) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaInvalidateAllocation failed.");
            return {};
        }

        [[nodiscard]] VkMemoryRequirements memory_requirements() const noexcept {
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(device_, buffer_, &req);
            return req;
        }

        [[nodiscard]] VkMemoryRequirements2 memory_requirements2() const noexcept {
            VkBufferMemoryRequirementsInfo2 query{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
                .pNext = nullptr,
                .buffer = buffer_,
            };
            VkMemoryRequirements2 req{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = nullptr};
            vkGetBufferMemoryRequirements2(device_, &query, &req);
            return req;
        }

        [[nodiscard]] RendererResult bind_memory(VkDeviceMemory memory,
                                                 VkDeviceSize offset = 0) noexcept {
            if (vkBindBufferMemory(device_, buffer_, memory, offset) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkBindBufferMemory failed.");
            return {};
        }

        // Requires VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT.
        [[nodiscard]] VkDeviceAddress device_address() const noexcept {
            VkBufferDeviceAddressInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .pNext = nullptr,
                .buffer = buffer_,
            };
            return vkGetBufferDeviceAddress(device_, &info);
        }

        void destroy() noexcept {
            if (buffer_ == VK_NULL_HANDLE)
                return;

            if (allocation_ != VK_NULL_HANDLE) {
                vmaDestroyBuffer(allocator_, buffer_, allocation_);
                allocation_ = VK_NULL_HANDLE;
                allocator_ = VK_NULL_HANDLE;
            } else {
                vkDestroyBuffer(device_, buffer_, nullptr);
            }

            buffer_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
            size_ = 0;
            usage_ = 0;
        }

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VmaAllocator allocator_ = VK_NULL_HANDLE;
        VkBuffer buffer_ = VK_NULL_HANDLE;
        VmaAllocation allocation_ = VK_NULL_HANDLE;
        VkDeviceSize size_ = 0;
        VkBufferUsageFlags usage_ = 0;
    };

    // ─── VulkanBufferView ─────────────────────────────────────────────────────────

    // Owns a VkBufferView — the typed window over a buffer range that a uniform/storage *texel* buffer
    // binding reads through (the buffer must carry the matching UNIFORM/STORAGE_TEXEL_BUFFER usage). The
    // buffer must outlive the view.
    class VulkanBufferView {
      public:
        VulkanBufferView() = default;
        ~VulkanBufferView() { destroy(); }

        VulkanBufferView(const VulkanBufferView &) = delete;
        VulkanBufferView &operator=(const VulkanBufferView &) = delete;

        VulkanBufferView(VulkanBufferView &&o) noexcept
            : device_(o.device_), view_(o.view_), format_(o.format_) {
            o.device_ = VK_NULL_HANDLE;
            o.view_ = VK_NULL_HANDLE;
        }
        VulkanBufferView &operator=(VulkanBufferView &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                view_ = o.view_;
                format_ = o.format_;
                o.device_ = VK_NULL_HANDLE;
                o.view_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        [[nodiscard]] static RendererExpected<VulkanBufferView> create(
            VkDevice device,
            VkBuffer buffer,
            VkFormat format,
            VkDeviceSize offset = 0,
            VkDeviceSize range = VK_WHOLE_SIZE) noexcept {
            VkBufferViewCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .buffer = buffer,
                .format = format,
                .offset = offset,
                .range = range,
            };
            VkBufferView view = VK_NULL_HANDLE;
            if (vkCreateBufferView(device, &info, nullptr, &view) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateBufferView failed.");
            VulkanBufferView out;
            out.device_ = device;
            out.view_ = view;
            out.format_ = format;
            return out;
        }

        [[nodiscard]] VkBufferView vk_handle() const noexcept { return view_; }
        [[nodiscard]] bool is_valid() const noexcept { return view_ != VK_NULL_HANDLE; }
        [[nodiscard]] VkFormat format() const noexcept { return format_; }

        void destroy() noexcept {
            if (view_ == VK_NULL_HANDLE)
                return;
            vkDestroyBufferView(device_, view_, nullptr);
            view_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkBufferView view_ = VK_NULL_HANDLE;
        VkFormat format_ = VK_FORMAT_UNDEFINED;
    };

} // namespace SFT::Core::Vulkan
