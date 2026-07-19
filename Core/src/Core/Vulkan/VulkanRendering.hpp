#pragma once

#include <Foundation/src/Foundation.hpp>
#pragma region Imports
#include "volk.h"
#include <span>
#include <vector>
#pragma endregion

using std::span;
using std::vector;

namespace SFT::Core::Vulkan {

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

        [[nodiscard]] VkRenderingAttachmentInfo to_vk() const noexcept;
    };

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
        RenderingInfo &set_render_area(VkRect2D area) noexcept;
        RenderingInfo &set_layer_count(u32 count) noexcept;
        // Non-zero enables multiview; view_mask bits correspond to view indices.
        RenderingInfo &set_view_mask(u32 mask) noexcept;
        RenderingInfo &set_flags(VkRenderingFlags flags) noexcept;
        RenderingInfo &add_flags(VkRenderingFlags flags) noexcept;
        RenderingInfo &set_next(const void *next) noexcept;
        RenderingInfo &suspend() noexcept;
        RenderingInfo &resume() noexcept;

        RenderingInfo &add_color(ColorAttachment att);
        RenderingInfo &set_color(u32 location, ColorAttachment att);
        RenderingInfo &set_unused_color(u32 location);
        RenderingInfo &set_colors(span<const ColorAttachment> attachments);
        RenderingInfo &set_depth(DepthAttachment att) noexcept;
        RenderingInfo &set_stencil(StencilAttachment att) noexcept;

        // Returns a VkRenderingInfo backed by storage in this object.
        // Valid until the next call to build() or destruction of this RenderingInfo.
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
        ScopedRenderingPass() = default;
        ScopedRenderingPass(VkCommandBuffer command_buffer, const VkRenderingInfo &info) noexcept;
        ~ScopedRenderingPass();

        ScopedRenderingPass(const ScopedRenderingPass &) = delete;
        ScopedRenderingPass &operator=(const ScopedRenderingPass &) = delete;

        ScopedRenderingPass(ScopedRenderingPass &&other) noexcept;
        ScopedRenderingPass &operator=(ScopedRenderingPass &&other) noexcept;

        void end() noexcept;

        [[nodiscard]] bool active() const noexcept;

      private:
        VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
        bool active_ = false;
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

    struct DynamicRenderingSignature {
        vector<VkFormat> color_formats;
        VkFormat depth_format = VK_FORMAT_UNDEFINED;
        VkFormat stencil_format = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        u32 view_mask = 0;

        [[nodiscard]] bool has_depth_stencil() const noexcept;
    };

    class PipelineRenderingInfo {
      public:
        PipelineRenderingInfo() = default;
        explicit PipelineRenderingInfo(const DynamicRenderingSignature &signature);

        PipelineRenderingInfo &add_color_format(VkFormat fmt);
        PipelineRenderingInfo &set_color_format(u32 location, VkFormat fmt);
        PipelineRenderingInfo &set_color_formats(span<const VkFormat> formats);
        PipelineRenderingInfo &set_depth_format(VkFormat fmt) noexcept;
        PipelineRenderingInfo &set_stencil_format(VkFormat fmt) noexcept;
        PipelineRenderingInfo &set_depth_stencil_format(VkFormat fmt) noexcept;
        PipelineRenderingInfo &set_next(const void *next) noexcept;
        // Non-zero enables multiview — must match the RenderingInfo used at draw time.
        PipelineRenderingInfo &set_view_mask(u32 mask) noexcept;

        [[nodiscard]] DynamicRenderingSignature signature(VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) const;

        [[nodiscard]] VkPipelineRenderingCreateInfo to_vk() const noexcept;

      private:
        vector<VkFormat> color_formats_;
        VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
        VkFormat stencil_format_ = VK_FORMAT_UNDEFINED;
        u32 view_mask_ = 0;
        const void *pnext_ = nullptr;
    };

} // namespace SFT::Core::Vulkan
