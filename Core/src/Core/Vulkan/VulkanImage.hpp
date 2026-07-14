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
        ~VulkanImageView() { destroy(); }

        VulkanImageView(const VulkanImageView &) = delete;
        VulkanImageView &operator=(const VulkanImageView &) = delete;

        VulkanImageView(VulkanImageView &&o) noexcept
            : device_(o.device_), view_(o.view_), format_(o.format_), view_type_(o.view_type_) {
            o.device_ = VK_NULL_HANDLE;
            o.view_ = VK_NULL_HANDLE;
        }
        VulkanImageView &operator=(VulkanImageView &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                view_ = o.view_;
                format_ = o.format_;
                view_type_ = o.view_type_;
                o.device_ = VK_NULL_HANDLE;
                o.view_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        [[nodiscard]] static RendererExpected<VulkanImageView> create(
            VkDevice device,
            const VkImageViewCreateInfo &info) noexcept {
            VkImageView view = VK_NULL_HANDLE;
            if (vkCreateImageView(device, &info, nullptr, &view) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateImageView failed.");
            VulkanImageView out;
            out.device_ = device;
            out.view_ = view;
            out.format_ = info.format;
            out.view_type_ = info.viewType;
            return out;
        }

        [[nodiscard]] VkImageView vk_handle() const noexcept { return view_; }
        [[nodiscard]] bool is_valid() const noexcept { return view_ != VK_NULL_HANDLE; }
        [[nodiscard]] VkFormat format() const noexcept { return format_; }
        [[nodiscard]] VkImageViewType view_type() const noexcept { return view_type_; }

        void destroy() noexcept {
            if (view_ == VK_NULL_HANDLE)
                return;
            vkDestroyImageView(device_, view_, nullptr);
            view_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

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
        ~VulkanImage() { destroy(); }

        VulkanImage(const VulkanImage &) = delete;
        VulkanImage &operator=(const VulkanImage &) = delete;

        VulkanImage(VulkanImage &&o) noexcept
            : device_(o.device_), allocator_(o.allocator_), image_(o.image_), allocation_(o.allocation_),
              format_(o.format_), extent_(o.extent_), mip_levels_(o.mip_levels_),
              array_layers_(o.array_layers_), image_type_(o.image_type_),
              usage_(o.usage_), owns_image_(o.owns_image_) {
            o.device_ = VK_NULL_HANDLE;
            o.allocator_ = VK_NULL_HANDLE;
            o.image_ = VK_NULL_HANDLE;
            o.allocation_ = VK_NULL_HANDLE;
        }
        VulkanImage &operator=(VulkanImage &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                allocator_ = o.allocator_;
                image_ = o.image_;
                allocation_ = o.allocation_;
                format_ = o.format_;
                extent_ = o.extent_;
                mip_levels_ = o.mip_levels_;
                array_layers_ = o.array_layers_;
                image_type_ = o.image_type_;
                usage_ = o.usage_;
                owns_image_ = o.owns_image_;
                o.device_ = VK_NULL_HANDLE;
                o.allocator_ = VK_NULL_HANDLE;
                o.image_ = VK_NULL_HANDLE;
                o.allocation_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        [[nodiscard]] static RendererExpected<VulkanImage> create(
            VkDevice device,
            const VkImageCreateInfo &info) noexcept {
            VkImage image = VK_NULL_HANDLE;
            if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateImage failed.");
            VulkanImage out;
            out.device_ = device;
            out.image_ = image;
            out.format_ = info.format;
            out.extent_ = info.extent;
            out.mip_levels_ = info.mipLevels;
            out.array_layers_ = info.arrayLayers;
            out.image_type_ = info.imageType;
            out.usage_ = info.usage;
            return out;
        }

        [[nodiscard]] static VulkanImage borrow(
            VkDevice device,
            VkImage image,
            VkFormat format,
            VkExtent3D extent,
            VkImageUsageFlags usage,
            u32 mip_levels = 1,
            u32 array_layers = 1,
            VkImageType image_type = VK_IMAGE_TYPE_2D) noexcept {
            VulkanImage out;
            out.device_ = device;
            out.image_ = image;
            out.format_ = format;
            out.extent_ = extent;
            out.mip_levels_ = mip_levels;
            out.array_layers_ = array_layers;
            out.image_type_ = image_type;
            out.usage_ = usage;
            out.owns_image_ = false;
            return out;
        }

        [[nodiscard]] static RendererExpected<VulkanImage> create(
            VkDevice device,
            VmaAllocator allocator,
            const VkImageCreateInfo &image_info,
            const VmaAllocationCreateInfo &allocation_info) noexcept {
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            if (vmaCreateImage(allocator, &image_info, &allocation_info, &image, &allocation, nullptr) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaCreateImage failed.");
            VulkanImage out;
            out.device_ = device;
            out.allocator_ = allocator;
            out.image_ = image;
            out.allocation_ = allocation;
            out.format_ = image_info.format;
            out.extent_ = image_info.extent;
            out.mip_levels_ = image_info.mipLevels;
            out.array_layers_ = image_info.arrayLayers;
            out.image_type_ = image_info.imageType;
            out.usage_ = image_info.usage;
            return out;
        }

        [[nodiscard]] VkImage vk_handle() const noexcept { return image_; }
        [[nodiscard]] VmaAllocation allocation() const noexcept { return allocation_; }
        [[nodiscard]] bool is_valid() const noexcept { return image_ != VK_NULL_HANDLE; }
        [[nodiscard]] bool owns_allocation() const noexcept { return allocation_ != VK_NULL_HANDLE; }
        [[nodiscard]] VkFormat format() const noexcept { return format_; }
        [[nodiscard]] VkExtent3D extent() const noexcept { return extent_; }
        [[nodiscard]] u32 mip_levels() const noexcept { return mip_levels_; }
        [[nodiscard]] u32 array_layers() const noexcept { return array_layers_; }
        [[nodiscard]] VkImageType image_type() const noexcept { return image_type_; }
        [[nodiscard]] VkImageUsageFlags usage() const noexcept { return usage_; }

        [[nodiscard]] VkMemoryRequirements memory_requirements() const noexcept {
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(device_, image_, &req);
            return req;
        }

        [[nodiscard]] VkMemoryRequirements2 memory_requirements2() const noexcept {
            VkImageMemoryRequirementsInfo2 query{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
                .pNext = nullptr,
                .image = image_,
            };
            VkMemoryRequirements2 req{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = nullptr};
            vkGetImageMemoryRequirements2(device_, &query, &req);
            return req;
        }

        [[nodiscard]] RendererResult bind_memory(VkDeviceMemory memory,
                                                 VkDeviceSize offset = 0) noexcept {
            if (vkBindImageMemory(device_, image_, memory, offset) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkBindImageMemory failed.");
            return {};
        }

        [[nodiscard]] VkSubresourceLayout subresource_layout(
            const VkImageSubresource &subresource) const noexcept {
            VkSubresourceLayout layout{};
            vkGetImageSubresourceLayout(device_, image_, &subresource, &layout);
            return layout;
        }

        // Convenience: create a view for this image with common defaults.
        [[nodiscard]] RendererExpected<VulkanImageView> create_view(
            VkImageAspectFlags aspect,
            VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D,
            u32 base_mip = 0,
            u32 mip_count = VK_REMAINING_MIP_LEVELS,
            u32 base_layer = 0,
            u32 layer_count = VK_REMAINING_ARRAY_LAYERS) const noexcept {
            VkImageViewCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .image = image_,
                .viewType = view_type,
                .format = format_,
                .components = {
                    VK_COMPONENT_SWIZZLE_IDENTITY,
                    VK_COMPONENT_SWIZZLE_IDENTITY,
                    VK_COMPONENT_SWIZZLE_IDENTITY,
                    VK_COMPONENT_SWIZZLE_IDENTITY,
                },
                .subresourceRange = {
                    .aspectMask = aspect,
                    .baseMipLevel = base_mip,
                    .levelCount = mip_count,
                    .baseArrayLayer = base_layer,
                    .layerCount = layer_count,
                },
            };
            return VulkanImageView::create(device_, info);
        }

        void destroy() noexcept {
            if (image_ == VK_NULL_HANDLE)
                return;

            if (allocation_ != VK_NULL_HANDLE) {
                vmaDestroyImage(allocator_, image_, allocation_);
                allocation_ = VK_NULL_HANDLE;
                allocator_ = VK_NULL_HANDLE;
            } else if (owns_image_) {
                vkDestroyImage(device_, image_, nullptr);
            }

            image_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
            format_ = VK_FORMAT_UNDEFINED;
            extent_ = {};
            mip_levels_ = 1;
            array_layers_ = 1;
            image_type_ = VK_IMAGE_TYPE_2D;
            usage_ = 0;
            owns_image_ = true;
        }

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
