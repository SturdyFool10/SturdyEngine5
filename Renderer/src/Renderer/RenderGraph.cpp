#include "RenderGraph.hpp"

#include <set>

namespace SFT::Renderer {

[[nodiscard]] RenderGraph::PassUsage RenderGraph::pass_usage_of(const RenderGraphRenderPassBuilder &pass) {
            PassUsage usage;
            for (const RenderGraphColorAttachmentDesc &attachment : pass.color_attachments_) {
                usage.writes.push_back(attachment.texture);
            }
            if (pass.has_depth_stencil_attachment_) {
                // Counted as both: a Load op reads the prior contents and either store op writes
                // the result. Treating it as a read too only makes the derived dependency edges
                // more conservative (an extra edge that was already going to hold in practice),
                // never wrong.
                usage.writes.push_back(pass.depth_stencil_attachment_.texture);
                usage.reads.push_back(pass.depth_stencil_attachment_.texture);
            }
            for (const RenderGraphSampledTextureReadDesc &read : pass.sampled_texture_reads_) {
                usage.reads.push_back(read.texture);
            }
            usage.always_live = pass.color_attachments_.empty() && !pass.has_depth_stencil_attachment_;
            return usage;
        }

[[nodiscard]] RenderGraph::PassUsage RenderGraph::pass_usage_of(const RenderGraphBlitDesc &pass) {
            PassUsage usage;
            usage.writes.push_back(pass.destination);
            usage.reads.push_back(pass.source);
            return usage;
        }

RenderGraphRenderPassBuilder::RenderGraphRenderPassBuilder(string label) : label_(std::move(label)) {}

RenderGraphRenderPassBuilder &RenderGraphRenderPassBuilder::add_color_attachment(const RenderGraphColorAttachmentDesc &attachment) {
            color_attachments_.push_back(attachment);
            return *this;
        }

RenderGraphRenderPassBuilder &RenderGraphRenderPassBuilder::set_depth_stencil_attachment(const RenderGraphDepthStencilAttachmentDesc &attachment) {
            depth_stencil_attachment_ = attachment;
            has_depth_stencil_attachment_ = true;
            return *this;
        }

RenderGraphRenderPassBuilder &RenderGraphRenderPassBuilder::add_sampled_texture(const RenderGraphSampledTextureReadDesc &read) {
            sampled_texture_reads_.push_back(read);
            return *this;
        }

RenderGraphRenderPassBuilder &RenderGraphRenderPassBuilder::set_render_area(const RHI::Rect2D &render_area) noexcept {
            render_area_ = render_area;
            return *this;
        }

RenderGraphRenderPassBuilder &RenderGraphRenderPassBuilder::set_view_mask(u32 view_mask) noexcept {
            view_mask_ = view_mask;
            return *this;
        }

RenderGraphRenderPassBuilder &RenderGraphRenderPassBuilder::set_execute(RenderGraphExecuteFn execute) noexcept {
            execute_ = std::move(execute);
            return *this;
        }

[[nodiscard]] RenderGraphTextureHandle RenderGraph::import_texture(const RenderGraphImportedTextureDesc &desc) {
            const RenderGraphTextureHandle handle{static_cast<u32>(textures_.size())};
            textures_.push_back(TextureRecord{
                .imported = desc,
                .is_transient = false,
                .texture = desc.texture,
                .default_view = desc.default_view,
                .format = desc.format,
                .extent = desc.extent,
                .final_layout = desc.final_layout,
                .final_stage = desc.final_stage,
                .final_access = desc.final_access,
                .current_layout = desc.initial_layout,
                .current_stage = desc.initial_stage,
                .current_access = desc.initial_access,
                .label = desc.label ? desc.label : "",
            });
            return handle;
        }

[[nodiscard]] RenderGraphTextureHandle RenderGraph::create_texture(const RenderGraphTextureDesc &desc) {
            const RenderGraphTextureHandle handle{static_cast<u32>(textures_.size())};
            textures_.push_back(TextureRecord{
                .transient = desc,
                .is_transient = true,
                .format = desc.format,
                .extent = desc.extent,
                .final_layout = desc.final_layout,
                .final_stage = desc.final_stage,
                .final_access = desc.final_access,
                .current_layout = desc.initial_layout,
                .current_stage = desc.initial_stage,
                .current_access = desc.initial_access,
                .label = desc.label ? desc.label : "",
            });
            return handle;
        }

[[nodiscard]] RenderGraphRenderPassBuilder &RenderGraph::add_render_pass(string_view label) {
            const u32 index = static_cast<u32>(render_passes_.size());
            render_passes_.emplace_back(string{label});
            ordered_passes_.push_back(OrderedPass{.kind = PassKind::Render, .index = index});
            return render_passes_.back();
        }

void RenderGraph::add_blit_pass(const RenderGraphBlitDesc &desc) {
            const u32 index = static_cast<u32>(blit_passes_.size());
            blit_passes_.push_back(desc);
            ordered_passes_.push_back(OrderedPass{.kind = PassKind::Blit, .index = index});
        }

[[nodiscard]] RenderGraphTextureAccess RenderGraph::texture_access(RenderGraphTextureHandle handle) const noexcept {
            const TextureRecord *record = texture_record(handle);
            if (record == nullptr) {
                return {};
            }
            return RenderGraphTextureAccess{
                .texture = record->texture,
                .default_view = record->default_view,
                .format = record->format,
                .extent = record->extent,
                .current_layout = record->current_layout,
            };
        }

// Derives execution order from resource dependencies and culls dead passes — see the header's doc
// comment on compile_execution_order() and the PassUsage helpers above for what's being derived
// from what. Algorithm, in three passes over the (small, per-frame) `ordered_passes_` list:
//
// 1. Build a `depends_on[i]` edge list: pass i depends on the most recent earlier pass that wrote
//    a texture pass i reads (RAW), and on the most recent earlier pass that wrote a texture pass i
//    ALSO writes (WAW — keeps two writers of the same texture in their original relative order;
//    the topo-sort below is never free to swap them).
// 2. Liveness: backward-reachability flood fill starting from every pass that writes an *imported*
//    texture (the graph's only externally-visible output — nothing outside the graph can observe a
//    transient one) or that declared no attachments at all (`always_live` — can't reason about an
//    undeclared side effect, so never culled). A pass reachable from a live pass via `depends_on`
//    is live too; anything left unmarked is genuinely dead — its output is never read by anything
//    that itself matters — and gets dropped from the returned order entirely.
// 3. Stable Kahn's-algorithm topological sort restricted to the live set: among all passes whose
//    dependencies are already scheduled, always schedule the smallest original insertion index
//    next. Every existing caller in this codebase already calls add_render_pass()/add_blit_pass()
//    in dependency order (a pass reads what an earlier add_*_pass call wrote), which is already a
//    valid topological order — so "smallest ready index first" reproduces that exact order,
//    verified by the deferred pipeline (gbuffer -> lighting -> tonemap -> UI) still rendering
//    identically after this landed. A future caller that adds passes out of dependency order would
//    still get correctly reordered instead of silently misrendering.
[[nodiscard]] vector<RenderGraph::OrderedPass> RenderGraph::compile_execution_order() const {
            const usize pass_count = ordered_passes_.size();
            vector<PassUsage> usage(pass_count);
            for (usize i = 0; i < pass_count; ++i) {
                const OrderedPass &ordered = ordered_passes_[i];
                usage[i] = ordered.kind == PassKind::Render ? pass_usage_of(render_passes_[ordered.index])
                                                             : pass_usage_of(blit_passes_[ordered.index]);
            }

            vector<i64> last_writer(textures_.size(), -1);
            vector<vector<u32>> depends_on(pass_count);
            for (usize i = 0; i < pass_count; ++i) {
                for (RenderGraphTextureHandle read : usage[i].reads) {
                    if (read.index < last_writer.size() && last_writer[read.index] >= 0) {
                        depends_on[i].push_back(static_cast<u32>(last_writer[read.index]));
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
            }

            vector<bool> live(pass_count, false);
            vector<u32> pending;
            for (usize i = 0; i < pass_count; ++i) {
                bool writes_imported = false;
                for (RenderGraphTextureHandle write : usage[i].writes) {
                    const TextureRecord *record = texture_record(write);
                    if (record != nullptr && !record->is_transient) {
                        writes_imported = true;
                        break;
                    }
                }
                if ((writes_imported || usage[i].always_live) && !live[i]) {
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

            std::set<u32> ready;
            for (usize i = 0; i < pass_count; ++i) {
                if (live[i] && in_degree[i] == 0) {
                    ready.insert(static_cast<u32>(i));
                }
            }

            vector<bool> scheduled(pass_count, false);
            vector<OrderedPass> order;
            order.reserve(pass_count);
            while (!ready.empty()) {
                const u32 i = *ready.begin();
                ready.erase(ready.begin());
                scheduled[i] = true;
                order.push_back(ordered_passes_[i]);
                for (u32 dependent : dependents[i]) {
                    if (--in_degree[dependent] == 0) {
                        ready.insert(dependent);
                    }
                }
            }
            // A cycle (never expected from any real graph — see the header comment) would leave
            // some live pass permanently at in_degree > 0; append any stragglers in original order
            // rather than silently dropping a pass that was determined to be live.
            for (usize i = 0; i < pass_count; ++i) {
                if (live[i] && !scheduled[i]) {
                    order.push_back(ordered_passes_[i]);
                }
            }
            return order;
        }

[[nodiscard]] Core::RendererResult RenderGraph::execute(RHI::RhiDevice &device, RHI::CommandEncoder &encoder) {
            if (Core::RendererResult created = create_transient_resources(device); !created.has_value()) {
                destroy_transient_resources(device);
                return created;
            }

            const vector<OrderedPass> execution_order = compile_execution_order();
            for (const OrderedPass &ordered : execution_order) {
                Core::RendererResult result = {};
                switch (ordered.kind) {
                    case PassKind::Render: {
                        RenderGraphRenderPassBuilder &pass = render_passes_[ordered.index];
                        result = with_debug_group(encoder, pass.label_, [&]() {
                            return execute_render_pass(encoder, pass);
                        });
                        break;
                    }
                    case PassKind::Blit: {
                        const RenderGraphBlitDesc &pass = blit_passes_[ordered.index];
                        result = with_debug_group(encoder, pass.label ? pass.label : "render graph blit", [&]() {
                            return execute_blit_pass(encoder, pass);
                        });
                        break;
                    }
                }
                if (!result.has_value()) {
                    destroy_transient_resources(device);
                    return result;
                }
            }

            Core::RendererResult final_transitions = transition_to_final_states(encoder);
            if (!final_transitions.has_value()) {
                destroy_transient_resources(device);
            }
            return final_transitions;
        }

void RenderGraph::destroy_transient_resources(RHI::RhiDevice &device) noexcept {
            for (TextureRecord &record : textures_) {
                if (!record.is_transient) {
                    continue;
                }
                if (record.default_view) {
                    device.destroy_texture_view(record.default_view);
                }
                if (record.texture) {
                    device.destroy_texture(record.texture);
                }
                record.texture = {};
                record.default_view = {};
            }
        }

void RenderGraph::take_transient_resources(vector<RHI::TextureHandle> &textures,
                                      vector<RHI::TextureViewHandle> &views) {
            for (TextureRecord &record : textures_) {
                if (!record.is_transient) {
                    continue;
                }
                if (record.texture) {
                    textures.push_back(record.texture);
                }
                if (record.default_view) {
                    views.push_back(record.default_view);
                }
                record.texture = {};
                record.default_view = {};
            }
        }

void RenderGraph::reset() noexcept {
            ordered_passes_.clear();
            render_passes_.clear();
            blit_passes_.clear();
            textures_.clear();
        }

[[nodiscard]] RenderGraph::TextureRecord *RenderGraph::texture_record(RenderGraphTextureHandle handle) noexcept {
            if (!handle || handle.index >= textures_.size()) {
                return nullptr;
            }
            return &textures_[handle.index];
        }

[[nodiscard]] const RenderGraph::TextureRecord *RenderGraph::texture_record(RenderGraphTextureHandle handle) const noexcept {
            if (!handle || handle.index >= textures_.size()) {
                return nullptr;
            }
            return &textures_[handle.index];
        }

[[nodiscard]] Core::RendererResult RenderGraph::transition_texture(RHI::CommandEncoder &encoder,
                                                              RenderGraphTextureHandle handle,
                                                              RHI::TextureLayout next_layout,
                                                              RHI::PipelineStage next_stage,
                                                              RHI::AccessFlags next_access) {
            TextureRecord *record = texture_record(handle);
            if (record == nullptr || !record->texture) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Render graph pass references an unknown texture.");
            }

            if (record->current_layout == next_layout && record->current_stage == next_stage &&
                record->current_access == next_access) {
                return {};
            }

            const RHI::TextureBarrier barrier{
                .texture = record->texture,
                .src_stage = record->current_stage,
                .src_access = record->current_access,
                .dst_stage = next_stage,
                .dst_access = next_access,
                .old_layout = record->current_layout,
                .new_layout = next_layout,
            };
            encoder.barrier({}, {}, span<const RHI::TextureBarrier>{&barrier, 1});
            record->current_layout = next_layout;
            record->current_stage = next_stage;
            record->current_access = next_access;
            return {};
        }

[[nodiscard]] Core::RendererResult RenderGraph::execute_render_pass(RHI::CommandEncoder &encoder,
                                                               RenderGraphRenderPassBuilder &pass) {
            for (const RenderGraphSampledTextureReadDesc &read : pass.sampled_texture_reads_) {
                Core::RendererResult transition = transition_texture(encoder,
                                                                     read.texture,
                                                                     RHI::TextureLayout::ShaderReadOnly,
                                                                     read.stages,
                                                                     read.access);
                if (!transition.has_value()) {
                    return transition;
                }
            }

            vector<RHI::ColorAttachment> color_attachments;
            color_attachments.reserve(pass.color_attachments_.size());

            for (const RenderGraphColorAttachmentDesc &attachment : pass.color_attachments_) {
                TextureRecord *record = texture_record(attachment.texture);
                if (record == nullptr) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Render graph color attachment references an unknown texture.");
                }
                Core::RendererResult transition = transition_texture(encoder,
                                                                     attachment.texture,
                                                                     RHI::TextureLayout::ColorAttachment,
                                                                     RHI::PipelineStage::ColorAttachmentOutput,
                                                                     RHI::AccessFlags::ColorAttachmentWrite);
                if (!transition.has_value()) {
                    return transition;
                }
                color_attachments.push_back(RHI::ColorAttachment{
                    .view = attachment.view ? attachment.view : record->default_view,
                    .load_op = attachment.load_op,
                    .store_op = attachment.store_op,
                    .clear_color = attachment.clear_color,
                });
            }

            RHI::DepthStencilAttachment depth_stencil{};
            if (pass.has_depth_stencil_attachment_) {
                const RenderGraphDepthStencilAttachmentDesc &attachment = pass.depth_stencil_attachment_;
                TextureRecord *record = texture_record(attachment.texture);
                if (record == nullptr) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Render graph depth/stencil attachment references an unknown texture.");
                }
                Core::RendererResult transition = transition_texture(encoder,
                                                                     attachment.texture,
                                                                     RHI::TextureLayout::DepthStencilAttachment,
                                                                     RHI::PipelineStage::EarlyFragmentTests | RHI::PipelineStage::LateFragmentTests,
                                                                     RHI::AccessFlags::DepthStencilAttachmentRead | RHI::AccessFlags::DepthStencilAttachmentWrite);
                if (!transition.has_value()) {
                    return transition;
                }
                depth_stencil = RHI::DepthStencilAttachment{
                    .view = attachment.view ? attachment.view : record->default_view,
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
                .label = pass.label_.empty() ? nullptr : pass.label_.c_str(),
            };
            auto render_pass = encoder.begin_render_pass(pass_desc);
            if (!render_pass) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    string("begin render graph pass '") + pass.label_ + "' failed: " + render_pass.error().message);
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

[[nodiscard]] Core::RendererResult RenderGraph::execute_blit_pass(RHI::CommandEncoder &encoder, const RenderGraphBlitDesc &pass) {
            TextureRecord *source = texture_record(pass.source);
            TextureRecord *destination = texture_record(pass.destination);
            if (source == nullptr || destination == nullptr) {
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
                .src_max = RHI::Offset3D{static_cast<i32>(source->extent.width),
                                         static_cast<i32>(source->extent.height),
                                         static_cast<i32>(source->extent.depth_or_layers)},
                .dst_subresource = RHI::TextureSubresourceLayers{.mip_level = 0, .base_array_layer = 0, .array_layer_count = 1},
                .dst_min = RHI::Offset3D{0, 0, 0},
                .dst_max = RHI::Offset3D{static_cast<i32>(destination->extent.width),
                                         static_cast<i32>(destination->extent.height),
                                         static_cast<i32>(destination->extent.depth_or_layers)},
            };
            encoder.blit_texture(source->texture, destination->texture, blit, pass.filter);
            return {};
        }

[[nodiscard]] Core::RendererResult RenderGraph::create_transient_resources(RHI::RhiDevice &device) {
            for (TextureRecord &record : textures_) {
                if (!record.is_transient) {
                    continue;
                }

                auto texture_handle = device.create_texture(RHI::TextureDesc{
                    .dimension = RHI::TextureDimension::Dim2D,
                    .format = record.transient.format,
                    .extent = record.transient.extent,
                    .mip_levels = record.transient.mip_levels,
                    .samples = record.transient.samples,
                    .usage = record.transient.usage,
                    .label = record.label.empty() ? "render graph transient texture" : record.label.c_str(),
                });
                if (!texture_handle) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        string("create render graph transient texture '") + record.label + "' failed: " + texture_handle.error().message);
                }

                auto view_handle = device.create_texture_view(RHI::TextureViewDesc{
                    .texture = *texture_handle,
                    .view_type = RHI::TextureViewType::View2D,
                    .label = record.label.empty() ? "render graph transient texture view" : record.label.c_str(),
                });
                if (!view_handle) {
                    device.destroy_texture(*texture_handle);
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        string("create render graph transient texture view '") + record.label + "' failed: " + view_handle.error().message);
                }

                record.texture = *texture_handle;
                record.default_view = *view_handle;
            }
            return {};
        }

[[nodiscard]] Core::RendererResult RenderGraph::transition_to_final_states(RHI::CommandEncoder &encoder) {
            for (usize i = 0; i < textures_.size(); ++i) {
                TextureRecord &record = textures_[i];
                if (record.final_layout == RHI::TextureLayout::Undefined) {
                    continue;
                }
                if (record.current_layout == record.final_layout &&
                    record.current_stage == record.final_stage &&
                    record.current_access == record.final_access) {
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

RenderGraphContext::RenderGraphContext(RenderGraph &graph,
                                                  RHI::CommandEncoder &command_encoder,
                                                  RHI::RenderPassEncoder &render_pass) noexcept
        : graph_(&graph), command_encoder_(&command_encoder), render_pass_(&render_pass) {}

RHI::CommandEncoder &RenderGraphContext::command_encoder() const noexcept {
        return *command_encoder_;
    }

RHI::RenderPassEncoder &RenderGraphContext::render_pass() const noexcept {
        return *render_pass_;
    }

RenderGraphTextureAccess RenderGraphContext::texture(RenderGraphTextureHandle handle) const noexcept {
        return graph_ != nullptr ? graph_->texture_access(handle) : RenderGraphTextureAccess{};
    }

} // namespace SFT::Renderer
