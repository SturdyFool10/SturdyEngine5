module;
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"

export module Sturdy.Core:VulkanBuffer;

import :RendererError;
import Sturdy.Foundation;

using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using SFT::Core::RendererErrorCode;
using SFT::Core::renderer_error;

export namespace SFT::Core::Vulkan {

// Owns a VkBuffer. Memory is intentionally not managed here — bind it via
// VulkanDevice::bind_buffer_memory or through VMA once initialized.
class VulkanBuffer {
  public:
    VulkanBuffer() = default;
    ~VulkanBuffer() { destroy(); }

    VulkanBuffer(const VulkanBuffer&)            = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;

    VulkanBuffer(VulkanBuffer&& o) noexcept
        : device_(o.device_), buffer_(o.buffer_), size_(o.size_), usage_(o.usage_) {
        o.device_ = VK_NULL_HANDLE;
        o.buffer_ = VK_NULL_HANDLE;
        o.size_   = 0;
    }
    VulkanBuffer& operator=(VulkanBuffer&& o) noexcept {
        if (this != &o) { destroy();
            device_ = o.device_; buffer_ = o.buffer_; size_ = o.size_; usage_ = o.usage_;
            o.device_ = VK_NULL_HANDLE; o.buffer_ = VK_NULL_HANDLE; o.size_ = 0; }
        return *this;
    }

    [[nodiscard]] static RendererExpected<VulkanBuffer> create(
        VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage,
        VkBufferCreateFlags flags = 0,
        VkSharingMode sharing    = VK_SHARING_MODE_EXCLUSIVE
    ) noexcept {
        VkBufferCreateInfo info{
            .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext       = nullptr,
            .flags       = flags,
            .size        = size,
            .usage       = usage,
            .sharingMode = sharing,
        };
        VkBuffer buf = VK_NULL_HANDLE;
        if (vkCreateBuffer(device, &info, nullptr, &buf) != VK_SUCCESS)
            return renderer_error(RendererErrorCode::OperationFailed, "vkCreateBuffer failed.");
        VulkanBuffer out;
        out.device_ = device;
        out.buffer_ = buf;
        out.size_   = size;
        out.usage_  = usage;
        return out;
    }

    [[nodiscard]] VkBuffer           vk_handle() const noexcept { return buffer_; }
    [[nodiscard]] bool               is_valid()  const noexcept { return buffer_ != VK_NULL_HANDLE; }
    [[nodiscard]] VkDeviceSize       size()      const noexcept { return size_; }
    [[nodiscard]] VkBufferUsageFlags usage()     const noexcept { return usage_; }

    [[nodiscard]] VkMemoryRequirements memory_requirements() const noexcept {
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, buffer_, &req);
        return req;
    }

    [[nodiscard]] VkMemoryRequirements2 memory_requirements2() const noexcept {
        VkBufferMemoryRequirementsInfo2 query{
            .sType  = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
            .pNext  = nullptr,
            .buffer = buffer_,
        };
        VkMemoryRequirements2 req{ .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = nullptr };
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
            .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .pNext  = nullptr,
            .buffer = buffer_,
        };
        return vkGetBufferDeviceAddress(device_, &info);
    }

    void destroy() noexcept {
        if (buffer_ == VK_NULL_HANDLE) return;
        vkDestroyBuffer(device_, buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        size_   = 0;
    }

  private:
    VkDevice           device_ = VK_NULL_HANDLE;
    VkBuffer           buffer_ = VK_NULL_HANDLE;
    VkDeviceSize       size_   = 0;
    VkBufferUsageFlags usage_  = 0;
};

} // namespace SFT::Core::Vulkan
