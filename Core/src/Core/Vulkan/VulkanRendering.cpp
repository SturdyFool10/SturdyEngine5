#include "VulkanRendering.hpp"

namespace SFT::Core::Vulkan {

[[nodiscard]] VkRenderingAttachmentInfo ColorAttachment::to_vk() const noexcept {
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

[[nodiscard]] VkRenderingAttachmentInfo DepthAttachment::to_vk() const noexcept {
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

[[nodiscard]] VkRenderingAttachmentInfo StencilAttachment::to_vk() const noexcept {
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

RenderingInfo &RenderingInfo::set_render_area(VkRect2D area) noexcept {
            render_area_ = area;
            return *this;
        }

RenderingInfo &RenderingInfo::set_layer_count(u32 count) noexcept {
            layer_count_ = count;
            return *this;
        }

RenderingInfo &RenderingInfo::set_view_mask(u32 mask) noexcept {
            view_mask_ = mask;
            return *this;
        }

RenderingInfo &RenderingInfo::set_flags(VkRenderingFlags flags) noexcept {
            flags_ = flags;
            return *this;
        }

RenderingInfo &RenderingInfo::add_flags(VkRenderingFlags flags) noexcept {
            flags_ |= flags;
            return *this;
        }

RenderingInfo &RenderingInfo::set_next(const void *next) noexcept {
            pnext_ = next;
            return *this;
        }

RenderingInfo &RenderingInfo::suspend() noexcept {
            flags_ |= VK_RENDERING_SUSPENDING_BIT;
            return *this;
        }

RenderingInfo &RenderingInfo::resume() noexcept {
            flags_ |= VK_RENDERING_RESUMING_BIT;
            return *this;
        }

RenderingInfo &RenderingInfo::add_color(ColorAttachment att) {
            color_attachments_vk_.push_back(att.to_vk());
            return *this;
        }

RenderingInfo &RenderingInfo::set_color(u32 location, ColorAttachment att) {
            if (color_attachments_vk_.size() <= location) {
                color_attachments_vk_.resize(static_cast<usize>(location) + 1, unused_rendering_attachment());
            }
            color_attachments_vk_[location] = att.to_vk();
            return *this;
        }

RenderingInfo &RenderingInfo::set_unused_color(u32 location) {
            if (color_attachments_vk_.size() <= location) {
                color_attachments_vk_.resize(static_cast<usize>(location) + 1, unused_rendering_attachment());
            }
            color_attachments_vk_[location] = unused_rendering_attachment();
            return *this;
        }

RenderingInfo &RenderingInfo::set_colors(span<const ColorAttachment> attachments) {
            color_attachments_vk_.clear();
            color_attachments_vk_.reserve(attachments.size());
            for (const ColorAttachment &attachment : attachments) {
                color_attachments_vk_.push_back(attachment.to_vk());
            }
            return *this;
        }

RenderingInfo &RenderingInfo::set_depth(DepthAttachment att) noexcept {
            depth_vk_ = att.to_vk();
            has_depth_ = true;
            return *this;
        }

RenderingInfo &RenderingInfo::set_stencil(StencilAttachment att) noexcept {
            stencil_vk_ = att.to_vk();
            has_stencil_ = true;
            return *this;
        }

[[nodiscard]] VkRenderingInfo RenderingInfo::build() noexcept {
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

ScopedRenderingPass::ScopedRenderingPass(VkCommandBuffer command_buffer, const VkRenderingInfo &info) noexcept
            : command_buffer_(command_buffer), active_(command_buffer != VK_NULL_HANDLE) {
            if (active_) {
                vkCmdBeginRendering(command_buffer_, &info);
            }
        }

ScopedRenderingPass::~ScopedRenderingPass() { end(); }

ScopedRenderingPass::ScopedRenderingPass(ScopedRenderingPass &&other) noexcept
            : command_buffer_(other.command_buffer_), active_(other.active_) {
            other.command_buffer_ = VK_NULL_HANDLE;
            other.active_ = false;
        }

ScopedRenderingPass &ScopedRenderingPass::operator=(ScopedRenderingPass &&other) noexcept {
            if (this != &other) {
                end();
                command_buffer_ = other.command_buffer_;
                active_ = other.active_;
                other.command_buffer_ = VK_NULL_HANDLE;
                other.active_ = false;
            }
            return *this;
        }

void ScopedRenderingPass::end() noexcept {
            if (!active_) {
                return;
            }
            vkCmdEndRendering(command_buffer_);
            active_ = false;
        }

[[nodiscard]] bool ScopedRenderingPass::active() const noexcept { return active_; }

[[nodiscard]] bool DynamicRenderingSignature::has_depth_stencil() const noexcept {
            return depth_format != VK_FORMAT_UNDEFINED || stencil_format != VK_FORMAT_UNDEFINED;
        }

PipelineRenderingInfo::PipelineRenderingInfo(const DynamicRenderingSignature &signature)
            : color_formats_(signature.color_formats), depth_format_(signature.depth_format),
              stencil_format_(signature.stencil_format), view_mask_(signature.view_mask) {}

PipelineRenderingInfo &PipelineRenderingInfo::add_color_format(VkFormat fmt) {
            color_formats_.push_back(fmt);
            return *this;
        }

PipelineRenderingInfo &PipelineRenderingInfo::set_color_format(u32 location, VkFormat fmt) {
            if (color_formats_.size() <= location) {
                color_formats_.resize(static_cast<usize>(location) + 1, VK_FORMAT_UNDEFINED);
            }
            color_formats_[location] = fmt;
            return *this;
        }

PipelineRenderingInfo &PipelineRenderingInfo::set_color_formats(span<const VkFormat> formats) {
            color_formats_.assign(formats.begin(), formats.end());
            return *this;
        }

PipelineRenderingInfo &PipelineRenderingInfo::set_depth_format(VkFormat fmt) noexcept {
            depth_format_ = fmt;
            return *this;
        }

PipelineRenderingInfo &PipelineRenderingInfo::set_stencil_format(VkFormat fmt) noexcept {
            stencil_format_ = fmt;
            return *this;
        }

PipelineRenderingInfo &PipelineRenderingInfo::set_depth_stencil_format(VkFormat fmt) noexcept {
            depth_format_ = fmt;
            stencil_format_ = fmt;
            return *this;
        }

PipelineRenderingInfo &PipelineRenderingInfo::set_next(const void *next) noexcept {
            pnext_ = next;
            return *this;
        }

PipelineRenderingInfo &PipelineRenderingInfo::set_view_mask(u32 mask) noexcept {
            view_mask_ = mask;
            return *this;
        }

[[nodiscard]] DynamicRenderingSignature PipelineRenderingInfo::signature(VkSampleCountFlagBits samples) const {
            return DynamicRenderingSignature{
                .color_formats = color_formats_,
                .depth_format = depth_format_,
                .stencil_format = stencil_format_,
                .samples = samples,
                .view_mask = view_mask_,
            };
        }

[[nodiscard]] VkPipelineRenderingCreateInfo PipelineRenderingInfo::to_vk() const noexcept {
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

} // namespace SFT::Core::Vulkan
