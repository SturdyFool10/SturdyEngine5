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
        ~VulkanBuffer();

        VulkanBuffer(const VulkanBuffer &) = delete;
        VulkanBuffer &operator=(const VulkanBuffer &) = delete;

        VulkanBuffer(VulkanBuffer &&o) noexcept;
        VulkanBuffer &operator=(VulkanBuffer &&o) noexcept;

        [[nodiscard]] static RendererExpected<VulkanBuffer> create(
            VkDevice device,
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            VkBufferCreateFlags flags = 0,
            VkSharingMode sharing = VK_SHARING_MODE_EXCLUSIVE) noexcept;

        [[nodiscard]] static RendererExpected<VulkanBuffer> create(
            VkDevice device,
            VmaAllocator allocator,
            const VkBufferCreateInfo &buffer_info,
            const VmaAllocationCreateInfo &allocation_info) noexcept;

        [[nodiscard]] VkBuffer vk_handle() const noexcept;
        [[nodiscard]] VmaAllocation allocation() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;
        [[nodiscard]] bool owns_allocation() const noexcept;
        [[nodiscard]] VkDeviceSize size() const noexcept;
        [[nodiscard]] VkBufferUsageFlags usage() const noexcept;

        // Maps, memcpy's `bytes` from `data` at `offset`, then unmaps. Only valid for a VMA-backed
        // buffer created with a host-visible `VmaAllocationCreateInfo` (e.g. a staging buffer) —
        // there is no allocation to map otherwise.
        [[nodiscard]] RendererResult upload(const void *data, VkDeviceSize bytes, VkDeviceSize offset = 0) noexcept;

        // Symmetric readback: maps, copies `bytes` out into `dst` at `offset`, then unmaps. Only valid
        // for a host-visible VMA allocation (a HostReadback buffer the GPU copied results into).
        [[nodiscard]] RendererResult download(void *dst, VkDeviceSize bytes, VkDeviceSize offset = 0) noexcept;

        // Persistent map/unmap for direct CPU access — the backing of the RHI's map_buffer. VMA
        // reference-counts nested maps, so pair each map() with an unmap(). The pointer is valid for
        // the whole allocation; the caller offsets into it. Only valid for a host-visible allocation.
        [[nodiscard]] RendererExpected<void *> map() noexcept;
        void unmap() noexcept;

        // Flush/invalidate a mapped range for non-coherent host memory (no-op cost on coherent memory,
        // which VMA reports; call around non-coherent readback/upload to make writes visible).
        [[nodiscard]] RendererResult flush(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept;
        [[nodiscard]] RendererResult invalidate(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept;

        [[nodiscard]] VkMemoryRequirements memory_requirements() const noexcept;

        [[nodiscard]] VkMemoryRequirements2 memory_requirements2() const noexcept;

        [[nodiscard]] RendererResult bind_memory(VkDeviceMemory memory,
                                                 VkDeviceSize offset = 0) noexcept;

        // Requires VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT.
        [[nodiscard]] VkDeviceAddress device_address() const noexcept;

        void destroy() noexcept;

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
        ~VulkanBufferView();

        VulkanBufferView(const VulkanBufferView &) = delete;
        VulkanBufferView &operator=(const VulkanBufferView &) = delete;

        VulkanBufferView(VulkanBufferView &&o) noexcept;
        VulkanBufferView &operator=(VulkanBufferView &&o) noexcept;

        [[nodiscard]] static RendererExpected<VulkanBufferView> create(
            VkDevice device,
            VkBuffer buffer,
            VkFormat format,
            VkDeviceSize offset = 0,
            VkDeviceSize range = VK_WHOLE_SIZE) noexcept;

        [[nodiscard]] VkBufferView vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;
        [[nodiscard]] VkFormat format() const noexcept;

        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkBufferView view_ = VK_NULL_HANDLE;
        VkFormat format_ = VK_FORMAT_UNDEFINED;
    };

} // namespace SFT::Core::Vulkan
