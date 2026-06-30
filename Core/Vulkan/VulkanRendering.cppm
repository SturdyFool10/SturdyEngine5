module;
#include "volk.h"
#include <vector>

export module Sturdy.Core:VulkanRendering;

import Sturdy.Foundation;

using std::vector;

export namespace SFT::Core::Vulkan {

    // ─── Attachment helpers ───────────────────────────────────────────────────────
    //
    // These convert to VkRenderingAttachmentInfo (Vulkan 1.3+ dynamic rendering).
    // Pass them to RenderingInfo below rather than constructing the Vk structs by hand.

    struct ColorAttachment {
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentLoadOp load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkAttachmentStoreOp store_op = VK_ATTACHMENT_STORE_OP_STORE;
        VkClearColorValue clear_color = {};
        // Optional MSAA resolve target.
        VkResolveModeFlagBits resolve_mode = VK_RESOLVE_MODE_NONE;
        VkImageView resolve_view = VK_NULL_HANDLE;
        VkImageLayout resolve_layout = VK_IMAGE_LAYOUT_UNDEFINED;

        [[nodiscard]] VkRenderingAttachmentInfo to_vk() const noexcept {
            VkClearValue cv{};
            cv.color = clear_color;
            return {
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .pNext = nullptr,
                .imageView = view,
                .imageLayout = layout,
                .resolveMode = resolve_mode,
                .resolveImageView = resolve_view,
                .resolveImageLayout = resolve_layout,
                .loadOp = load_op,
                .storeOp = store_op,
                .clearValue = cv,
            };
        }
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

        [[nodiscard]] VkRenderingAttachmentInfo to_vk() const noexcept {
            VkClearValue cv{};
            cv.depthStencil.depth = clear_depth;
            return {
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .pNext = nullptr,
                .imageView = view,
                .imageLayout = layout,
                .resolveMode = resolve_mode,
                .resolveImageView = resolve_view,
                .resolveImageLayout = resolve_layout,
                .loadOp = load_op,
                .storeOp = store_op,
                .clearValue = cv,
            };
        }
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

        [[nodiscard]] VkRenderingAttachmentInfo to_vk() const noexcept {
            VkClearValue cv{};
            cv.depthStencil.stencil = clear_stencil;
            return {
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .pNext = nullptr,
                .imageView = view,
                .imageLayout = layout,
                .resolveMode = resolve_mode,
                .resolveImageView = resolve_view,
                .resolveImageLayout = resolve_layout,
                .loadOp = load_op,
                .storeOp = store_op,
                .clearValue = cv,
            };
        }
    };

    // ─── RenderingInfo ────────────────────────────────────────────────────────────
    //
    // Builder for VkRenderingInfo passed to vkCmdBeginRendering.
    // The builder must remain alive until vkCmdBeginRendering returns — build()
    // returns a VkRenderingInfo whose pointers are owned by this object.
    //
    // Typical usage:
    //   auto ri = RenderingInfo{}
    //       .set_render_area({{0,0}, {width, height}})
    //       .add_color(ColorAttachment{ .view = view, .clear_color = {{0,0,0,1}} })
    //       .set_depth(DepthAttachment{ .view = depth_view });
    //   vkCmdBeginRendering(cmd, &ri.build());
    //   // ... draw calls ...
    //   vkCmdEndRendering(cmd);

    class RenderingInfo {
      public:
        RenderingInfo &set_render_area(VkRect2D area) noexcept {
            render_area_ = area;
            return *this;
        }
        RenderingInfo &set_layer_count(u32 count) noexcept {
            layer_count_ = count;
            return *this;
        }
        // Non-zero enables multiview; view_mask bits correspond to view indices.
        RenderingInfo &set_view_mask(u32 mask) noexcept {
            view_mask_ = mask;
            return *this;
        }
        RenderingInfo &set_flags(VkRenderingFlags flags) noexcept {
            flags_ = flags;
            return *this;
        }

        RenderingInfo &add_color(ColorAttachment att) {
            color_attachments_vk_.push_back(att.to_vk());
            return *this;
        }
        RenderingInfo &set_depth(DepthAttachment att) noexcept {
            depth_vk_ = att.to_vk();
            has_depth_ = true;
            return *this;
        }
        RenderingInfo &set_stencil(StencilAttachment att) noexcept {
            stencil_vk_ = att.to_vk();
            has_stencil_ = true;
            return *this;
        }

        // Returns a VkRenderingInfo backed by storage in this object.
        // Valid until the next call to build() or destruction of this RenderingInfo.
        [[nodiscard]] VkRenderingInfo build() noexcept {
            return {
                .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .pNext = nullptr,
                .flags = flags_,
                .renderArea = render_area_,
                .layerCount = layer_count_,
                .viewMask = view_mask_,
                .colorAttachmentCount = static_cast<u32>(color_attachments_vk_.size()),
                .pColorAttachments = color_attachments_vk_.empty() ? nullptr
                                                                   : color_attachments_vk_.data(),
                .pDepthAttachment = has_depth_ ? &depth_vk_ : nullptr,
                .pStencilAttachment = has_stencil_ ? &stencil_vk_ : nullptr,
            };
        }

      private:
        VkRect2D render_area_ = {};
        u32 layer_count_ = 1;
        u32 view_mask_ = 0;
        VkRenderingFlags flags_ = 0;
        vector<VkRenderingAttachmentInfo> color_attachments_vk_;
        VkRenderingAttachmentInfo depth_vk_ = {};
        VkRenderingAttachmentInfo stencil_vk_ = {};
        bool has_depth_ = false;
        bool has_stencil_ = false;
    };

    // ─── PipelineRenderingInfo ────────────────────────────────────────────────────
    //
    // Describes the attachment formats for a graphics pipeline that uses dynamic rendering.
    // Place this in the pNext chain of VkGraphicsPipelineCreateInfo and set
    // VkGraphicsPipelineCreateInfo::renderPass = VK_NULL_HANDLE.
    //
    // The struct must remain alive until vkCreateGraphicsPipelines returns.
    //
    // Example:
    //   auto pri = PipelineRenderingInfo{}
    //       .add_color_format(VK_FORMAT_B8G8R8A8_SRGB)
    //       .set_depth_format(VK_FORMAT_D32_SFLOAT);
    //   VkGraphicsPipelineCreateInfo ci{ .pNext = &pri.to_vk(), .renderPass = VK_NULL_HANDLE, ... };

    class PipelineRenderingInfo {
      public:
        PipelineRenderingInfo &add_color_format(VkFormat fmt) {
            color_formats_.push_back(fmt);
            return *this;
        }
        PipelineRenderingInfo &set_depth_format(VkFormat fmt) noexcept {
            depth_format_ = fmt;
            return *this;
        }
        PipelineRenderingInfo &set_stencil_format(VkFormat fmt) noexcept {
            stencil_format_ = fmt;
            return *this;
        }
        // Non-zero enables multiview — must match the RenderingInfo used at draw time.
        PipelineRenderingInfo &set_view_mask(u32 mask) noexcept {
            view_mask_ = mask;
            return *this;
        }

        [[nodiscard]] VkPipelineRenderingCreateInfo to_vk() const noexcept {
            return {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                .pNext = nullptr,
                .viewMask = view_mask_,
                .colorAttachmentCount = static_cast<u32>(color_formats_.size()),
                .pColorAttachmentFormats = color_formats_.empty() ? nullptr : color_formats_.data(),
                .depthAttachmentFormat = depth_format_,
                .stencilAttachmentFormat = stencil_format_,
            };
        }

      private:
        vector<VkFormat> color_formats_;
        VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
        VkFormat stencil_format_ = VK_FORMAT_UNDEFINED;
        u32 view_mask_ = 0;
    };

} // namespace SFT::Core::Vulkan
