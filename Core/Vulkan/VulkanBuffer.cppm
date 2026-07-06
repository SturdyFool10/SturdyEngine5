module;
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

export module Sturdy.Core:VulkanBuffer;

import :RendererError;
import Sturdy.Foundation;

using SFT::Core::renderer_error;
using SFT::Core::RendererErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;

export namespace SFT::Core::Vulkan {

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
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateBuffer failed.");
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
                return renderer_error(RendererErrorCode::OperationFailed, "vmaCreateBuffer failed.");
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
                return renderer_error(RendererErrorCode::OperationFailed, "vmaMapMemory failed.");
            std::memcpy(static_cast<std::byte *>(mapped) + offset, data, bytes);
            vmaUnmapMemory(allocator_, allocation_);
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
                return renderer_error(RendererErrorCode::OperationFailed, "vkBindBufferMemory failed.");
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

} // namespace SFT::Core::Vulkan
