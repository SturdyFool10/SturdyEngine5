module;
#include <Foundation/Foundation.hpp>
#pragma region Imports
#include "volk.h"
#include <span>
#include <vector>
#pragma endregion

export module Sturdy.Core:VulkanRendering;


using std::span;
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
        RenderingInfo &add_flags(VkRenderingFlags flags) noexcept {
            flags_ |= flags;
            return *this;
        }
        RenderingInfo &set_next(const void *next) noexcept {
            pnext_ = next;
            return *this;
        }
        RenderingInfo &suspend() noexcept {
            flags_ |= VK_RENDERING_SUSPENDING_BIT;
            return *this;
        }
        RenderingInfo &resume() noexcept {
            flags_ |= VK_RENDERING_RESUMING_BIT;
            return *this;
        }

        RenderingInfo &add_color(ColorAttachment att) {
            color_attachments_vk_.push_back(att.to_vk());
            return *this;
        }
        RenderingInfo &set_color(u32 location, ColorAttachment att) {
            if (color_attachments_vk_.size() <= location) {
                color_attachments_vk_.resize(static_cast<usize>(location) + 1, unused_rendering_attachment());
            }
            color_attachments_vk_[location] = att.to_vk();
            return *this;
        }
        RenderingInfo &set_unused_color(u32 location) {
            if (color_attachments_vk_.size() <= location) {
                color_attachments_vk_.resize(static_cast<usize>(location) + 1, unused_rendering_attachment());
            }
            color_attachments_vk_[location] = unused_rendering_attachment();
            return *this;
        }
        RenderingInfo &set_colors(span<const ColorAttachment> attachments) {
            color_attachments_vk_.clear();
            color_attachments_vk_.reserve(attachments.size());
            for (const ColorAttachment &attachment : attachments) {
                color_attachments_vk_.push_back(attachment.to_vk());
            }
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
                .pNext = pnext_,
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
        ScopedRenderingPass(VkCommandBuffer command_buffer, const VkRenderingInfo &info) noexcept
            : command_buffer_(command_buffer), active_(command_buffer != VK_NULL_HANDLE) {
            if (active_) {
                vkCmdBeginRendering(command_buffer_, &info);
            }
        }
        ~ScopedRenderingPass() { end(); }

        ScopedRenderingPass(const ScopedRenderingPass &) = delete;
        ScopedRenderingPass &operator=(const ScopedRenderingPass &) = delete;

        ScopedRenderingPass(ScopedRenderingPass &&other) noexcept
            : command_buffer_(other.command_buffer_), active_(other.active_) {
            other.command_buffer_ = VK_NULL_HANDLE;
            other.active_ = false;
        }
        ScopedRenderingPass &operator=(ScopedRenderingPass &&other) noexcept {
            if (this != &other) {
                end();
                command_buffer_ = other.command_buffer_;
                active_ = other.active_;
                other.command_buffer_ = VK_NULL_HANDLE;
                other.active_ = false;
            }
            return *this;
        }

        void end() noexcept {
            if (!active_) {
                return;
            }
            vkCmdEndRendering(command_buffer_);
            active_ = false;
        }

        [[nodiscard]] bool active() const noexcept { return active_; }

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

        [[nodiscard]] bool has_depth_stencil() const noexcept {
            return depth_format != VK_FORMAT_UNDEFINED || stencil_format != VK_FORMAT_UNDEFINED;
        }
    };

    class PipelineRenderingInfo {
      public:
        PipelineRenderingInfo() = default;
        explicit PipelineRenderingInfo(const DynamicRenderingSignature &signature)
            : color_formats_(signature.color_formats), depth_format_(signature.depth_format),
              stencil_format_(signature.stencil_format), view_mask_(signature.view_mask) {}

        PipelineRenderingInfo &add_color_format(VkFormat fmt) {
            color_formats_.push_back(fmt);
            return *this;
        }
        PipelineRenderingInfo &set_color_format(u32 location, VkFormat fmt) {
            if (color_formats_.size() <= location) {
                color_formats_.resize(static_cast<usize>(location) + 1, VK_FORMAT_UNDEFINED);
            }
            color_formats_[location] = fmt;
            return *this;
        }
        PipelineRenderingInfo &set_color_formats(span<const VkFormat> formats) {
            color_formats_.assign(formats.begin(), formats.end());
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
        PipelineRenderingInfo &set_depth_stencil_format(VkFormat fmt) noexcept {
            depth_format_ = fmt;
            stencil_format_ = fmt;
            return *this;
        }
        PipelineRenderingInfo &set_next(const void *next) noexcept {
            pnext_ = next;
            return *this;
        }
        // Non-zero enables multiview — must match the RenderingInfo used at draw time.
        PipelineRenderingInfo &set_view_mask(u32 mask) noexcept {
            view_mask_ = mask;
            return *this;
        }

        [[nodiscard]] DynamicRenderingSignature signature(VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) const {
            return DynamicRenderingSignature{
                .color_formats = color_formats_,
                .depth_format = depth_format_,
                .stencil_format = stencil_format_,
                .samples = samples,
                .view_mask = view_mask_,
            };
        }

        [[nodiscard]] VkPipelineRenderingCreateInfo to_vk() const noexcept {
            return {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                .pNext = pnext_,
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
        const void *pnext_ = nullptr;
    };

} // namespace SFT::Core::Vulkan
