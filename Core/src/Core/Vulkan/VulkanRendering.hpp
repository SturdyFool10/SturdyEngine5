#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#include "volk.h"
#include <span>
#include <vector>
#pragma endregion

using std::span;
using std::vector;

namespace SFT::Core::Vulkan {


    struct ColorAttachment {
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentLoadOp load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkAttachmentStoreOp store_op = VK_ATTACHMENT_STORE_OP_STORE;
        VkClearColorValue clear_color = {};

        VkResolveModeFlagBits resolve_mode = VK_RESOLVE_MODE_NONE;
        VkImageView resolve_view = VK_NULL_HANDLE;
        VkImageLayout resolve_layout = VK_IMAGE_LAYOUT_UNDEFINED;

        /// Converts the supplied engine/RHI value to its Vulkan representation.
        ///
        /// @return Returns the current to Vulkan value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkRenderingAttachmentInfo to_vk() const noexcept;
    };

    struct DepthAttachment {
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        VkAttachmentLoadOp load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkAttachmentStoreOp store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        float clear_depth = 1.0f;
        VkResolveModeFlagBits resolve_mode = VK_RESOLVE_MODE_NONE;
        VkImageView resolve_view = VK_NULL_HANDLE;
        VkImageLayout resolve_layout = VK_IMAGE_LAYOUT_UNDEFINED;

        /// Converts the supplied engine/RHI value to its Vulkan representation.
        ///
        /// @return Returns the current to Vulkan value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkRenderingAttachmentInfo to_vk() const noexcept;
    };

    struct StencilAttachment {
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentLoadOp load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkAttachmentStoreOp store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        u32 clear_stencil = 0;
        VkResolveModeFlagBits resolve_mode = VK_RESOLVE_MODE_NONE;
        VkImageView resolve_view = VK_NULL_HANDLE;
        VkImageLayout resolve_layout = VK_IMAGE_LAYOUT_UNDEFINED;

        /// Converts the supplied engine/RHI value to its Vulkan representation.
        ///
        /// @return Returns the current to Vulkan value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkRenderingAttachmentInfo to_vk() const noexcept;
    };

    /// Returns the current or globally available unused rendering attachment value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr VkRenderingAttachmentInfo unused_rendering_attachment() noexcept {
        return VkRenderingAttachmentInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .pNext = nullptr,
            .imageView = VK_NULL_HANDLE,
            .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .resolveMode = VK_RESOLVE_MODE_NONE,
            .resolveImageView = VK_NULL_HANDLE,
            .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .clearValue = {},
        };
    }


    class RenderingInfo {
      public:
        /// Sets the render area for this `RenderingInfo`.
        ///
        /// @param area `area` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderingInfo &set_render_area(VkRect2D area) noexcept;
        /// Sets the layer count for this `RenderingInfo`.
        ///
        /// @param count Number of elements or operations to process.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderingInfo &set_layer_count(u32 count) noexcept;

        /// Sets the view mask for this `RenderingInfo`.
        ///
        /// @param mask `mask` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderingInfo &set_view_mask(u32 mask) noexcept;
        /// Sets the flags for this `RenderingInfo`.
        ///
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderingInfo &set_flags(VkRenderingFlags flags) noexcept;
        /// Adds flags using the supplied arguments and current state.
        ///
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderingInfo &add_flags(VkRenderingFlags flags) noexcept;
        /// Sets the next for this `RenderingInfo`.
        ///
        /// @param next `next` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderingInfo &set_next(const void *next) noexcept;
        /// Returns the current or globally available suspend value.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderingInfo &suspend() noexcept;
        /// Returns the current or globally available resume value.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderingInfo &resume() noexcept;

        /// Adds color using the supplied arguments and current state.
        ///
        /// @param att `att` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        RenderingInfo &add_color(ColorAttachment att);
        /// Sets the color for this `RenderingInfo`.
        ///
        /// @param location `location` value used by the operation.
        /// @param att `att` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        RenderingInfo &set_color(u32 location, ColorAttachment att);
        /// Sets the unused color for this `RenderingInfo`.
        ///
        /// @param location `location` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        RenderingInfo &set_unused_color(u32 location);
        /// Sets the colors for this `RenderingInfo`.
        ///
        /// @param attachments `attachments` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        RenderingInfo &set_colors(span<const ColorAttachment> attachments);
        /// Sets the depth for this `RenderingInfo`.
        ///
        /// @param att `att` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderingInfo &set_depth(DepthAttachment att) noexcept;
        /// Sets the stencil for this `RenderingInfo`.
        ///
        /// @param att `att` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderingInfo &set_stencil(StencilAttachment att) noexcept;


        /// Builds the requested object or derived state.
        ///
        /// @return Returns the current build value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkRenderingInfo build() noexcept;

      private:
        VkRect2D render_area_ = {};
        u32 layer_count_ = 1;
        u32 view_mask_ = 0;
        VkRenderingFlags flags_ = 0;
        const void *pnext_ = nullptr;
        vector<VkRenderingAttachmentInfo> color_attachments_vk_;
        VkRenderingAttachmentInfo depth_vk_ = {};
        VkRenderingAttachmentInfo stencil_vk_ = {};
        bool has_depth_ = false;
        bool has_stencil_ = false;
    };

    class ScopedRenderingPass {
      public:
        /// Constructs a `ScopedRenderingPass` in its default state.
        ///
        /// @note This function does not throw exceptions.
        ScopedRenderingPass() = default;
        /// Constructs a `ScopedRenderingPass` from the supplied initialization values.
        ///
        /// @param command_buffer Buffer used or affected by the operation.
        /// @param info Description of the resource or operation to perform.
        ///
        /// @note This function does not throw exceptions.
        ScopedRenderingPass(VkCommandBuffer command_buffer, const VkRenderingInfo &info) noexcept;
        /// Destroys the `ScopedRenderingPass` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~ScopedRenderingPass();

        /// Disables this construction form for `ScopedRenderingPass`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ScopedRenderingPass(const ScopedRenderingPass &) = delete;
        /// Assigns a new value to this `ScopedRenderingPass`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ScopedRenderingPass &operator=(const ScopedRenderingPass &) = delete;

        /// Constructs a `ScopedRenderingPass` from another instance.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @note This function does not throw exceptions.
        ScopedRenderingPass(ScopedRenderingPass &&other) noexcept;
        /// Assigns a new value to this `ScopedRenderingPass`.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        ScopedRenderingPass &operator=(ScopedRenderingPass &&other) noexcept;

        /// Returns the one-past-the-end iterator for the range.
        ///
        /// @note This function does not throw exceptions.
        void end() noexcept;

        /// Returns the current or globally available active value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool active() const noexcept;

      private:
        VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
        bool active_ = false;
    };


    struct DynamicRenderingSignature {
        vector<VkFormat> color_formats;
        VkFormat depth_format = VK_FORMAT_UNDEFINED;
        VkFormat stencil_format = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        u32 view_mask = 0;

        /// Reports whether this `DynamicRenderingSignature` has depth stencil.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool has_depth_stencil() const noexcept;
    };

    class PipelineRenderingInfo {
      public:
        /// Constructs a `PipelineRenderingInfo` in its default state.
        ///
        /// @note This function does not throw exceptions.
        PipelineRenderingInfo() = default;
        /// Constructs a `PipelineRenderingInfo` from the supplied initialization values.
        ///
        /// @param signature `signature` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit PipelineRenderingInfo(const DynamicRenderingSignature &signature);

        /// Adds color format using the supplied arguments and current state.
        ///
        /// @param fmt `fmt` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        PipelineRenderingInfo &add_color_format(VkFormat fmt);
        /// Sets the color format for this `PipelineRenderingInfo`.
        ///
        /// @param location `location` value used by the operation.
        /// @param fmt `fmt` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        PipelineRenderingInfo &set_color_format(u32 location, VkFormat fmt);
        /// Sets the color formats for this `PipelineRenderingInfo`.
        ///
        /// @param formats Format used for the resource, render target, or conversion.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        PipelineRenderingInfo &set_color_formats(span<const VkFormat> formats);
        /// Sets the depth format for this `PipelineRenderingInfo`.
        ///
        /// @param fmt `fmt` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        PipelineRenderingInfo &set_depth_format(VkFormat fmt) noexcept;
        /// Sets the stencil format for this `PipelineRenderingInfo`.
        ///
        /// @param fmt `fmt` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        PipelineRenderingInfo &set_stencil_format(VkFormat fmt) noexcept;
        /// Sets the depth stencil format for this `PipelineRenderingInfo`.
        ///
        /// @param fmt `fmt` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        PipelineRenderingInfo &set_depth_stencil_format(VkFormat fmt) noexcept;
        /// Sets the next for this `PipelineRenderingInfo`.
        ///
        /// @param next `next` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        PipelineRenderingInfo &set_next(const void *next) noexcept;

        /// Sets the view mask for this `PipelineRenderingInfo`.
        ///
        /// @param mask `mask` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        PipelineRenderingInfo &set_view_mask(u32 mask) noexcept;

        /// Performs the signature operation for `PipelineRenderingInfo` using the supplied arguments.
        ///
        /// @param samples `samples` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] DynamicRenderingSignature signature(VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) const;

        /// Converts the supplied engine/RHI value to its Vulkan representation.
        ///
        /// @return Returns the current to Vulkan value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkPipelineRenderingCreateInfo to_vk() const noexcept;

      private:
        vector<VkFormat> color_formats_;
        VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
        VkFormat stencil_format_ = VK_FORMAT_UNDEFINED;
        u32 view_mask_ = 0;
        const void *pnext_ = nullptr;
    };

} // namespace SFT::Core::Vulkan
