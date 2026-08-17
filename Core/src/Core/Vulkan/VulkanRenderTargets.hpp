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

    /// Performs the default aspect for format operation using the supplied arguments.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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

        /// Converts the supplied engine/RHI value to its Vulkan representation.
        ///
        /// @param image `image` value used by the operation.
        ///
        /// @return Returns the value converted to Vulkan representation.
        /// @note This function does not throw exceptions.
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

        /// Reports whether valid holds for this `VulkanAttachmentRef`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;
        /// Returns the current or globally available extent 2d value.
        ///
        /// @return Returns the current extent 2d value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkExtent2D extent_2d() const noexcept;
    };

    class VulkanAttachmentImage {
      public:
        /// Constructs a `VulkanAttachmentImage` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanAttachmentImage() = default;
        /// Destroys the `VulkanAttachmentImage` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanAttachmentImage() = default;

        /// Disables this construction form for `VulkanAttachmentImage`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanAttachmentImage(const VulkanAttachmentImage &) = delete;
        /// Assigns a new value to this `VulkanAttachmentImage`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanAttachmentImage &operator=(const VulkanAttachmentImage &) = delete;
        /// Constructs a `VulkanAttachmentImage` from another instance.
        ///
        /// @note This function does not throw exceptions.
        VulkanAttachmentImage(VulkanAttachmentImage &&) noexcept = default;
        /// Assigns a new value to this `VulkanAttachmentImage`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanAttachmentImage &operator=(VulkanAttachmentImage &&) noexcept = default;

        /// Creates a `VulkanAttachmentImage` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param allocator Allocator used for storage owned by the operation.
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanAttachmentImage> create(
            VkDevice device,
            VmaAllocator allocator,
            const VulkanAttachmentImageDesc &desc) noexcept;

        /// Creates a color from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param allocator Allocator used for storage owned by the operation.
        /// @param format Format used for the resource, render target, or conversion.
        /// @param extent `extent` value used by the operation.
        /// @param extra_usage Usage flags or category applied to the resource.
        /// @param samples `samples` value used by the operation.
        /// @param mip_levels `mip_levels` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanAttachmentImage> create_color(
            VkDevice device,
            VmaAllocator allocator,
            VkFormat format,
            VkExtent2D extent,
            VkImageUsageFlags extra_usage = VK_IMAGE_USAGE_SAMPLED_BIT,
            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
            u32 mip_levels = 1) noexcept;

        /// Creates a depth from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param allocator Allocator used for storage owned by the operation.
        /// @param format Format used for the resource, render target, or conversion.
        /// @param extent `extent` value used by the operation.
        /// @param extra_usage Usage flags or category applied to the resource.
        /// @param samples `samples` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanAttachmentImage> create_depth(
            VkDevice device,
            VmaAllocator allocator,
            VkFormat format,
            VkExtent2D extent,
            VkImageUsageFlags extra_usage = VK_IMAGE_USAGE_SAMPLED_BIT,
            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) noexcept;

        /// Returns the current or globally available image value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const VulkanImage &image() const noexcept;
        /// Returns the current or globally available view value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const VulkanImageView &view() const noexcept;
        /// Returns the current or globally available ref value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const VulkanAttachmentRef &ref() const noexcept;
        /// Reports whether valid holds for this `VulkanAttachmentImage`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;

      private:
        VulkanImage image_;
        VulkanImageView view_;
        VulkanAttachmentRef ref_{};
    };


    class VulkanRenderTarget {
      public:
        /// Constructs a `VulkanRenderTarget` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanRenderTarget() = default;

        /// Sets the extent for this `VulkanRenderTarget`.
        ///
        /// @param extent `extent` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanRenderTarget &set_extent(VkExtent2D extent) noexcept;
        /// Sets the samples for this `VulkanRenderTarget`.
        ///
        /// @param samples `samples` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanRenderTarget &set_samples(VkSampleCountFlagBits samples) noexcept;
        /// Adds color using the supplied arguments and current state.
        ///
        /// @param attachment `attachment` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        VulkanRenderTarget &add_color(const VulkanAttachmentRef &attachment);
        /// Sets the colors for this `VulkanRenderTarget`.
        ///
        /// @param attachments `attachments` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        VulkanRenderTarget &set_colors(span<const VulkanAttachmentRef> attachments);
        /// Sets the depth stencil for this `VulkanRenderTarget`.
        ///
        /// @param attachment `attachment` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanRenderTarget &set_depth_stencil(const VulkanAttachmentRef &attachment) noexcept;

        /// Returns the current or globally available colors value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] span<const VulkanAttachmentRef> colors() const noexcept;
        /// Returns the current or globally available depth stencil value.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const VulkanAttachmentRef *depth_stencil() const noexcept;
        /// Returns the current or globally available extent value.
        ///
        /// @return Returns the current extent value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkExtent2D extent() const noexcept;
        /// Renders area using the current rendering state.
        ///
        /// @return Returns the current render area value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkRect2D render_area() const noexcept;
        /// Returns the current or globally available samples value.
        ///
        /// @return Returns the current samples value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkSampleCountFlagBits samples() const noexcept;
        /// Reports whether this `VulkanRenderTarget` has depth stencil.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool has_depth_stencil() const noexcept;
        /// Reports whether this `VulkanRenderTarget` contains no elements or payload.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool empty() const noexcept;

      private:
        vector<VulkanAttachmentRef> color_;
        VulkanAttachmentRef depth_stencil_{};
        bool has_depth_stencil_ = false;
        VkExtent2D extent_{};
        VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    };

    /// Performs the color attachment operation using the supplied arguments.
    ///
    /// @param attachment `attachment` value used by the operation.
    /// @param load_op `load_op` value used by the operation.
    /// @param store_op `store_op` value used by the operation.
    /// @param clear `clear` value used by the operation.
    /// @param layout `layout` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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

    /// Performs the color resolve attachment operation using the supplied arguments.
    ///
    /// @param multisampled_attachment `multisampled_attachment` value used by the operation.
    /// @param resolve_attachment `resolve_attachment` value used by the operation.
    /// @param resolve_mode Mode controlling how the operation is performed.
    /// @param load_op `load_op` value used by the operation.
    /// @param store_op `store_op` value used by the operation.
    /// @param clear `clear` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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

    /// Performs the depth attachment operation using the supplied arguments.
    ///
    /// @param attachment `attachment` value used by the operation.
    /// @param load_op `load_op` value used by the operation.
    /// @param store_op `store_op` value used by the operation.
    /// @param clear_depth `clear_depth` value used by the operation.
    /// @param layout `layout` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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

    /// Performs the stencil attachment operation for `Vulkan` using the supplied arguments.
    ///
    /// @param attachment `attachment` value used by the operation.
    /// @param load_op `load_op` value used by the operation.
    /// @param store_op `store_op` value used by the operation.
    /// @param clear_stencil `clear_stencil` value used by the operation.
    /// @param layout `layout` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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
