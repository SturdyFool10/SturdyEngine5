#pragma once

#include <Foundation/src/Foundation.hpp>
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
#pragma endregion

#include <Core/GraphicsBackendError.hpp>

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;

namespace SFT::Core::Vulkan {

    // ─── VulkanImageView ─────────────────────────────────────────────────────────

    class VulkanImageView {
      public:
        VulkanImageView() = default;
        ~VulkanImageView();

        VulkanImageView(const VulkanImageView &) = delete;
        VulkanImageView &operator=(const VulkanImageView &) = delete;

        VulkanImageView(VulkanImageView &&o) noexcept;
        VulkanImageView &operator=(VulkanImageView &&o) noexcept;

        [[nodiscard]] static RendererExpected<VulkanImageView> create(
            VkDevice device,
            const VkImageViewCreateInfo &info) noexcept;

        [[nodiscard]] VkImageView vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;
        [[nodiscard]] VkFormat format() const noexcept;
        [[nodiscard]] VkImageViewType view_type() const noexcept;

        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkImageView view_ = VK_NULL_HANDLE;
        VkFormat format_ = VK_FORMAT_UNDEFINED;
        VkImageViewType view_type_ = VK_IMAGE_VIEW_TYPE_2D;
    };

    // ─── VulkanImage ─────────────────────────────────────────────────────────────

    // Owns a VkImage. When created through VMA, the matching VmaAllocation is owned here too.
    class VulkanImage {
      public:
        VulkanImage() = default;
        ~VulkanImage();

        VulkanImage(const VulkanImage &) = delete;
        VulkanImage &operator=(const VulkanImage &) = delete;

        VulkanImage(VulkanImage &&o) noexcept;
        VulkanImage &operator=(VulkanImage &&o) noexcept;

        [[nodiscard]] static RendererExpected<VulkanImage> create(
            VkDevice device,
            const VkImageCreateInfo &info) noexcept;

        [[nodiscard]] static VulkanImage borrow(
            VkDevice device,
            VkImage image,
            VkFormat format,
            VkExtent3D extent,
            VkImageUsageFlags usage,
            u32 mip_levels = 1,
            u32 array_layers = 1,
            VkImageType image_type = VK_IMAGE_TYPE_2D) noexcept;

        [[nodiscard]] static RendererExpected<VulkanImage> create(
            VkDevice device,
            VmaAllocator allocator,
            const VkImageCreateInfo &image_info,
            const VmaAllocationCreateInfo &allocation_info) noexcept;

        [[nodiscard]] VkImage vk_handle() const noexcept;
        [[nodiscard]] VmaAllocation allocation() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;
        [[nodiscard]] bool owns_allocation() const noexcept;
        [[nodiscard]] VkFormat format() const noexcept;
        [[nodiscard]] VkExtent3D extent() const noexcept;
        [[nodiscard]] u32 mip_levels() const noexcept;
        [[nodiscard]] u32 array_layers() const noexcept;
        [[nodiscard]] VkImageType image_type() const noexcept;
        [[nodiscard]] VkImageUsageFlags usage() const noexcept;

        [[nodiscard]] VkMemoryRequirements memory_requirements() const noexcept;

        [[nodiscard]] VkMemoryRequirements2 memory_requirements2() const noexcept;

        [[nodiscard]] RendererResult bind_memory(VkDeviceMemory memory,
                                                 VkDeviceSize offset = 0) noexcept;

        [[nodiscard]] VkSubresourceLayout subresource_layout(
            const VkImageSubresource &subresource) const noexcept;

        // Convenience: create a view for this image with common defaults.
        [[nodiscard]] RendererExpected<VulkanImageView> create_view(
            VkImageAspectFlags aspect,
            VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D,
            u32 base_mip = 0,
            u32 mip_count = VK_REMAINING_MIP_LEVELS,
            u32 base_layer = 0,
            u32 layer_count = VK_REMAINING_ARRAY_LAYERS) const noexcept;

        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VmaAllocator allocator_ = VK_NULL_HANDLE;
        VkImage image_ = VK_NULL_HANDLE;
        VmaAllocation allocation_ = VK_NULL_HANDLE;
        VkFormat format_ = VK_FORMAT_UNDEFINED;
        VkExtent3D extent_ = {};
        u32 mip_levels_ = 1;
        u32 array_layers_ = 1;
        VkImageType image_type_ = VK_IMAGE_TYPE_2D;
        VkImageUsageFlags usage_ = 0;
        bool owns_image_ = true;
    };

} // namespace SFT::Core::Vulkan
