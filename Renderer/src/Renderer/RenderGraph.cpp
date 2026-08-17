#include "RenderGraph.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <tracy/Tracy.hpp>

namespace SFT::Renderer {

    using std::optional;
    using std::pair;
    using std::string;
    using std::string_view;
    using std::unique_ptr;

    namespace {
        using CpuClock = std::chrono::steady_clock;

        const UString render_graph_blit_label{"render graph blit"_ustr};
        const UString render_graph_copy_label{"render graph copy"_ustr};

        /// Renders graph labelled error using the current rendering state.
        ///
        /// @param operation `operation` value used by the operation.
        /// @param label `label` value used by the operation.
        /// @param error Error value describing the failure.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] string render_graph_labelled_error(
            const ustr &operation, const UString &label, const string &error) {
            UString message{operation};
            message.append(" '"_ustr);
            message.append(label);
            message.append("' failed: "_ustr);
            message.append(string_view{error});
            return message.cpp_string();
        }
    } // namespace

/// Performs the pass usage of operation for `Renderer` using the supplied arguments.
///
/// @param pass Render-pass encoder that receives the draw commands.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] RenderGraph::PassUsage RenderGraph::pass_usage_of(const RenderGraphRenderPassBuilder &pass) {
            ZoneScopedN("RenderGraph::pass_usage_of");
            PassUsage usage;
            for (const RenderGraphColorAttachmentDesc &attachment : pass.color_attachments_) {
                usage.writes.push_back(attachment.texture);
                if (attachment.load_op == RHI::LoadOp::Load) {
                    usage.reads.push_back(attachment.texture);
                }
                if (attachment.resolve_texture) {
                    usage.writes.push_back(attachment.resolve_texture);
                }
            }
            if (pass.has_depth_stencil_attachment_) {


                usage.writes.push_back(pass.depth_stencil_attachment_.texture);
                usage.reads.push_back(pass.depth_stencil_attachment_.texture);
                if (pass.depth_stencil_attachment_.resolve_texture) {
                    usage.writes.push_back(pass.depth_stencil_attachment_.resolve_texture);
                }
            }
            for (const RenderGraphSampledTextureReadDesc &read : pass.sampled_texture_reads_) {
                usage.reads.push_back(read.texture);
            }
            for (const RenderGraphBufferAccessDesc &access : pass.buffers_) {
                if (access.read) {
                    usage.buffer_reads.push_back(access.buffer);
                }
                if (access.write) {
                    usage.buffer_writes.push_back(access.buffer);
                }
            }
            usage.always_live = pass.side_effect_ ||
                                (pass.color_attachments_.empty() && !pass.has_depth_stencil_attachment_);
            return usage;
        }

/// Performs the pass usage of operation for `Renderer` using the supplied arguments.
///
/// @param pass Render-pass encoder that receives the draw commands.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] RenderGraph::PassUsage RenderGraph::pass_usage_of(const RenderGraphBlitDesc &pass) {
            ZoneScopedN("RenderGraph::pass_usage_of");
            PassUsage usage;
            usage.writes.push_back(pass.destination);
            usage.reads.push_back(pass.source);
            return usage;
        }

/// Performs the pass usage of operation for `Renderer` using the supplied arguments.
///
/// @param pass Render-pass encoder that receives the draw commands.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] RenderGraph::PassUsage RenderGraph::pass_usage_of(const RenderGraphComputePassBuilder &pass) {
            ZoneScopedN("RenderGraph::pass_usage_of");
            PassUsage usage;
            for (RenderGraphTextureHandle read : pass.sampled_texture_reads_) {
                usage.reads.push_back(read);
            }
            for (const RenderGraphStorageTextureAccessDesc &access : pass.storage_textures_) {
                if (access.read) {
                    usage.reads.push_back(access.texture);
                }
                if (access.write) {
                    usage.writes.push_back(access.texture);
                }
            }
            for (const RenderGraphBufferAccessDesc &access : pass.buffers_) {
                if (access.read) {
                    usage.buffer_reads.push_back(access.buffer);
                }
                if (access.write) {
                    usage.buffer_writes.push_back(access.buffer);
                }
            }


            usage.always_live = pass.side_effect_ ||
                                (usage.writes.empty() && usage.buffer_writes.empty());
            return usage;
        }

/// Performs the pass usage of operation for `Renderer` using the supplied arguments.
///
/// @param pass Render-pass encoder that receives the draw commands.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] RenderGraph::PassUsage RenderGraph::pass_usage_of(const RenderGraphCopyDesc &pass) {
            ZoneScopedN("RenderGraph::pass_usage_of");
            PassUsage usage;
            usage.writes.push_back(pass.destination);
            usage.reads.push_back(pass.source);
            return usage;
        }

/// Performs the usage of ordered operation for `Renderer` using the supplied arguments.
///
/// @param ordered `ordered` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] RenderGraph::PassUsage RenderGraph::usage_of_ordered(const OrderedPass &ordered) const {
            ZoneScopedN("RenderGraph::usage_of_ordered");
            switch (ordered.kind) {
                case PassKind::Render: return pass_usage_of(render_passes_[ordered.index]);
                case PassKind::Blit: return pass_usage_of(blit_passes_[ordered.index]);
                case PassKind::Compute: return pass_usage_of(compute_passes_[ordered.index]);
                case PassKind::Copy: return pass_usage_of(copy_passes_[ordered.index]);
            }
            return {};
        }

/// Renders the requested content using the current rendering state.
///
/// @param label `label` value used by the operation.
///
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
RenderGraphRenderPassBuilder::RenderGraphRenderPassBuilder(const ustr &label) : label_(label) {}

/// Adds color attachment using the supplied arguments and current state.
///
/// @param attachment `attachment` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
RenderGraphRenderPassBuilder &RenderGraphRenderPassBuilder::add_color_attachment(const RenderGraphColorAttachmentDesc &attachment) {
            ZoneScopedN("RenderGraphRenderPassBuilder::add_color_attachment");
            color_attachments_.push_back(attachment);
            return *this;
        }

/// Sets the depth stencil attachment for this `Renderer`.
///
/// @param attachment `attachment` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
RenderGraphRenderPassBuilder &RenderGraphRenderPassBuilder::set_depth_stencil_attachment(const RenderGraphDepthStencilAttachmentDesc &attachment) {
            ZoneScopedN("RenderGraphRenderPassBuilder::set_depth_stencil_attachment");
            depth_stencil_attachment_ = attachment;
            has_depth_stencil_attachment_ = true;
            return *this;
        }

/// Adds sampled texture using the supplied arguments and current state.
///
/// @param read `read` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
RenderGraphRenderPassBuilder &RenderGraphRenderPassBuilder::add_sampled_texture(const RenderGraphSampledTextureReadDesc &read) {
            ZoneScopedN("RenderGraphRenderPassBuilder::add_sampled_texture");
            sampled_texture_reads_.push_back(read);
            return *this;
        }

/// Adds buffer using the supplied arguments and current state.
///
/// @param access `access` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
RenderGraphRenderPassBuilder &RenderGraphRenderPassBuilder::add_buffer(const RenderGraphBufferAccessDesc &access) {
            ZoneScopedN("RenderGraphRenderPassBuilder::add_buffer");
            buffers_.push_back(access);
            return *this;
        }

/// Sets the render area for this `Renderer`.
///
/// @param render_area `render_area` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
RenderGraphRenderPassBuilder &RenderGraphRenderPassBuilder::set_render_area(const RHI::Rect2D &render_area) noexcept {
            ZoneScopedN("RenderGraphRenderPassBuilder::set_render_area");
            render_area_ = render_area;
            return *this;
        }

/// Sets the view mask for this `Renderer`.
///
/// @param view_mask `view_mask` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
RenderGraphRenderPassBuilder &RenderGraphRenderPassBuilder::set_view_mask(u32 view_mask) noexcept {
            ZoneScopedN("RenderGraphRenderPassBuilder::set_view_mask");
            view_mask_ = view_mask;
            return *this;
        }

/// Sets the allow bundles for this `Renderer`.
///
/// @param allow_bundles `allow_bundles` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
RenderGraphRenderPassBuilder &RenderGraphRenderPassBuilder::set_allow_bundles(bool allow_bundles) noexcept {
            ZoneScopedN("RenderGraphRenderPassBuilder::set_allow_bundles");
            allow_bundles_ = allow_bundles;
            return *this;
        }

/// Sets the side effect for this `Renderer`.
///
/// @param side_effect `side_effect` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
RenderGraphRenderPassBuilder &RenderGraphRenderPassBuilder::set_side_effect(bool side_effect) noexcept {
            ZoneScopedN("RenderGraphRenderPassBuilder::set_side_effect");
            side_effect_ = side_effect;
            return *this;
        }

/// Sets the execute for this `Renderer`.
///
/// @param execute `execute` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
RenderGraphRenderPassBuilder &RenderGraphRenderPassBuilder::set_execute(RenderGraphExecuteFn execute) noexcept {
            ZoneScopedN("RenderGraphRenderPassBuilder::set_execute");
            execute_ = std::move(execute);
            return *this;
        }

/// Renders the requested content using the current rendering state.
///
/// @param label `label` value used by the operation.
///
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
RenderGraphComputePassBuilder::RenderGraphComputePassBuilder(const ustr &label) : label_(label) {}

/// Adds sampled texture using the supplied arguments and current state.
///
/// @param texture Texture used or affected by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
RenderGraphComputePassBuilder &RenderGraphComputePassBuilder::add_sampled_texture(RenderGraphTextureHandle texture) {
            ZoneScopedN("RenderGraphComputePassBuilder::add_sampled_texture");
            sampled_texture_reads_.push_back(texture);
            return *this;
        }

/// Adds storage texture using the supplied arguments and current state.
///
/// @param access `access` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
RenderGraphComputePassBuilder &RenderGraphComputePassBuilder::add_storage_texture(const RenderGraphStorageTextureAccessDesc &access) {
            ZoneScopedN("RenderGraphComputePassBuilder::add_storage_texture");
            storage_textures_.push_back(access);
            return *this;
        }

/// Adds buffer using the supplied arguments and current state.
///
/// @param access `access` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
RenderGraphComputePassBuilder &RenderGraphComputePassBuilder::add_buffer(const RenderGraphBufferAccessDesc &access) {
            ZoneScopedN("RenderGraphComputePassBuilder::add_buffer");
            buffers_.push_back(access);
            return *this;
        }

/// Sets the side effect for this `Renderer`.
///
/// @param side_effect `side_effect` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
RenderGraphComputePassBuilder &RenderGraphComputePassBuilder::set_side_effect(bool side_effect) noexcept {
            ZoneScopedN("RenderGraphComputePassBuilder::set_side_effect");
            side_effect_ = side_effect;
            return *this;
        }

/// Sets the execute for this `Renderer`.
///
/// @param execute `execute` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
RenderGraphComputePassBuilder &RenderGraphComputePassBuilder::set_execute(RenderGraphComputeExecuteFn execute) noexcept {
            ZoneScopedN("RenderGraphComputePassBuilder::set_execute");
            execute_ = std::move(execute);
            return *this;
        }

/// Imports texture using the supplied arguments and current state.
///
/// @param desc Description of the resource or operation to perform.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] RenderGraphTextureHandle RenderGraph::import_texture(const RenderGraphImportedTextureDesc &desc) {
            ZoneScopedN("RenderGraph::import_texture");
            const u32 slot_index = static_cast<u32>(physical_slots_.size());
            physical_slots_.push_back(PhysicalSlot{
                .texture = desc.texture,
                .default_view = desc.default_view,
                .mip_states = vector<TextureState>(std::max(desc.mip_levels, 1u), TextureState{
                    .layout = desc.initial_layout,
                    .stage = desc.initial_stage,
                    .access = desc.initial_access,
                }),
                .owns_resource = false,
            });

            const RenderGraphTextureHandle handle{static_cast<u32>(textures_.size())};
            textures_.push_back(TextureRecord{
                .imported = desc,
                .is_transient = false,
                .physical_slot = slot_index,
                .format = desc.format,
                .extent = desc.extent,
                .mip_levels = std::max(desc.mip_levels, 1u),
                .samples = desc.samples,
                .usage = desc.usage,
                .final_layout = desc.final_layout,
                .final_stage = desc.final_stage,
                .final_access = desc.final_access,
                .label = UString{desc.label ? desc.label : ""},
            });
            return handle;
        }

/// Imports buffer using the supplied arguments and current state.
///
/// @param desc Description of the resource or operation to perform.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] RenderGraphBufferHandle RenderGraph::import_buffer(const RenderGraphImportedBufferDesc &desc) {
            ZoneScopedN("RenderGraph::import_buffer");
            const RenderGraphBufferHandle handle{static_cast<u32>(buffers_.size())};
            buffers_.push_back(BufferRecord{
                .imported = desc,
                .stage = desc.initial_stage,
                .access = desc.initial_access,
                .label = UString{desc.label ? desc.label : ""},
            });
            return handle;
        }

/// Creates a texture from the supplied parameters.
///
/// @param desc Description of the resource or operation to perform.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] RenderGraphTextureHandle RenderGraph::create_texture(const RenderGraphTextureDesc &desc) {
            ZoneScopedN("RenderGraph::create_texture");


            const RenderGraphTextureHandle handle{static_cast<u32>(textures_.size())};
            textures_.push_back(TextureRecord{
                .transient = desc,
                .is_transient = true,
                .physical_slot = ~0u,
                .format = desc.format,
                .extent = desc.extent,
                .mip_levels = std::max(desc.mip_levels, 1u),
                .samples = desc.samples,
                .usage = desc.usage,
                .final_layout = desc.final_layout,
                .final_stage = desc.final_stage,
                .final_access = desc.final_access,
                .label = UString{desc.label ? desc.label : ""},
            });
            return handle;
        }

/// Adds render pass using the supplied arguments and current state.
///
/// @param label `label` value used by the operation.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] RenderGraphRenderPassBuilder &RenderGraph::add_render_pass(const ustr &label) {
            ZoneScopedN("RenderGraph::add_render_pass");
            const u32 index = static_cast<u32>(render_passes_.size());
            render_passes_.emplace_back(label);
            ordered_passes_.push_back(OrderedPass{.kind = PassKind::Render, .index = index});
            return render_passes_.back();
        }

/// Adds blit pass using the supplied arguments and current state.
///
/// @param desc Description of the resource or operation to perform.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void RenderGraph::add_blit_pass(const RenderGraphBlitDesc &desc) {
            ZoneScopedN("RenderGraph::add_blit_pass");
            const u32 index = static_cast<u32>(blit_passes_.size());
            blit_passes_.push_back(desc);
            ordered_passes_.push_back(OrderedPass{.kind = PassKind::Blit, .index = index});
        }

/// Adds compute pass using the supplied arguments and current state.
///
/// @param label `label` value used by the operation.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] RenderGraphComputePassBuilder &RenderGraph::add_compute_pass(const ustr &label) {
            ZoneScopedN("RenderGraph::add_compute_pass");
            const u32 index = static_cast<u32>(compute_passes_.size());
            compute_passes_.emplace_back(label);
            ordered_passes_.push_back(OrderedPass{.kind = PassKind::Compute, .index = index});
            return compute_passes_.back();
        }

/// Adds copy pass using the supplied arguments and current state.
///
/// @param desc Description of the resource or operation to perform.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void RenderGraph::add_copy_pass(const RenderGraphCopyDesc &desc) {
            ZoneScopedN("RenderGraph::add_copy_pass");
            const u32 index = static_cast<u32>(copy_passes_.size());
            copy_passes_.push_back(desc);
            ordered_passes_.push_back(OrderedPass{.kind = PassKind::Copy, .index = index});
        }

/// Marks output using the supplied arguments and current state.
///
/// @param texture Texture used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void RenderGraph::mark_output(RenderGraphTextureHandle texture) {
            ZoneScopedN("RenderGraph::mark_output");
            if (std::find(outputs_.begin(), outputs_.end(), texture) == outputs_.end()) {
                outputs_.push_back(texture);
            }
        }

/// Performs the texture access operation for `Renderer` using the supplied arguments.
///
/// @param handle Handle identifying the target object or resource.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] RenderGraphTextureAccess RenderGraph::texture_access(RenderGraphTextureHandle handle) const noexcept {
            ZoneScopedN("RenderGraph::texture_access");
            const TextureRecord *record = texture_record(handle);
            const PhysicalSlot *slot = physical_slot_for(handle);
            if (record == nullptr || slot == nullptr) {
                return {};
            }
            return RenderGraphTextureAccess{
                .texture = slot->texture,
                .default_view = slot->default_view,
                .format = record->format,
                .extent = record->extent,
                .current_layout = slot->mip_states.empty() ? RHI::TextureLayout::Undefined : slot->mip_states.front().layout,
            };
        }

/// Performs the buffer access operation for `Renderer` using the supplied arguments.
///
/// @param handle Handle identifying the target object or resource.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] RenderGraphBufferAccess RenderGraph::buffer_access(RenderGraphBufferHandle handle) const noexcept {
            ZoneScopedN("RenderGraph::buffer_access");
            const BufferRecord *record = buffer_record(handle);
            return record != nullptr
                ? RenderGraphBufferAccess{.buffer = record->imported.buffer, .size = record->imported.size}
                : RenderGraphBufferAccess{};
        }


/// Compiles the supplied source or pipeline state.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `RenderGraphCompileErrorCode::UnknownTextureHandle`, `RenderGraphCompileErrorCode::UnknownBufferHandle`, `RenderGraphCompileErrorCode::InvalidBufferAccess`, `RenderGraphCompileErrorCode::IncompatibleTextureCopy`, `RenderGraphCompileErrorCode::MissingProducer`.
[[nodiscard]] RenderGraph::CompileResult RenderGraph::compile() const {
            ZoneScopedN("RenderGraph::compile");
            const usize pass_count = ordered_passes_.size();
            vector<PassUsage> usage(pass_count);
            for (usize i = 0; i < pass_count; ++i) {
                usage[i] = usage_of_ordered(ordered_passes_[i]);
            }

            for (RenderGraphTextureHandle output : outputs_) {
                if (texture_record(output) == nullptr) {
                    return std::unexpected(RenderGraphCompileError{
                        .code = RenderGraphCompileErrorCode::UnknownTextureHandle,
                        .message = "Render graph marks a texture handle this graph never created or imported as output.",
                    });
                }
            }

            for (usize i = 0; i < pass_count; ++i) {
                for (RenderGraphTextureHandle handle : usage[i].reads) {
                    if (texture_record(handle) == nullptr) {
                        return std::unexpected(RenderGraphCompileError{
                            .code = RenderGraphCompileErrorCode::UnknownTextureHandle,
                            .message = "Render graph pass reads a texture handle this graph never created or imported.",
                        });
                    }
                }
                for (RenderGraphTextureHandle handle : usage[i].writes) {
                    if (texture_record(handle) == nullptr) {
                        return std::unexpected(RenderGraphCompileError{
                            .code = RenderGraphCompileErrorCode::UnknownTextureHandle,
                            .message = "Render graph pass writes a texture handle this graph never created or imported.",
                        });
                    }
                }
                for (RenderGraphBufferHandle handle : usage[i].buffer_reads) {
                    if (buffer_record(handle) == nullptr) {
                        return std::unexpected(RenderGraphCompileError{
                            .code = RenderGraphCompileErrorCode::UnknownBufferHandle,
                            .message = "Render graph pass reads a buffer handle this graph never imported.",
                        });
                    }
                }
                for (RenderGraphBufferHandle handle : usage[i].buffer_writes) {
                    if (buffer_record(handle) == nullptr) {
                        return std::unexpected(RenderGraphCompileError{
                            .code = RenderGraphCompileErrorCode::UnknownBufferHandle,
                            .message = "Render graph pass writes a buffer handle this graph never imported.",
                        });
                    }
                }
            }

            const auto validate_buffer_access = [this](const RenderGraphBufferAccessDesc &access)
                -> optional<RenderGraphCompileError> {
                const BufferRecord *record = buffer_record(access.buffer);
                const bool range_valid = record != nullptr && record->imported.size > 0 &&
                    access.offset < record->imported.size &&
                    (access.size == 0 || access.size <= record->imported.size - access.offset);
                constexpr RHI::AccessFlags read_mask =
                    RHI::AccessFlags::IndirectCommandRead | RHI::AccessFlags::IndexRead |
                    RHI::AccessFlags::VertexAttributeRead | RHI::AccessFlags::UniformRead |
                    RHI::AccessFlags::ShaderRead | RHI::AccessFlags::ColorAttachmentRead |
                    RHI::AccessFlags::DepthStencilAttachmentRead | RHI::AccessFlags::TransferRead |
                    RHI::AccessFlags::HostRead | RHI::AccessFlags::AccelerationStructureRead |
                    RHI::AccessFlags::MemoryRead;
                constexpr RHI::AccessFlags write_mask =
                    RHI::AccessFlags::ShaderWrite | RHI::AccessFlags::ColorAttachmentWrite |
                    RHI::AccessFlags::DepthStencilAttachmentWrite | RHI::AccessFlags::TransferWrite |
                    RHI::AccessFlags::HostWrite | RHI::AccessFlags::AccelerationStructureWrite |
                    RHI::AccessFlags::MemoryWrite;
                const bool mask_reads = (access.access & read_mask) != RHI::AccessFlags::None;
                const bool mask_writes = (access.access & write_mask) != RHI::AccessFlags::None;
                if ((!access.read && !access.write) || access.stages == RHI::PipelineStage::None ||
                    access.access == RHI::AccessFlags::None || access.read != mask_reads ||
                    access.write != mask_writes || !range_valid) {
                    return RenderGraphCompileError{
                        .code = RenderGraphCompileErrorCode::InvalidBufferAccess,
                        .message = "Render graph pass declares an invalid buffer access or range.",
                    };
                }
                return std::nullopt;
            };
            for (const RenderGraphCopyDesc &copy : copy_passes_) {
                const TextureRecord *source = texture_record(copy.source);
                const TextureRecord *destination = texture_record(copy.destination);
                const bool distinct_backing = source != nullptr && destination != nullptr && copy.source != copy.destination &&
                    (source->is_transient || destination->is_transient ||
                     source->imported.texture != destination->imported.texture);
                const bool compatible = distinct_backing && source->format == destination->format &&
                    source->extent.width == destination->extent.width &&
                    source->extent.height == destination->extent.height && source->extent.depth_or_layers == 1 &&
                    destination->extent.depth_or_layers == 1 && source->mip_levels == 1 && destination->mip_levels == 1 &&
                    source->samples == destination->samples &&
                    (source->usage & RHI::TextureUsage::TransferSrc) != RHI::TextureUsage::None &&
                    (destination->usage & RHI::TextureUsage::TransferDst) != RHI::TextureUsage::None;
                if (!compatible) {
                    return std::unexpected(RenderGraphCompileError{
                        .code = RenderGraphCompileErrorCode::IncompatibleTextureCopy,
                        .message = "Render graph exact-copy pass requires distinct single-mip/single-layer textures with identical format, extent, and sample count plus TransferSrc/TransferDst usage.",
                    });
                }
            }

            for (const RenderGraphRenderPassBuilder &pass : render_passes_) {
                for (const RenderGraphBufferAccessDesc &access : pass.buffers_) {
                    if (auto error = validate_buffer_access(access)) {
                        return std::unexpected(std::move(*error));
                    }
                }
            }
            for (const RenderGraphComputePassBuilder &pass : compute_passes_) {
                for (const RenderGraphBufferAccessDesc &access : pass.buffers_) {
                    if (auto error = validate_buffer_access(access)) {
                        return std::unexpected(std::move(*error));
                    }
                }
            }

            vector<i64> last_writer(textures_.size(), -1);
            vector<i64> last_buffer_writer(buffers_.size(), -1);
            vector<vector<u32>> depends_on(pass_count);
            for (usize i = 0; i < pass_count; ++i) {
                for (RenderGraphTextureHandle read : usage[i].reads) {
                    if (read.index < last_writer.size() && last_writer[read.index] >= 0) {
                        depends_on[i].push_back(static_cast<u32>(last_writer[read.index]));
                        continue;
                    }
                    const TextureRecord *record = texture_record(read);
                    const bool same_pass_write =
                        std::find(usage[i].writes.begin(), usage[i].writes.end(), read) != usage[i].writes.end();
                    if (record != nullptr && record->is_transient && !same_pass_write) {
                        UString message{"Render graph pass reads transient texture '"_ustr};
                        message.append(record->label);
                        message.append("' before any earlier pass wrote it."_ustr);
                        return std::unexpected(RenderGraphCompileError{
                            .code = RenderGraphCompileErrorCode::MissingProducer,
                            .message = std::move(message),
                        });
                    }
                }
                for (RenderGraphTextureHandle write : usage[i].writes) {
                    if (write.index < last_writer.size() && last_writer[write.index] >= 0 &&
                        static_cast<usize>(last_writer[write.index]) != i) {
                        depends_on[i].push_back(static_cast<u32>(last_writer[write.index]));
                    }
                    if (write.index < last_writer.size()) {
                        last_writer[write.index] = static_cast<i64>(i);
                    }
                }
                for (RenderGraphBufferHandle read : usage[i].buffer_reads) {
                    if (read.index < last_buffer_writer.size() && last_buffer_writer[read.index] >= 0) {
                        depends_on[i].push_back(static_cast<u32>(last_buffer_writer[read.index]));
                    }
                }
                for (RenderGraphBufferHandle write : usage[i].buffer_writes) {
                    if (write.index < last_buffer_writer.size() && last_buffer_writer[write.index] >= 0 &&
                        static_cast<usize>(last_buffer_writer[write.index]) != i) {
                        depends_on[i].push_back(static_cast<u32>(last_buffer_writer[write.index]));
                    }
                    if (write.index < last_buffer_writer.size()) {
                        last_buffer_writer[write.index] = static_cast<i64>(i);
                    }
                }
            }

            vector<bool> live(pass_count, false);
            vector<u32> pending;
            for (usize i = 0; i < pass_count; ++i) {
                const bool writes_output = std::ranges::any_of(usage[i].writes, [this](RenderGraphTextureHandle write) {
                    return std::find(outputs_.begin(), outputs_.end(), write) != outputs_.end();
                });
                if ((writes_output || usage[i].always_live) && !live[i]) {
                    live[i] = true;
                    pending.push_back(static_cast<u32>(i));
                }
            }
            while (!pending.empty()) {
                const u32 i = pending.back();
                pending.pop_back();
                for (u32 dependency : depends_on[i]) {
                    if (!live[dependency]) {
                        live[dependency] = true;
                        pending.push_back(dependency);
                    }
                }
            }

            vector<u32> in_degree(pass_count, 0);
            vector<vector<u32>> dependents(pass_count);
            for (usize i = 0; i < pass_count; ++i) {
                if (!live[i]) {
                    continue;
                }
                for (u32 dependency : depends_on[i]) {
                    if (live[dependency]) {
                        dependents[dependency].push_back(static_cast<u32>(i));
                        ++in_degree[i];
                    }
                }
            }


            vector<u32> ready;
            const auto push_ready = [&ready](u32 index) {
                ready.push_back(index);
                std::push_heap(ready.begin(), ready.end(), std::greater<>());
            };
            for (usize i = 0; i < pass_count; ++i) {
                if (live[i] && in_degree[i] == 0) {
                    push_ready(static_cast<u32>(i));
                }
            }

            vector<bool> scheduled(pass_count, false);
            vector<OrderedPass> order;


            vector<u32> order_original_index;
            order.reserve(pass_count);
            order_original_index.reserve(pass_count);
            while (!ready.empty()) {
                std::pop_heap(ready.begin(), ready.end(), std::greater<>());
                const u32 i = ready.back();
                ready.pop_back();
                scheduled[i] = true;
                order.push_back(ordered_passes_[i]);
                order_original_index.push_back(i);
                for (u32 dependent : dependents[i]) {
                    if (--in_degree[dependent] == 0) {
                        push_ready(dependent);
                    }
                }
            }


            for (usize i = 0; i < pass_count; ++i) {
                if (live[i] && !scheduled[i]) {
                    order.push_back(ordered_passes_[i]);
                    order_original_index.push_back(static_cast<u32>(i));
                }
            }

            vector<PassUsage> usage_by_order_position(order.size());
            for (usize i = 0; i < order.size(); ++i) {
                usage_by_order_position[i] = std::move(usage[order_original_index[i]]);
            }
            vector<u32> levels = compute_levels_from_usage(usage_by_order_position);
            return CompiledPlan{.order = std::move(order), .levels = std::move(levels)};
        }

/// Executes the requested work.
///
/// @param device Device used or affected by the operation.
/// @param encoder `encoder` value used by the operation.
/// @param timestamp_query_set `timestamp_query_set` value used by the operation.
/// @param out_pass_timings `out_pass_timings` value used by the operation.
/// @param out_cpu_pass_timings `out_cpu_pass_timings` value used by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
[[nodiscard]] Core::RendererResult RenderGraph::execute(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                                         RHI::QuerySetHandle timestamp_query_set,
                                                         vector<GpuPassTiming> *out_pass_timings,
                                                         vector<CpuPassTiming> *out_cpu_pass_timings) {
            ZoneScopedN("RenderGraph::execute");


            CompileResult compiled = compile();
            if (!compiled.has_value()) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    compiled.error().message.cpp_string());
            }
            const vector<OrderedPass> &execution_order = compiled->order;
            if (Core::RendererResult created = create_transient_resources(device, execution_order); !created.has_value()) {
                destroy_transient_resources(device);
                return created;
            }

            const bool timing_enabled = static_cast<bool>(timestamp_query_set) && out_pass_timings != nullptr;
            if (timing_enabled) {
                out_pass_timings->clear();
                out_pass_timings->reserve(execution_order.size());
                encoder.reset_query_set(timestamp_query_set, 0, static_cast<u32>(execution_order.size() * 2));
            }
            const bool cpu_timing_enabled = out_cpu_pass_timings != nullptr;
            if (cpu_timing_enabled) {
                out_cpu_pass_timings->clear();
                out_cpu_pass_timings->reserve(execution_order.size());
            }

            for (usize i = 0; i < execution_order.size(); ++i) {
                GpuPassTiming gpu_timing{};
                CpuPassTiming cpu_timing{};
                Core::RendererResult result =
                    execute_one_pass(encoder, execution_order[i], static_cast<u32>(i * 2), timestamp_query_set,
                                     timing_enabled, cpu_timing_enabled, timing_enabled ? &gpu_timing : nullptr,
                                     cpu_timing_enabled ? &cpu_timing : nullptr);
                if (!result.has_value()) {
                    destroy_transient_resources(device);
                    return result;
                }
                if (cpu_timing_enabled) {
                    out_cpu_pass_timings->push_back(std::move(cpu_timing));
                }
                if (timing_enabled) {
                    out_pass_timings->push_back(std::move(gpu_timing));
                }
            }

            Core::RendererResult final_transitions = transition_to_final_states(encoder);
            if (!final_transitions.has_value()) {
                destroy_transient_resources(device);
            }
            return final_transitions;
        }

/// Executes one pass.
///
/// @param encoder `encoder` value used by the operation.
/// @param ordered `ordered` value used by the operation.
/// @param begin_query_index Zero-based index of the target element or entry.
/// @param timestamp_query_set `timestamp_query_set` value used by the operation.
/// @param timing_enabled `timing_enabled` value used by the operation.
/// @param cpu_timing_enabled `cpu_timing_enabled` value used by the operation.
/// @param out_gpu_timing `out_gpu_timing` value used by the operation.
/// @param out_cpu_timing `out_cpu_timing` value used by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
[[nodiscard]] Core::RendererResult RenderGraph::execute_one_pass(RHI::CommandEncoder &encoder, const OrderedPass &ordered,
                                                                   u32 begin_query_index, RHI::QuerySetHandle timestamp_query_set,
                                                                   bool timing_enabled, bool cpu_timing_enabled,
                                                                   GpuPassTiming *out_gpu_timing, CpuPassTiming *out_cpu_timing) {
            ZoneScopedN("RenderGraph::execute_one_pass");
            if (timing_enabled) {
                encoder.write_timestamp(RHI::PipelineStage::AllCommands, timestamp_query_set, begin_query_index);
            }
            const CpuClock::time_point cpu_begin = cpu_timing_enabled ? CpuClock::now() : CpuClock::time_point{};
            Core::RendererResult result = {};
            const UString *label = &render_graph_copy_label;
            switch (ordered.kind) {
                case PassKind::Render: {
                    RenderGraphRenderPassBuilder &pass = render_passes_[ordered.index];
                    label = &pass.label_;
                    result = with_debug_group(encoder, *label, [&]() {
                        return execute_render_pass(encoder, pass);
                    });
                    break;
                }
                case PassKind::Blit: {
                    const RenderGraphBlitDesc &pass = blit_passes_[ordered.index];
                    label = pass.label.empty() ? &render_graph_blit_label : &pass.label;
                    result = with_debug_group(encoder, *label, [&]() {
                        return execute_blit_pass(encoder, pass);
                    });
                    break;
                }
                case PassKind::Compute: {
                    RenderGraphComputePassBuilder &pass = compute_passes_[ordered.index];
                    label = &pass.label_;
                    result = with_debug_group(encoder, *label, [&]() {
                        return execute_compute_pass(encoder, pass);
                    });
                    break;
                }
                case PassKind::Copy: {
                    const RenderGraphCopyDesc &pass = copy_passes_[ordered.index];
                    label = pass.label.empty() ? &render_graph_copy_label : &pass.label;
                    result = with_debug_group(encoder, *label, [&]() {
                        return execute_copy_pass(encoder, pass);
                    });
                    break;
                }
            }
            if (!label->empty()) {
                ZoneText(label->data(), label->byte_size());
            }
            if (!result.has_value()) {
                return result;
            }
            if (out_cpu_timing != nullptr) {
                const f64 ms = std::chrono::duration<f64, std::milli>(CpuClock::now() - cpu_begin).count();
                *out_cpu_timing = CpuPassTiming{.label = *label, .duration_ms = ms};
            }
            if (timing_enabled) {
                const u32 end_query_index = begin_query_index + 1;
                encoder.write_timestamp(RHI::PipelineStage::AllCommands, timestamp_query_set, end_query_index);
                if (out_gpu_timing != nullptr) {
                    *out_gpu_timing = GpuPassTiming{
                        .label = *label,
                        .begin_query_index = begin_query_index,
                        .end_query_index = end_query_index,
                    };
                }
            }
            return {};
        }

/// Computes execution levels using the supplied arguments and current state.
///
/// @param execution_order `execution_order` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] vector<u32> RenderGraph::compute_execution_levels(const vector<OrderedPass> &execution_order) const {
            ZoneScopedN("RenderGraph::compute_execution_levels");
            vector<PassUsage> usage(execution_order.size());
            for (usize i = 0; i < execution_order.size(); ++i) {
                usage[i] = usage_of_ordered(execution_order[i]);
            }
            return compute_levels_from_usage(usage);
        }


/// Computes levels from usage using the supplied arguments and current state.
///
/// @param usage_by_position `usage_by_position` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] vector<u32> RenderGraph::compute_levels_from_usage(const vector<PassUsage> &usage_by_position) const {
            ZoneScopedN("RenderGraph::compute_levels_from_usage");
            const usize count = usage_by_position.size();
            const usize logical_key_offset = physical_slots_.size();
            vector<i64> last_touch_pos(physical_slots_.size() + textures_.size(), -1);
            vector<i64> last_buffer_touch_pos(buffers_.size(), -1);
            const auto touch_key = [this, logical_key_offset](RenderGraphTextureHandle handle) -> usize {
                const TextureRecord *record = texture_record(handle);
                if (record == nullptr) {
                    return std::numeric_limits<usize>::max();
                }
                if (record->physical_slot < physical_slots_.size()) {
                    return record->physical_slot;
                }
                return logical_key_offset + handle.index;
            };
            vector<u32> level(count, 0);
            for (usize i = 0; i < count; ++i) {
                u32 pass_level = 0;
                for (const vector<RenderGraphTextureHandle> *handles : {&usage_by_position[i].reads, &usage_by_position[i].writes}) {
                    for (RenderGraphTextureHandle handle : *handles) {
                        const usize key = touch_key(handle);
                        if (key >= last_touch_pos.size()) {
                            continue;
                        }
                        const i64 toucher = last_touch_pos[key];
                        if (toucher >= 0) {
                            pass_level = std::max(pass_level, level[static_cast<usize>(toucher)] + 1);
                        }
                    }
                }
                for (const vector<RenderGraphBufferHandle> *handles : {
                         &usage_by_position[i].buffer_reads, &usage_by_position[i].buffer_writes}) {
                    for (RenderGraphBufferHandle handle : *handles) {
                        if (handle.index < last_buffer_touch_pos.size()) {
                            const i64 toucher = last_buffer_touch_pos[handle.index];
                            if (toucher >= 0) {
                                pass_level = std::max(pass_level, level[static_cast<usize>(toucher)] + 1);
                            }
                        }
                    }
                }
                level[i] = pass_level;
                for (const vector<RenderGraphTextureHandle> *handles : {&usage_by_position[i].reads, &usage_by_position[i].writes}) {
                    for (RenderGraphTextureHandle handle : *handles) {
                        const usize key = touch_key(handle);
                        if (key < last_touch_pos.size()) {
                            last_touch_pos[key] = static_cast<i64>(i);
                        }
                    }
                }
                for (const vector<RenderGraphBufferHandle> *handles : {
                         &usage_by_position[i].buffer_reads, &usage_by_position[i].buffer_writes}) {
                    for (RenderGraphBufferHandle handle : *handles) {
                        if (handle.index < last_buffer_touch_pos.size()) {
                            last_buffer_touch_pos[handle.index] = static_cast<i64>(i);
                        }
                    }
                }
            }
            return level;
        }

/// Executes parallel.
///
/// @param device Device used or affected by the operation.
/// @param primary_encoder `primary_encoder` value used by the operation.
/// @param queue Queue used or affected by the operation.
/// @param out_command_buffers Buffer used or affected by the operation.
/// @param timestamp_query_set `timestamp_query_set` value used by the operation.
/// @param out_pass_timings `out_pass_timings` value used by the operation.
/// @param out_cpu_pass_timings `out_cpu_pass_timings` value used by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
[[nodiscard]] Core::RendererResult RenderGraph::execute_parallel(RHI::RhiDevice &device,
                                                                   unique_ptr<RHI::CommandEncoder> primary_encoder,
                                                                   RHI::QueueLane queue,
                                                                   vector<RHI::CommandBufferHandle> &out_command_buffers,
                                                                   RHI::QuerySetHandle timestamp_query_set,
                                                                   vector<GpuPassTiming> *out_pass_timings,
                                                                   vector<CpuPassTiming> *out_cpu_pass_timings) {
            ZoneScopedN("RenderGraph::execute_parallel");


            auto fail = [&](Core::RendererResult result) {
                for (RHI::CommandBufferHandle handle : out_command_buffers) {
                    device.destroy_command_buffer(handle);
                }
                out_command_buffers.clear();
                destroy_transient_resources(device);
                return result;
            };

            CompileResult compiled = [&] {
                ZoneScopedN("RenderGraph::compile");
                return compile();
            }();
            if (!compiled.has_value()) {
                return fail(Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                         compiled.error().message.cpp_string()));
            }
            const vector<OrderedPass> &execution_order = compiled->order;
            Core::RendererResult created = [&] {
                ZoneScopedN("RenderGraph::create_transient_resources");
                return create_transient_resources(device, execution_order);
            }();
            if (!created.has_value()) {
                return fail(created);
            }

            const bool timing_enabled = static_cast<bool>(timestamp_query_set) && out_pass_timings != nullptr;
            const bool cpu_timing_enabled = out_cpu_pass_timings != nullptr;
            if (timing_enabled) {
                out_pass_timings->assign(execution_order.size(), GpuPassTiming{});
            }
            if (cpu_timing_enabled) {
                out_cpu_pass_timings->assign(execution_order.size(), CpuPassTiming{});
            }


            if (timing_enabled) {
                primary_encoder->reset_query_set(timestamp_query_set, 0, static_cast<u32>(execution_order.size() * 2));
            }


            if (execution_order.size() < 2 || Async::Scheduler::worker_count() <= 1) {
                {
                    ZoneScopedN("RenderGraph::execute_serial_passes");
                    for (usize i = 0; i < execution_order.size(); ++i) {
                        Core::RendererResult result = execute_one_pass(
                            *primary_encoder, execution_order[i], static_cast<u32>(i * 2), timestamp_query_set, timing_enabled,
                            cpu_timing_enabled, timing_enabled ? &(*out_pass_timings)[i] : nullptr,
                            cpu_timing_enabled ? &(*out_cpu_pass_timings)[i] : nullptr);
                        if (!result.has_value()) {
                            return fail(result);
                        }
                    }
                }
                Core::RendererResult final_transitions = transition_to_final_states(*primary_encoder);
                if (!final_transitions.has_value()) {
                    return fail(final_transitions);
                }
                auto finished = primary_encoder->finish();
                if (!finished) {
                    return fail(Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                             "execute_parallel: failed to finish command encoder."));
                }
                out_command_buffers.push_back(*finished);
                return {};
            }

            {
                auto finished_primary = primary_encoder->finish();
                if (!finished_primary) {
                    return fail(Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                             "execute_parallel: failed to finish the primary command encoder."));
                }
                out_command_buffers.push_back(*finished_primary);
            }


            const vector<u32> levels = compute_execution_levels(execution_order);
            u32 max_level = 0;
            for (u32 l : levels) {
                max_level = std::max(max_level, l);
            }
            vector<vector<usize>> positions_by_level(static_cast<usize>(max_level) + 1);
            for (usize i = 0; i < execution_order.size(); ++i) {
                positions_by_level[levels[i]].push_back(i);
            }

            for (const vector<usize> &positions : positions_by_level) {
                if (positions.empty()) {
                    continue;
                }
                if (positions.size() == 1) {
                    const usize i = positions.front();
                    auto encoder = device.create_command_encoder(RHI::CommandEncoderDesc{.queue = queue, .label = "render graph pass"});
                    if (!encoder) {
                        return fail(Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                                 "execute_parallel: failed to create command encoder."));
                    }
                    Core::RendererResult result = execute_one_pass(
                        **encoder, execution_order[i], static_cast<u32>(i * 2), timestamp_query_set, timing_enabled,
                        cpu_timing_enabled, timing_enabled ? &(*out_pass_timings)[i] : nullptr,
                        cpu_timing_enabled ? &(*out_cpu_pass_timings)[i] : nullptr);
                    if (!result.has_value()) {
                        return fail(result);
                    }
                    auto finished = (*encoder)->finish();
                    if (!finished) {
                        return fail(Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                                 "execute_parallel: failed to finish command encoder."));
                    }
                    out_command_buffers.push_back(*finished);
                    continue;
                }

                struct LevelPassGroup {
                    Core::RendererResult status{};
                    RHI::CommandBufferHandle command_buffer{};
                    unique_ptr<RHI::CommandEncoder> encoder;
                    usize begin = 0;
                    usize end = 0;
                };


                const usize worker_count = std::max<usize>(1, Async::Scheduler::worker_count());
                const usize group_count = std::min(positions.size(), worker_count);
                vector<LevelPassGroup> groups(group_count);
                {
                    const usize base = positions.size() / group_count;
                    const usize remainder = positions.size() % group_count;
                    usize cursor = 0;
                    for (usize g = 0; g < group_count; ++g) {
                        const usize count = base + (g < remainder ? 1 : 0);
                        groups[g].begin = cursor;
                        groups[g].end = cursor + count;
                        cursor += count;
                    }
                }


                {
                    ZoneScopedN("RenderGraph::create_level_encoders");
                    for (LevelPassGroup &group : groups) {
                        auto encoder = device.create_command_encoder(
                            RHI::CommandEncoderDesc{.queue = queue, .label = "render graph pass (parallel)"});
                        if (!encoder) {
                            return fail(Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                                     "execute_parallel: failed to create command encoder."));
                        }
                        group.encoder = std::move(*encoder);
                    }
                }

                {
                    ZoneScopedN("RenderGraph::execute_level_tasks");
                    vector<Async::TaskHandle<void>> tasks;
                    tasks.reserve(groups.size());
                    for (usize g = 0; g < groups.size(); ++g) {
                        tasks.push_back(Async::Scheduler::spawn([this, &execution_order, &positions, &groups, g,
                                                                  timestamp_query_set, timing_enabled, cpu_timing_enabled,
                                                                  out_pass_timings, out_cpu_pass_timings]() {
                            LevelPassGroup &group = groups[g];
                            RHI::CommandEncoder &encoder = *group.encoder;
                            for (usize slot = group.begin; slot < group.end; ++slot) {
                                const usize i = positions[slot];
                                Core::RendererResult result = execute_one_pass(
                                    encoder, execution_order[i], static_cast<u32>(i * 2), timestamp_query_set, timing_enabled,
                                    cpu_timing_enabled, timing_enabled ? &(*out_pass_timings)[i] : nullptr,
                                    cpu_timing_enabled ? &(*out_cpu_pass_timings)[i] : nullptr);
                                if (!result.has_value()) {
                                    group.status = result;
                                    return;
                                }
                            }
                            auto finished = encoder.finish();
                            if (!finished) {
                                group.status = Core::graphics_backend_error(
                                    Core::GraphicsBackendErrorCode::OperationFailed, "execute_parallel: failed to finish command encoder.");
                                return;
                            }
                            group.command_buffer = *finished;
                        }));
                    }
                    for (const Async::TaskHandle<void> &task : tasks) {
                        task.wait();
                    }
                }

                Core::RendererResult first_error{};
                bool has_error = false;
                for (LevelPassGroup &group : groups) {
                    if (!group.status.has_value()) {
                        if (!has_error) {
                            first_error = group.status;
                            has_error = true;
                        }
                        continue;
                    }
                    out_command_buffers.push_back(group.command_buffer);
                }
                if (has_error) {
                    return fail(first_error);
                }
            }

            {
                ZoneScopedN("RenderGraph::transition_to_final_states");
                auto epilogue = device.create_command_encoder(RHI::CommandEncoderDesc{.queue = queue, .label = "render graph final transitions"});
                if (!epilogue) {
                    return fail(Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                             "execute_parallel: failed to create the final-transitions command encoder."));
                }
                Core::RendererResult final_transitions = transition_to_final_states(**epilogue);
                if (!final_transitions.has_value()) {
                    return fail(final_transitions);
                }
                auto finished_epilogue = (*epilogue)->finish();
                if (!finished_epilogue) {
                    return fail(Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                             "execute_parallel: failed to finish the final-transitions command encoder."));
                }
                out_command_buffers.push_back(*finished_epilogue);
            }
            return {};
        }

/// Destroys the transient resources identified by the supplied parameters.
///
/// @param device Device used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void RenderGraph::destroy_transient_resources(RHI::RhiDevice &device) noexcept {
            ZoneScopedN("RenderGraph::destroy_transient_resources");


            for (PhysicalSlot &slot : physical_slots_) {
                if (!slot.owns_resource) {
                    continue;
                }
                if (slot.default_view) {
                    device.destroy_texture_view(slot.default_view);
                }
                if (slot.texture) {
                    device.destroy_texture(slot.texture);
                }
                slot.texture = {};
                slot.default_view = {};
            }
        }

/// Performs the take transient resources operation for `Renderer` using the supplied arguments.
///
/// @param textures Texture used or affected by the operation.
/// @param views `views` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void RenderGraph::take_transient_resources(vector<RHI::TextureHandle> &textures,
                                      vector<RHI::TextureViewHandle> &views) {
            ZoneScopedN("RenderGraph::take_transient_resources");
            for (PhysicalSlot &slot : physical_slots_) {
                if (!slot.owns_resource) {
                    continue;
                }
                if (slot.texture) {
                    textures.push_back(slot.texture);
                }
                if (slot.default_view) {
                    views.push_back(slot.default_view);
                }
                slot.texture = {};
                slot.default_view = {};
            }
        }

/// Resets the object to its baseline state.
///
/// @return Returns the current reset value.
/// @note This function does not throw exceptions.
void RenderGraph::reset() noexcept {
            ZoneScopedN("RenderGraph::reset");
            ordered_passes_.clear();
            render_passes_.clear();
            blit_passes_.clear();
            compute_passes_.clear();
            copy_passes_.clear();
            outputs_.clear();
            textures_.clear();
            physical_slots_.clear();
            buffers_.clear();
        }

/// Performs the texture record operation for `Renderer` using the supplied arguments.
///
/// @param handle Handle identifying the target object or resource.
///
/// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
/// @note Absence is represented by a null pointer rather than an exception.
/// @note This function does not throw exceptions.
[[nodiscard]] RenderGraph::TextureRecord *RenderGraph::texture_record(RenderGraphTextureHandle handle) noexcept {
            ZoneScopedN("RenderGraph::texture_record");
            if (!handle || handle.index >= textures_.size()) {
                return nullptr;
            }
            return &textures_[handle.index];
        }

/// Performs the texture record operation for `Renderer` using the supplied arguments.
///
/// @param handle Handle identifying the target object or resource.
///
/// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
/// @note Absence is represented by a null pointer rather than an exception.
/// @note This function does not throw exceptions.
[[nodiscard]] const RenderGraph::TextureRecord *RenderGraph::texture_record(RenderGraphTextureHandle handle) const noexcept {
            ZoneScopedN("RenderGraph::texture_record");
            if (!handle || handle.index >= textures_.size()) {
                return nullptr;
            }
            return &textures_[handle.index];
        }

/// Performs the buffer record operation for `Renderer` using the supplied arguments.
///
/// @param handle Handle identifying the target object or resource.
///
/// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
/// @note Absence is represented by a null pointer rather than an exception.
/// @note This function does not throw exceptions.
[[nodiscard]] RenderGraph::BufferRecord *RenderGraph::buffer_record(RenderGraphBufferHandle handle) noexcept {
            ZoneScopedN("RenderGraph::buffer_record");
            if (!handle || handle.index >= buffers_.size()) {
                return nullptr;
            }
            return &buffers_[handle.index];
        }

/// Performs the buffer record operation for `Renderer` using the supplied arguments.
///
/// @param handle Handle identifying the target object or resource.
///
/// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
/// @note Absence is represented by a null pointer rather than an exception.
/// @note This function does not throw exceptions.
[[nodiscard]] const RenderGraph::BufferRecord *RenderGraph::buffer_record(RenderGraphBufferHandle handle) const noexcept {
            ZoneScopedN("RenderGraph::buffer_record");
            if (!handle || handle.index >= buffers_.size()) {
                return nullptr;
            }
            return &buffers_[handle.index];
        }

/// Resolves the physical slot associated with the supplied key, handle, or resource.
///
/// @param handle Handle identifying the target object or resource.
///
/// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
/// @note Absence is represented by a null pointer rather than an exception.
/// @note This function does not throw exceptions.
[[nodiscard]] RenderGraph::PhysicalSlot *RenderGraph::physical_slot_for(RenderGraphTextureHandle handle) noexcept {
            ZoneScopedN("RenderGraph::physical_slot_for");
            TextureRecord *record = texture_record(handle);
            if (record == nullptr || record->physical_slot >= physical_slots_.size()) {
                return nullptr;
            }
            return &physical_slots_[record->physical_slot];
        }

/// Resolves the physical slot associated with the supplied key, handle, or resource.
///
/// @param handle Handle identifying the target object or resource.
///
/// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
/// @note Absence is represented by a null pointer rather than an exception.
/// @note This function does not throw exceptions.
[[nodiscard]] const RenderGraph::PhysicalSlot *RenderGraph::physical_slot_for(RenderGraphTextureHandle handle) const noexcept {
            ZoneScopedN("RenderGraph::physical_slot_for");
            const TextureRecord *record = texture_record(handle);
            if (record == nullptr || record->physical_slot >= physical_slots_.size()) {
                return nullptr;
            }
            return &physical_slots_[record->physical_slot];
        }

/// Performs the transition buffer operation for `Renderer` using the supplied arguments.
///
/// @param encoder `encoder` value used by the operation.
/// @param access `access` value used by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
[[nodiscard]] Core::RendererResult RenderGraph::transition_buffer(
            RHI::CommandEncoder &encoder, const RenderGraphBufferAccessDesc &access) {
            ZoneScopedN("RenderGraph::transition_buffer");
            BufferRecord *record = buffer_record(access.buffer);
            if (record == nullptr || !record->imported.buffer) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Render graph pass references an unknown buffer.");
            }
            if (record->stage != RHI::PipelineStage::None || record->access != RHI::AccessFlags::None) {
                const RHI::BufferBarrier barrier{
                    .buffer = record->imported.buffer,
                    .src_stage = record->stage,
                    .src_access = record->access,
                    .dst_stage = access.stages,
                    .dst_access = access.access,


                    .offset = 0,
                    .size = 0,
                };
                encoder.barrier({}, span<const RHI::BufferBarrier>{&barrier, 1}, {});
            }
            record->stage = access.stages;
            record->access = access.access;
            return {};
        }

/// Performs the transition texture operation for `Renderer` using the supplied arguments.
///
/// @param encoder `encoder` value used by the operation.
/// @param handle Handle identifying the target object or resource.
/// @param next_layout `next_layout` value used by the operation.
/// @param next_stage `next_stage` value used by the operation.
/// @param next_access `next_access` value used by the operation.
/// @param subresources `subresources` value used by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
[[nodiscard]] Core::RendererResult RenderGraph::transition_texture(RHI::CommandEncoder &encoder,
                                                              RenderGraphTextureHandle handle,
                                                              RHI::TextureLayout next_layout,
                                                              RHI::PipelineStage next_stage,
                                                              RHI::AccessFlags next_access,
                                                              RHI::TextureSubresourceRange subresources) {
            ZoneScopedN("RenderGraph::transition_texture");


            PhysicalSlot *slot = physical_slot_for(handle);
            const TextureRecord *record = texture_record(handle);
            if (slot == nullptr || record == nullptr || !slot->texture || slot->mip_states.empty()) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Render graph pass references an unknown texture.");
            }

            const u32 first_mip = subresources.base_mip_level;
            const u32 available = first_mip < record->mip_levels ? record->mip_levels - first_mip : 0u;
            const u32 mip_count = subresources.mip_level_count == RHI::all_remaining
                ? available
                : std::min(subresources.mip_level_count, available);
            if (mip_count == 0) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Render graph pass references an invalid texture mip range.");
            }

            for (u32 mip = first_mip; mip < first_mip + mip_count; ++mip) {
                TextureState &state = slot->mip_states[mip];
                if (state.layout == next_layout && state.stage == next_stage && state.access == next_access) {
                    continue;
                }
                const RHI::TextureBarrier barrier{
                    .texture = slot->texture,
                    .src_stage = state.stage,
                    .src_access = state.access,
                    .dst_stage = next_stage,
                    .dst_access = next_access,
                    .old_layout = state.layout,
                    .new_layout = next_layout,
                    .range = RHI::TextureSubresourceRange{
                        .base_mip_level = mip,
                        .mip_level_count = 1,
                        .base_array_layer = subresources.base_array_layer,
                        .array_layer_count = subresources.array_layer_count,
                    },
                };
                encoder.barrier({}, {}, span<const RHI::TextureBarrier>{&barrier, 1});
                state = TextureState{.layout = next_layout, .stage = next_stage, .access = next_access};
            }
            return {};
        }

/// Executes render pass.
///
/// @param encoder `encoder` value used by the operation.
/// @param pass Render-pass encoder that receives the draw commands.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
[[nodiscard]] Core::RendererResult RenderGraph::execute_render_pass(RHI::CommandEncoder &encoder,
                                                               RenderGraphRenderPassBuilder &pass) {
            ZoneScopedN("RenderGraph::execute_render_pass");
            for (const RenderGraphBufferAccessDesc &access : pass.buffers_) {
                if (Core::RendererResult transition = transition_buffer(encoder, access); !transition.has_value()) {
                    return transition;
                }
            }
            for (const RenderGraphSampledTextureReadDesc &read : pass.sampled_texture_reads_) {
                Core::RendererResult transition = transition_texture(encoder,
                                                                     read.texture,
                                                                     RHI::TextureLayout::ShaderReadOnly,
                                                                     read.stages,
                                                                     read.access,
                                                                     read.subresources);
                if (!transition.has_value()) {
                    return transition;
                }
            }

            vector<RHI::ColorAttachment> color_attachments;
            color_attachments.reserve(pass.color_attachments_.size());

            for (const RenderGraphColorAttachmentDesc &attachment : pass.color_attachments_) {
                PhysicalSlot *slot = physical_slot_for(attachment.texture);
                if (slot == nullptr) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Render graph color attachment references an unknown texture.");
                }
                const RHI::AccessFlags attachment_access = attachment.load_op == RHI::LoadOp::Load
                    ? RHI::AccessFlags::ColorAttachmentRead | RHI::AccessFlags::ColorAttachmentWrite
                    : RHI::AccessFlags::ColorAttachmentWrite;
                Core::RendererResult transition = transition_texture(encoder,
                                                                     attachment.texture,
                                                                     RHI::TextureLayout::ColorAttachment,
                                                                     RHI::PipelineStage::ColorAttachmentOutput,
                                                                     attachment_access,
                                                                     attachment.subresources);
                if (!transition.has_value()) {
                    return transition;
                }
                RHI::TextureViewHandle resolve_view{};
                if (attachment.resolve_texture) {
                    PhysicalSlot *resolve_slot = physical_slot_for(attachment.resolve_texture);
                    if (resolve_slot == nullptr) {
                        return Core::graphics_backend_error(
                            Core::GraphicsBackendErrorCode::OperationFailed,
                            "Render graph color resolve references an unknown texture.");
                    }
                    transition = transition_texture(encoder,
                                                    attachment.resolve_texture,
                                                    RHI::TextureLayout::ColorAttachment,
                                                    RHI::PipelineStage::ColorAttachmentOutput,
                                                    RHI::AccessFlags::ColorAttachmentWrite,
                                                    attachment.resolve_subresources);
                    if (!transition.has_value()) {
                        return transition;
                    }
                    resolve_view = attachment.resolve_view ? attachment.resolve_view
                                                           : resolve_slot->default_view;
                }
                color_attachments.push_back(RHI::ColorAttachment{
                    .view = attachment.view ? attachment.view : slot->default_view,
                    .resolve_view = resolve_view,
                    .load_op = attachment.load_op,
                    .store_op = attachment.store_op,
                    .clear_color = attachment.clear_color,
                });
            }

            RHI::DepthStencilAttachment depth_stencil{};
            if (pass.has_depth_stencil_attachment_) {
                const RenderGraphDepthStencilAttachmentDesc &attachment = pass.depth_stencil_attachment_;
                PhysicalSlot *slot = physical_slot_for(attachment.texture);
                if (slot == nullptr) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Render graph depth/stencil attachment references an unknown texture.");
                }
                Core::RendererResult transition = transition_texture(encoder,
                                                                     attachment.texture,
                                                                     RHI::TextureLayout::DepthStencilAttachment,
                                                                     RHI::PipelineStage::EarlyFragmentTests | RHI::PipelineStage::LateFragmentTests,
                                                                     RHI::AccessFlags::DepthStencilAttachmentRead | RHI::AccessFlags::DepthStencilAttachmentWrite,
                                                                     attachment.subresources);
                if (!transition.has_value()) {
                    return transition;
                }
                RHI::TextureViewHandle resolve_view{};
                if (attachment.resolve_texture) {
                    PhysicalSlot *resolve_slot = physical_slot_for(attachment.resolve_texture);
                    if (resolve_slot == nullptr) {
                        return Core::graphics_backend_error(
                            Core::GraphicsBackendErrorCode::OperationFailed,
                            "Render graph depth resolve references an unknown texture.");
                    }
                    transition = transition_texture(encoder,
                                                    attachment.resolve_texture,
                                                    RHI::TextureLayout::DepthStencilAttachment,
                                                    RHI::PipelineStage::EarlyFragmentTests |
                                                        RHI::PipelineStage::LateFragmentTests,
                                                    RHI::AccessFlags::DepthStencilAttachmentWrite,
                                                    attachment.resolve_subresources);
                    if (!transition.has_value()) {
                        return transition;
                    }
                    resolve_view = attachment.resolve_view ? attachment.resolve_view
                                                           : resolve_slot->default_view;
                }
                depth_stencil = RHI::DepthStencilAttachment{
                    .view = attachment.view ? attachment.view : slot->default_view,
                    .resolve_view = resolve_view,
                    .depth_resolve_mode = attachment.depth_resolve_mode,
                    .depth_load_op = attachment.depth_load_op,
                    .depth_store_op = attachment.depth_store_op,
                    .stencil_load_op = attachment.stencil_load_op,
                    .stencil_store_op = attachment.stencil_store_op,
                    .clear_value = attachment.clear_value,
                };
            }

            const RHI::RenderPassDesc pass_desc{
                .color_attachments = span<const RHI::ColorAttachment>{color_attachments.data(), color_attachments.size()},
                .depth_stencil = depth_stencil,
                .render_area = pass.render_area_,
                .view_mask = pass.view_mask_,
                .allow_bundles = pass.allow_bundles_,
                .label = pass.label_.empty() ? nullptr : pass.label_.c_str(),
            };
            auto render_pass = encoder.begin_render_pass(pass_desc);
            if (!render_pass) {
                return Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    render_graph_labelled_error("begin render graph pass"_ustr, pass.label_, render_pass.error().message));
            }

            if (pass.execute_) {
                RenderGraphContext context{*this, encoder, **render_pass};
                Core::RendererResult result = pass.execute_(context);
                if (!result.has_value()) {
                    (*render_pass)->end();
                    return result;
                }
            }
            (*render_pass)->end();
            return {};
        }

/// Executes blit pass.
///
/// @param encoder `encoder` value used by the operation.
/// @param pass Render-pass encoder that receives the draw commands.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
[[nodiscard]] Core::RendererResult RenderGraph::execute_blit_pass(RHI::CommandEncoder &encoder, const RenderGraphBlitDesc &pass) {
            ZoneScopedN("RenderGraph::execute_blit_pass");
            const TextureRecord *source_record = texture_record(pass.source);
            const TextureRecord *destination_record = texture_record(pass.destination);
            PhysicalSlot *source = physical_slot_for(pass.source);
            PhysicalSlot *destination = physical_slot_for(pass.destination);
            if (source_record == nullptr || destination_record == nullptr || source == nullptr || destination == nullptr) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Render graph blit pass references an unknown texture.");
            }

            Core::RendererResult src_transition = transition_texture(encoder,
                                                                     pass.source,
                                                                     RHI::TextureLayout::TransferSrc,
                                                                     RHI::PipelineStage::Transfer,
                                                                     RHI::AccessFlags::TransferRead);
            if (!src_transition.has_value()) {
                return src_transition;
            }
            Core::RendererResult dst_transition = transition_texture(encoder,
                                                                     pass.destination,
                                                                     RHI::TextureLayout::TransferDst,
                                                                     RHI::PipelineStage::Transfer,
                                                                     RHI::AccessFlags::TransferWrite);
            if (!dst_transition.has_value()) {
                return dst_transition;
            }

            const RHI::TextureBlit blit{
                .src_subresource = RHI::TextureSubresourceLayers{.mip_level = 0, .base_array_layer = 0, .array_layer_count = 1},
                .src_min = RHI::Offset3D{0, 0, 0},
                .src_max = RHI::Offset3D{static_cast<i32>(source_record->extent.width),
                                         static_cast<i32>(source_record->extent.height),
                                         static_cast<i32>(source_record->extent.depth_or_layers)},
                .dst_subresource = RHI::TextureSubresourceLayers{.mip_level = 0, .base_array_layer = 0, .array_layer_count = 1},
                .dst_min = RHI::Offset3D{0, 0, 0},
                .dst_max = RHI::Offset3D{static_cast<i32>(destination_record->extent.width),
                                         static_cast<i32>(destination_record->extent.height),
                                         static_cast<i32>(destination_record->extent.depth_or_layers)},
            };
            encoder.blit_texture(source->texture, destination->texture, blit, pass.filter);
            return {};
        }

/// Executes compute pass.
///
/// @param encoder `encoder` value used by the operation.
/// @param pass Render-pass encoder that receives the draw commands.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
[[nodiscard]] Core::RendererResult RenderGraph::execute_compute_pass(RHI::CommandEncoder &encoder,
                                                                 RenderGraphComputePassBuilder &pass) {
            ZoneScopedN("RenderGraph::execute_compute_pass");
            for (const RenderGraphBufferAccessDesc &access : pass.buffers_) {
                if (Core::RendererResult transition = transition_buffer(encoder, access); !transition.has_value()) {
                    return transition;
                }
            }
            for (RenderGraphTextureHandle read : pass.sampled_texture_reads_) {
                Core::RendererResult transition = transition_texture(encoder,
                                                                     read,
                                                                     RHI::TextureLayout::ShaderReadOnly,
                                                                     RHI::PipelineStage::ComputeShader,
                                                                     RHI::AccessFlags::ShaderRead);
                if (!transition.has_value()) {
                    return transition;
                }
            }
            for (const RenderGraphStorageTextureAccessDesc &access : pass.storage_textures_) {
                RHI::AccessFlags storage_access = RHI::AccessFlags::None;
                if (access.read) {
                    storage_access = storage_access | RHI::AccessFlags::ShaderRead;
                }
                if (access.write) {
                    storage_access = storage_access | RHI::AccessFlags::ShaderWrite;
                }
                Core::RendererResult transition = transition_texture(encoder,
                                                                     access.texture,
                                                                     RHI::TextureLayout::General,
                                                                     RHI::PipelineStage::ComputeShader,
                                                                     storage_access);
                if (!transition.has_value()) {
                    return transition;
                }
            }

            const RHI::ComputePassDesc pass_desc{
                .label = pass.label_.empty() ? nullptr : pass.label_.c_str(),
            };
            auto compute_pass = encoder.begin_compute_pass(pass_desc);
            if (!compute_pass) {
                return Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    render_graph_labelled_error("begin render graph compute pass"_ustr, pass.label_, compute_pass.error().message));
            }

            if (pass.execute_) {
                RenderGraphComputeContext context{*this, encoder, **compute_pass};
                Core::RendererResult result = pass.execute_(context);
                if (!result.has_value()) {
                    (*compute_pass)->end();
                    return result;
                }
            }
            (*compute_pass)->end();
            return {};
        }

/// Executes copy pass.
///
/// @param encoder `encoder` value used by the operation.
/// @param pass Render-pass encoder that receives the draw commands.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
[[nodiscard]] Core::RendererResult RenderGraph::execute_copy_pass(RHI::CommandEncoder &encoder, const RenderGraphCopyDesc &pass) {
            ZoneScopedN("RenderGraph::execute_copy_pass");
            const TextureRecord *source_record = texture_record(pass.source);
            PhysicalSlot *source = physical_slot_for(pass.source);
            PhysicalSlot *destination = physical_slot_for(pass.destination);
            if (source_record == nullptr || source == nullptr || destination == nullptr) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Render graph copy pass references an unknown texture.");
            }

            Core::RendererResult src_transition = transition_texture(encoder,
                                                                     pass.source,
                                                                     RHI::TextureLayout::TransferSrc,
                                                                     RHI::PipelineStage::Transfer,
                                                                     RHI::AccessFlags::TransferRead);
            if (!src_transition.has_value()) {
                return src_transition;
            }
            Core::RendererResult dst_transition = transition_texture(encoder,
                                                                     pass.destination,
                                                                     RHI::TextureLayout::TransferDst,
                                                                     RHI::PipelineStage::Transfer,
                                                                     RHI::AccessFlags::TransferWrite);
            if (!dst_transition.has_value()) {
                return dst_transition;
            }

            const RHI::TextureCopy copy{
                .src_subresource = RHI::TextureSubresourceLayers{.mip_level = 0, .base_array_layer = 0, .array_layer_count = 1},
                .src_offset = RHI::Offset3D{0, 0, 0},
                .dst_subresource = RHI::TextureSubresourceLayers{.mip_level = 0, .base_array_layer = 0, .array_layer_count = 1},
                .dst_offset = RHI::Offset3D{0, 0, 0},
                .extent = source_record->extent,
            };
            encoder.copy_texture_to_texture(source->texture, destination->texture, copy);
            return {};
        }

/// Computes transient lifetimes using the supplied arguments and current state.
///
/// @param execution_order `execution_order` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] vector<RenderGraph::TextureLifetime> RenderGraph::compute_transient_lifetimes(const vector<OrderedPass> &execution_order) const {
            ZoneScopedN("RenderGraph::compute_transient_lifetimes");
            vector<TextureLifetime> lifetimes(textures_.size());
            for (usize order_index = 0; order_index < execution_order.size(); ++order_index) {
                const PassUsage usage = usage_of_ordered(execution_order[order_index]);
                auto mark = [&](RenderGraphTextureHandle handle) {
                    if (!handle || handle.index >= lifetimes.size()) {
                        return;
                    }
                    TextureLifetime &lifetime = lifetimes[handle.index];
                    if (lifetime.first_use < 0) {
                        lifetime.first_use = static_cast<i32>(order_index);
                    }
                    lifetime.last_use = static_cast<i32>(order_index);
                };
                for (RenderGraphTextureHandle read : usage.reads) {
                    mark(read);
                }
                for (RenderGraphTextureHandle write : usage.writes) {
                    mark(write);
                }
            }
            return lifetimes;
        }


/// Creates a transient resources from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param execution_order `execution_order` value used by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
[[nodiscard]] Core::RendererResult RenderGraph::create_transient_resources(RHI::RhiDevice &device,
                                                                      const vector<OrderedPass> &execution_order) {
            ZoneScopedN("RenderGraph::create_transient_resources");
            const vector<TextureLifetime> lifetimes = compute_transient_lifetimes(execution_order);

            struct PendingSlot {
                RenderGraphTextureDesc desc;
                UString label;
            };
            vector<PendingSlot> pending;

            auto signature_matches = [](const RenderGraphTextureDesc &a, const RenderGraphTextureDesc &b) noexcept {
                return a.format == b.format && a.extent.width == b.extent.width && a.extent.height == b.extent.height &&
                       a.extent.depth_or_layers == b.extent.depth_or_layers && a.mip_levels == b.mip_levels &&
                       a.samples == b.samples && a.usage == b.usage;
            };

            struct Bucket {
                RenderGraphTextureDesc signature;
                vector<u32> members;
            };
            vector<Bucket> buckets;

            for (usize i = 0; i < textures_.size(); ++i) {
                if (!textures_[i].is_transient) {
                    continue;
                }
                if (lifetimes[i].first_use < 0) {


                    continue;
                }
                bool placed = false;
                for (Bucket &bucket : buckets) {
                    if (signature_matches(bucket.signature, textures_[i].transient)) {
                        bucket.members.push_back(static_cast<u32>(i));
                        placed = true;
                        break;
                    }
                }
                if (!placed) {
                    buckets.push_back(Bucket{.signature = textures_[i].transient, .members = {static_cast<u32>(i)}});
                }
            }

            for (Bucket &bucket : buckets) {
                std::sort(bucket.members.begin(), bucket.members.end(), [&](u32 a, u32 b) {
                    return lifetimes[a].first_use < lifetimes[b].first_use;
                });
                vector<pair<u32, i32>> open_slots;
                for (u32 texture_index : bucket.members) {
                    const TextureLifetime &lifetime = lifetimes[texture_index];
                    i32 reused_slot = -1;
                    for (auto &open_slot : open_slots) {
                        if (open_slot.second < lifetime.first_use) {
                            reused_slot = static_cast<i32>(open_slot.first);
                            open_slot.second = lifetime.last_use;
                            break;
                        }
                    }
                    if (reused_slot >= 0) {
                        textures_[texture_index].physical_slot = static_cast<u32>(reused_slot);
                    } else {
                        const u32 slot_index = static_cast<u32>(physical_slots_.size());
                        physical_slots_.emplace_back();
                        pending.push_back(PendingSlot{.desc = textures_[texture_index].transient,
                                                      .label = textures_[texture_index].label});
                        textures_[texture_index].physical_slot = slot_index;
                        open_slots.push_back({slot_index, lifetime.last_use});
                    }
                }
            }
            const u32 first_new_slot = static_cast<u32>(physical_slots_.size() - pending.size());
            for (usize p = 0; p < pending.size(); ++p) {
                PhysicalSlot &slot = physical_slots_[first_new_slot + p];
                const PendingSlot &pending_slot = pending[p];

                auto texture_handle = device.create_texture(RHI::TextureDesc{
                    .dimension = RHI::TextureDimension::Dim2D,
                    .format = pending_slot.desc.format,
                    .extent = pending_slot.desc.extent,
                    .mip_levels = pending_slot.desc.mip_levels,
                    .samples = pending_slot.desc.samples,
                    .usage = pending_slot.desc.usage,
                    .label = pending_slot.label.empty() ? "render graph transient texture" : pending_slot.label.c_str(),
                });
                if (!texture_handle) {
                    return Core::graphics_backend_error(
                        Core::GraphicsBackendErrorCode::OperationFailed,
                        render_graph_labelled_error("create render graph transient texture"_ustr,
                                                    pending_slot.label, texture_handle.error().message));
                }

                auto view_handle = device.create_texture_view(RHI::TextureViewDesc{
                    .texture = *texture_handle,
                    .view_type = RHI::TextureViewType::View2D,
                    .label = pending_slot.label.empty() ? "render graph transient texture view" : pending_slot.label.c_str(),
                });
                if (!view_handle) {
                    device.destroy_texture(*texture_handle);
                    return Core::graphics_backend_error(
                        Core::GraphicsBackendErrorCode::OperationFailed,
                        render_graph_labelled_error("create render graph transient texture view"_ustr,
                                                    pending_slot.label, view_handle.error().message));
                }

                slot.texture = *texture_handle;
                slot.default_view = *view_handle;
                slot.mip_states = vector<TextureState>(std::max(pending_slot.desc.mip_levels, 1u), TextureState{
                    .layout = pending_slot.desc.initial_layout,
                    .stage = pending_slot.desc.initial_stage,
                    .access = pending_slot.desc.initial_access,
                });
                slot.owns_resource = true;
            }
            return {};
        }

/// Performs the transition to final states operation for `Renderer` using the supplied arguments.
///
/// @param encoder `encoder` value used by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
[[nodiscard]] Core::RendererResult RenderGraph::transition_to_final_states(RHI::CommandEncoder &encoder) {
            ZoneScopedN("RenderGraph::transition_to_final_states");
            for (usize i = 0; i < buffers_.size(); ++i) {
                BufferRecord &record = buffers_[i];
                if (record.imported.final_stage == RHI::PipelineStage::None &&
                    record.imported.final_access == RHI::AccessFlags::None) {
                    continue;
                }
                Core::RendererResult transition = transition_buffer(
                    encoder,
                    RenderGraphBufferAccessDesc{
                        .buffer = RenderGraphBufferHandle{static_cast<u32>(i)},
                        .stages = record.imported.final_stage,
                        .access = record.imported.final_access,
                        .offset = 0,
                        .size = 0,
                    });
                if (!transition.has_value()) {
                    return transition;
                }
            }
            for (usize i = 0; i < textures_.size(); ++i) {
                TextureRecord &record = textures_[i];
                if (record.final_layout == RHI::TextureLayout::Undefined) {
                    continue;
                }


                if (record.is_transient && record.physical_slot >= physical_slots_.size()) {
                    continue;
                }
                Core::RendererResult transition = transition_texture(encoder,
                                                                     RenderGraphTextureHandle{static_cast<u32>(i)},
                                                                     record.final_layout,
                                                                     record.final_stage,
                                                                     record.final_access);
                if (!transition.has_value()) {
                    return transition;
                }
            }
            return {};
        }

/// Renders the requested content using the current rendering state.
///
/// @param graph `graph` value used by the operation.
/// @param command_encoder `command_encoder` value used by the operation.
/// @param render_pass Render-pass encoder that receives the draw commands.
///
/// @note This function does not throw exceptions.
RenderGraphContext::RenderGraphContext(RenderGraph &graph,
                                                  RHI::CommandEncoder &command_encoder,
                                                  RHI::RenderPassEncoder &render_pass) noexcept
        : graph_(&graph), command_encoder_(&command_encoder), render_pass_(&render_pass) {}

/// Returns the current or globally available command encoder value.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
RHI::CommandEncoder &RenderGraphContext::command_encoder() const noexcept {
        ZoneScopedN("RenderGraphContext::RenderGraphContext");
        return *command_encoder_;
    }

/// Renders pass using the current rendering state.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
RHI::RenderPassEncoder &RenderGraphContext::render_pass() const noexcept {
        ZoneScopedN("RenderGraphContext::render_pass");
        return *render_pass_;
    }

/// Performs the texture operation for `Renderer` using the supplied arguments.
///
/// @param handle Handle identifying the target object or resource.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
RenderGraphTextureAccess RenderGraphContext::texture(RenderGraphTextureHandle handle) const noexcept {
        ZoneScopedN("RenderGraphContext::texture");
        return graph_ != nullptr ? graph_->texture_access(handle) : RenderGraphTextureAccess{};
    }

/// Performs the buffer operation for `Renderer` using the supplied arguments.
///
/// @param handle Handle identifying the target object or resource.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
RenderGraphBufferAccess RenderGraphContext::buffer(RenderGraphBufferHandle handle) const noexcept {
        ZoneScopedN("RenderGraphContext::buffer");
        return graph_ != nullptr ? graph_->buffer_access(handle) : RenderGraphBufferAccess{};
    }

/// Renders the requested content using the current rendering state.
///
/// @param graph `graph` value used by the operation.
/// @param command_encoder `command_encoder` value used by the operation.
/// @param compute_pass `compute_pass` value used by the operation.
///
/// @note This function does not throw exceptions.
RenderGraphComputeContext::RenderGraphComputeContext(RenderGraph &graph,
                                                  RHI::CommandEncoder &command_encoder,
                                                  RHI::ComputePassEncoder &compute_pass) noexcept
        : graph_(&graph), command_encoder_(&command_encoder), compute_pass_(&compute_pass) {}

/// Returns the current or globally available command encoder value.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
RHI::CommandEncoder &RenderGraphComputeContext::command_encoder() const noexcept {
        ZoneScopedN("RenderGraphComputeContext::RenderGraphComputeContext");
        return *command_encoder_;
    }

/// Computes pass using the supplied arguments and current state.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
RHI::ComputePassEncoder &RenderGraphComputeContext::compute_pass() const noexcept {
        ZoneScopedN("RenderGraphComputeContext::compute_pass");
        return *compute_pass_;
    }

/// Performs the texture operation for `Renderer` using the supplied arguments.
///
/// @param handle Handle identifying the target object or resource.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
RenderGraphTextureAccess RenderGraphComputeContext::texture(RenderGraphTextureHandle handle) const noexcept {
        ZoneScopedN("RenderGraphComputeContext::texture");
        return graph_ != nullptr ? graph_->texture_access(handle) : RenderGraphTextureAccess{};
    }

/// Performs the buffer operation for `Renderer` using the supplied arguments.
///
/// @param handle Handle identifying the target object or resource.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
RenderGraphBufferAccess RenderGraphComputeContext::buffer(RenderGraphBufferHandle handle) const noexcept {
        ZoneScopedN("RenderGraphComputeContext::buffer");
        return graph_ != nullptr ? graph_->buffer_access(handle) : RenderGraphBufferAccess{};
    }

} // namespace SFT::Renderer
