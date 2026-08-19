#include <Core/Vulkan/VulkanRendering.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Converts the supplied engine/RHI value to its Vulkan representation.
///
/// @return Returns the current to Vulkan value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkRenderingAttachmentInfo ColorAttachment::to_vk() const noexcept {
            ZoneScopedN("ColorAttachment::to_vk");
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

/// Converts the supplied engine/RHI value to its Vulkan representation.
///
/// @return Returns the current to Vulkan value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkRenderingAttachmentInfo DepthAttachment::to_vk() const noexcept {
            ZoneScopedN("DepthAttachment::to_vk");
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

/// Converts the supplied engine/RHI value to its Vulkan representation.
///
/// @return Returns the current to Vulkan value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkRenderingAttachmentInfo StencilAttachment::to_vk() const noexcept {
            ZoneScopedN("StencilAttachment::to_vk");
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

/// Sets the render area for this `Vulkan`.
///
/// @param area `area` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
RenderingInfo &RenderingInfo::set_render_area(VkRect2D area) noexcept {
            ZoneScopedN("RenderingInfo::set_render_area");
            render_area_ = area;
            return *this;
        }

/// Sets the layer count for this `Vulkan`.
///
/// @param count Number of elements or operations to process.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
RenderingInfo &RenderingInfo::set_layer_count(u32 count) noexcept {
            ZoneScopedN("RenderingInfo::set_layer_count");
            layer_count_ = count;
            return *this;
        }

/// Sets the view mask for this `Vulkan`.
///
/// @param mask `mask` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
RenderingInfo &RenderingInfo::set_view_mask(u32 mask) noexcept {
            ZoneScopedN("RenderingInfo::set_view_mask");
            view_mask_ = mask;
            return *this;
        }

/// Sets the flags for this `Vulkan`.
///
/// @param flags Flags controlling optional behavior.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
RenderingInfo &RenderingInfo::set_flags(VkRenderingFlags flags) noexcept {
            ZoneScopedN("RenderingInfo::set_flags");
            flags_ = flags;
            return *this;
        }

/// Adds flags using the supplied arguments and current state.
///
/// @param flags Flags controlling optional behavior.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
RenderingInfo &RenderingInfo::add_flags(VkRenderingFlags flags) noexcept {
            ZoneScopedN("RenderingInfo::add_flags");
            flags_ |= flags;
            return *this;
        }

/// Sets the next for this `Vulkan`.
///
/// @param next `next` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
RenderingInfo &RenderingInfo::set_next(const void *next) noexcept {
            ZoneScopedN("RenderingInfo::set_next");
            pnext_ = next;
            return *this;
        }

/// Returns the current or globally available suspend value.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
RenderingInfo &RenderingInfo::suspend() noexcept {
            ZoneScopedN("RenderingInfo::suspend");
            flags_ |= VK_RENDERING_SUSPENDING_BIT;
            return *this;
        }

/// Returns the current or globally available resume value.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
RenderingInfo &RenderingInfo::resume() noexcept {
            ZoneScopedN("RenderingInfo::resume");
            flags_ |= VK_RENDERING_RESUMING_BIT;
            return *this;
        }

/// Adds color using the supplied arguments and current state.
///
/// @param att `att` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
RenderingInfo &RenderingInfo::add_color(ColorAttachment att) {
            ZoneScopedN("RenderingInfo::add_color");
            color_attachments_vk_.push_back(att.to_vk());
            return *this;
        }

/// Sets the color for this `Vulkan`.
///
/// @param location `location` value used by the operation.
/// @param att `att` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
RenderingInfo &RenderingInfo::set_color(u32 location, ColorAttachment att) {
            ZoneScopedN("RenderingInfo::set_color");
            if (color_attachments_vk_.size() <= location) {
                color_attachments_vk_.resize(static_cast<usize>(location) + 1, unused_rendering_attachment());
            }
            color_attachments_vk_[location] = att.to_vk();
            return *this;
        }

/// Sets the unused color for this `Vulkan`.
///
/// @param location `location` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
RenderingInfo &RenderingInfo::set_unused_color(u32 location) {
            ZoneScopedN("RenderingInfo::set_unused_color");
            if (color_attachments_vk_.size() <= location) {
                color_attachments_vk_.resize(static_cast<usize>(location) + 1, unused_rendering_attachment());
            }
            color_attachments_vk_[location] = unused_rendering_attachment();
            return *this;
        }

/// Sets the colors for this `Vulkan`.
///
/// @param attachments `attachments` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
RenderingInfo &RenderingInfo::set_colors(span<const ColorAttachment> attachments) {
            ZoneScopedN("RenderingInfo::set_colors");
            color_attachments_vk_.clear();
            color_attachments_vk_.reserve(attachments.size());
            for (const ColorAttachment &attachment : attachments) {
                color_attachments_vk_.push_back(attachment.to_vk());
            }
            return *this;
        }

/// Sets the depth for this `Vulkan`.
///
/// @param att `att` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
RenderingInfo &RenderingInfo::set_depth(DepthAttachment att) noexcept {
            ZoneScopedN("RenderingInfo::set_depth");
            depth_vk_ = att.to_vk();
            has_depth_ = true;
            return *this;
        }

/// Sets the stencil for this `Vulkan`.
///
/// @param att `att` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
RenderingInfo &RenderingInfo::set_stencil(StencilAttachment att) noexcept {
            ZoneScopedN("RenderingInfo::set_stencil");
            stencil_vk_ = att.to_vk();
            has_stencil_ = true;
            return *this;
        }

/// Builds the requested object or derived state.
///
/// @return Returns the current build value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkRenderingInfo RenderingInfo::build() noexcept {
            ZoneScopedN("RenderingInfo::build");
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

/// Performs the scoped rendering pass operation for `Vulkan` using the supplied arguments.
///
/// @param command_buffer Buffer used or affected by the operation.
/// @param info Description of the resource or operation to perform.
///
/// @note This function does not throw exceptions.
ScopedRenderingPass::ScopedRenderingPass(VkCommandBuffer command_buffer, const VkRenderingInfo &info) noexcept
            : command_buffer_(command_buffer), active_(command_buffer != VK_NULL_HANDLE) {
            ZoneScopedN("ScopedRenderingPass::ScopedRenderingPass");
            if (active_) {
                vkCmdBeginRendering(command_buffer_, &info);
            }
        }

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
ScopedRenderingPass::~ScopedRenderingPass() { end(); }

/// Performs the scoped rendering pass operation for `Vulkan` using the supplied arguments.
///
/// @param other Other object used by the operation.
///
/// @note This function does not throw exceptions.
ScopedRenderingPass::ScopedRenderingPass(ScopedRenderingPass &&other) noexcept
            : command_buffer_(other.command_buffer_), active_(other.active_) {
            ZoneScopedN("ScopedRenderingPass::ScopedRenderingPass");
            other.command_buffer_ = VK_NULL_HANDLE;
            other.active_ = false;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param other Other object used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
ScopedRenderingPass &ScopedRenderingPass::operator=(ScopedRenderingPass &&other) noexcept {
            ZoneScopedN("ScopedRenderingPass::operator=");
            if (this != &other) {
                end();
                command_buffer_ = other.command_buffer_;
                active_ = other.active_;
                other.command_buffer_ = VK_NULL_HANDLE;
                other.active_ = false;
            }
            return *this;
        }

/// Returns the one-past-the-end iterator for the range.
///
/// @return Returns the one-past-the-end iterator.
/// @note This function does not throw exceptions.
void ScopedRenderingPass::end() noexcept {
            ZoneScopedN("ScopedRenderingPass::end");
            if (!active_) {
                return;
            }
            vkCmdEndRendering(command_buffer_);
            active_ = false;
        }

/// Returns the current or globally available active value.
///
/// @return Returns the current active value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool ScopedRenderingPass::active() const noexcept { return active_; }

/// Reports whether this `Vulkan` has depth stencil.
///
/// @return Returns the current has depth stencil value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool DynamicRenderingSignature::has_depth_stencil() const noexcept {
            ZoneScopedN("DynamicRenderingSignature::has_depth_stencil");
            return depth_format != VK_FORMAT_UNDEFINED || stencil_format != VK_FORMAT_UNDEFINED;
        }

/// Performs the pipeline rendering info operation for `Vulkan` using the supplied arguments.
///
/// @param signature `signature` value used by the operation.
///
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
PipelineRenderingInfo::PipelineRenderingInfo(const DynamicRenderingSignature &signature)
            : color_formats_(signature.color_formats), depth_format_(signature.depth_format),
              stencil_format_(signature.stencil_format), view_mask_(signature.view_mask) {}

/// Adds color format using the supplied arguments and current state.
///
/// @param fmt `fmt` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
PipelineRenderingInfo &PipelineRenderingInfo::add_color_format(VkFormat fmt) {
            ZoneScopedN("PipelineRenderingInfo::PipelineRenderingInfo");
            color_formats_.push_back(fmt);
            return *this;
        }

/// Sets the color format for this `Vulkan`.
///
/// @param location `location` value used by the operation.
/// @param fmt `fmt` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
PipelineRenderingInfo &PipelineRenderingInfo::set_color_format(u32 location, VkFormat fmt) {
            ZoneScopedN("PipelineRenderingInfo::set_color_format");
            if (color_formats_.size() <= location) {
                color_formats_.resize(static_cast<usize>(location) + 1, VK_FORMAT_UNDEFINED);
            }
            color_formats_[location] = fmt;
            return *this;
        }

/// Sets the color formats for this `Vulkan`.
///
/// @param formats Format used for the resource, render target, or conversion.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
PipelineRenderingInfo &PipelineRenderingInfo::set_color_formats(span<const VkFormat> formats) {
            ZoneScopedN("PipelineRenderingInfo::set_color_formats");
            color_formats_.assign(formats.begin(), formats.end());
            return *this;
        }

/// Sets the depth format for this `Vulkan`.
///
/// @param fmt `fmt` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
PipelineRenderingInfo &PipelineRenderingInfo::set_depth_format(VkFormat fmt) noexcept {
            ZoneScopedN("PipelineRenderingInfo::set_depth_format");
            depth_format_ = fmt;
            return *this;
        }

/// Sets the stencil format for this `Vulkan`.
///
/// @param fmt `fmt` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
PipelineRenderingInfo &PipelineRenderingInfo::set_stencil_format(VkFormat fmt) noexcept {
            ZoneScopedN("PipelineRenderingInfo::set_stencil_format");
            stencil_format_ = fmt;
            return *this;
        }

/// Sets the depth stencil format for this `Vulkan`.
///
/// @param fmt `fmt` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
PipelineRenderingInfo &PipelineRenderingInfo::set_depth_stencil_format(VkFormat fmt) noexcept {
            ZoneScopedN("PipelineRenderingInfo::set_depth_stencil_format");
            depth_format_ = fmt;
            stencil_format_ = fmt;
            return *this;
        }

/// Sets the next for this `Vulkan`.
///
/// @param next `next` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
PipelineRenderingInfo &PipelineRenderingInfo::set_next(const void *next) noexcept {
            ZoneScopedN("PipelineRenderingInfo::set_next");
            pnext_ = next;
            return *this;
        }

/// Sets the view mask for this `Vulkan`.
///
/// @param mask `mask` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
PipelineRenderingInfo &PipelineRenderingInfo::set_view_mask(u32 mask) noexcept {
            ZoneScopedN("PipelineRenderingInfo::set_view_mask");
            view_mask_ = mask;
            return *this;
        }

/// Performs the signature operation for `Vulkan` using the supplied arguments.
///
/// @param samples `samples` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] DynamicRenderingSignature PipelineRenderingInfo::signature(VkSampleCountFlagBits samples) const {
            ZoneScopedN("PipelineRenderingInfo::signature");
            return DynamicRenderingSignature{
                .color_formats = color_formats_,
                .depth_format = depth_format_,
                .stencil_format = stencil_format_,
                .samples = samples,
                .view_mask = view_mask_,
            };
        }

/// Converts the supplied engine/RHI value to its Vulkan representation.
///
/// @return Returns the current to Vulkan value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkPipelineRenderingCreateInfo PipelineRenderingInfo::to_vk() const noexcept {
            ZoneScopedN("PipelineRenderingInfo::to_vk");
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
