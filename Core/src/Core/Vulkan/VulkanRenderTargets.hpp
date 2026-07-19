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
#include <span>
#include <utility>
#include <vector>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>
#include <Core/Vulkan/VulkanImage.hpp>
#include <Core/Vulkan/VulkanRendering.hpp>

using SFT::Core::graphics_backend_error;
using SFT::Core::RendererExpected;
using std::span;
using std::vector;

namespace SFT::Core::Vulkan {

    [[nodiscard]] constexpr VkImageAspectFlags default_aspect_for_format(VkFormat format) noexcept {
        switch (format) {
            case VK_FORMAT_D16_UNORM:
            case VK_FORMAT_X8_D24_UNORM_PACK32:
            case VK_FORMAT_D32_SFLOAT:
                return VK_IMAGE_ASPECT_DEPTH_BIT;
            case VK_FORMAT_S8_UINT:
                return VK_IMAGE_ASPECT_STENCIL_BIT;
            case VK_FORMAT_D16_UNORM_S8_UINT:
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            default:
                return VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }

    struct VulkanImageViewDesc {
        VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT;
        u32 base_mip = 0;
        u32 mip_count = 1;
        u32 base_layer = 0;
        u32 layer_count = 1;
        VkComponentMapping components{
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        };

        [[nodiscard]] VkImageViewCreateInfo to_vk(VkImage image) const noexcept;
    };

    struct VulkanAttachmentImageDesc {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent3D extent{.width = 1, .height = 1, .depth = 1};
        VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VkImageAspectFlags aspects = 0;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        u32 mip_levels = 1;
        u32 array_layers = 1;
        VkImageType image_type = VK_IMAGE_TYPE_2D;
        VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
        VkImageCreateFlags image_flags = 0;
        VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
        VkImageLayout initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaMemoryUsage memory_usage = VMA_MEMORY_USAGE_AUTO;
        VmaAllocationCreateFlags allocation_flags = 0;
        const void *image_pnext = nullptr;
        const void *view_pnext = nullptr;
    };

    struct VulkanAttachmentRef {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent3D extent{};
        VkImageUsageFlags usage = 0;
        VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        u32 mip_levels = 1;
        u32 array_layers = 1;

        [[nodiscard]] bool is_valid() const noexcept;
        [[nodiscard]] VkExtent2D extent_2d() const noexcept;
    };

    class VulkanAttachmentImage {
      public:
        VulkanAttachmentImage() = default;
        ~VulkanAttachmentImage() = default;

        VulkanAttachmentImage(const VulkanAttachmentImage &) = delete;
        VulkanAttachmentImage &operator=(const VulkanAttachmentImage &) = delete;
        VulkanAttachmentImage(VulkanAttachmentImage &&) noexcept = default;
        VulkanAttachmentImage &operator=(VulkanAttachmentImage &&) noexcept = default;

        [[nodiscard]] static RendererExpected<VulkanAttachmentImage> create(
            VkDevice device,
            VmaAllocator allocator,
            const VulkanAttachmentImageDesc &desc) noexcept;

        [[nodiscard]] static RendererExpected<VulkanAttachmentImage> create_color(
            VkDevice device,
            VmaAllocator allocator,
            VkFormat format,
            VkExtent2D extent,
            VkImageUsageFlags extra_usage = VK_IMAGE_USAGE_SAMPLED_BIT,
            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
            u32 mip_levels = 1) noexcept;

        [[nodiscard]] static RendererExpected<VulkanAttachmentImage> create_depth(
            VkDevice device,
            VmaAllocator allocator,
            VkFormat format,
            VkExtent2D extent,
            VkImageUsageFlags extra_usage = VK_IMAGE_USAGE_SAMPLED_BIT,
            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) noexcept {
            return create(device, allocator, VulkanAttachmentImageDesc{
                .format = format,
                .extent = {.width = extent.width, .height = extent.height, .depth = 1},
                .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | extra_usage,
                .aspects = default_aspect_for_format(format),
                .samples = samples,
            });
        }

        [[nodiscard]] const VulkanImage &image() const noexcept;
        [[nodiscard]] const VulkanImageView &view() const noexcept;
        [[nodiscard]] const VulkanAttachmentRef &ref() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;

      private:
        VulkanImage image_;
        VulkanImageView view_;
        VulkanAttachmentRef ref_{};
    };

    // Lightweight pass target description: references attachment views but does not own them. A render
    // graph can build one per pass for G-buffer, lighting, bloom down/up-sample, tonemap, or present.
    class VulkanRenderTarget {
      public:
        VulkanRenderTarget() = default;

        VulkanRenderTarget &set_extent(VkExtent2D extent) noexcept;
        VulkanRenderTarget &set_samples(VkSampleCountFlagBits samples) noexcept;
        VulkanRenderTarget &add_color(const VulkanAttachmentRef &attachment);
        VulkanRenderTarget &set_colors(span<const VulkanAttachmentRef> attachments);
        VulkanRenderTarget &set_depth_stencil(const VulkanAttachmentRef &attachment) noexcept;

        [[nodiscard]] span<const VulkanAttachmentRef> colors() const noexcept;
        [[nodiscard]] const VulkanAttachmentRef *depth_stencil() const noexcept;
        [[nodiscard]] VkExtent2D extent() const noexcept;
        [[nodiscard]] VkRect2D render_area() const noexcept;
        [[nodiscard]] VkSampleCountFlagBits samples() const noexcept;
        [[nodiscard]] bool has_depth_stencil() const noexcept;
        [[nodiscard]] bool empty() const noexcept;

      private:
        vector<VulkanAttachmentRef> color_;
        VulkanAttachmentRef depth_stencil_{};
        bool has_depth_stencil_ = false;
        VkExtent2D extent_{};
        VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    };

    [[nodiscard]] constexpr ColorAttachment color_attachment(
        const VulkanAttachmentRef &attachment,
        VkAttachmentLoadOp load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
        VkAttachmentStoreOp store_op = VK_ATTACHMENT_STORE_OP_STORE,
        VkClearColorValue clear = {{0.0f, 0.0f, 0.0f, 1.0f}},
        VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) noexcept {
        return ColorAttachment{
            .view = attachment.view,
            .layout = layout,
            .load_op = load_op,
            .store_op = store_op,
            .clear_color = clear,
        };
    }

    [[nodiscard]] constexpr ColorAttachment color_resolve_attachment(
        const VulkanAttachmentRef &multisampled_attachment,
        const VulkanAttachmentRef &resolve_attachment,
        VkResolveModeFlagBits resolve_mode = VK_RESOLVE_MODE_AVERAGE_BIT,
        VkAttachmentLoadOp load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
        VkAttachmentStoreOp store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        VkClearColorValue clear = {{0.0f, 0.0f, 0.0f, 1.0f}}) noexcept {
        return ColorAttachment{
            .view = multisampled_attachment.view,
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .load_op = load_op,
            .store_op = store_op,
            .clear_color = clear,
            .resolve_mode = resolve_mode,
            .resolve_view = resolve_attachment.view,
            .resolve_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
    }

    [[nodiscard]] constexpr DepthAttachment depth_attachment(
        const VulkanAttachmentRef &attachment,
        VkAttachmentLoadOp load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
        VkAttachmentStoreOp store_op = VK_ATTACHMENT_STORE_OP_STORE,
        float clear_depth = 1.0f,
        VkImageLayout layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) noexcept {
        return DepthAttachment{
            .view = attachment.view,
            .layout = layout,
            .load_op = load_op,
            .store_op = store_op,
            .clear_depth = clear_depth,
        };
    }

    [[nodiscard]] constexpr StencilAttachment stencil_attachment(
        const VulkanAttachmentRef &attachment,
        VkAttachmentLoadOp load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
        VkAttachmentStoreOp store_op = VK_ATTACHMENT_STORE_OP_STORE,
        u32 clear_stencil = 0,
        VkImageLayout layout = VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL) noexcept {
        return StencilAttachment{
            .view = attachment.view,
            .layout = layout,
            .load_op = load_op,
            .store_op = store_op,
            .clear_stencil = clear_stencil,
        };
    }

} // namespace SFT::Core::Vulkan
