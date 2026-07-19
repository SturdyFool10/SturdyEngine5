#include "RenderGraph.hpp"

#include <algorithm>
#include <set>
#include <utility>

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

[[nodiscard]] RenderGraph::PassUsage RenderGraph::pass_usage_of(const RenderGraphComputePassBuilder &pass) {
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
            // No declared storage write means whatever this pass dispatches has an untracked side
            // effect (e.g. writing only to a buffer) the graph can't reason about from textures alone
            // — mirrors RenderGraphRenderPassBuilder's own always_live rule above.
            usage.always_live = usage.writes.empty();
            return usage;
        }

[[nodiscard]] RenderGraph::PassUsage RenderGraph::pass_usage_of(const RenderGraphCopyDesc &pass) {
            PassUsage usage;
            usage.writes.push_back(pass.destination);
            usage.reads.push_back(pass.source);
            return usage;
        }

[[nodiscard]] RenderGraph::PassUsage RenderGraph::usage_of_ordered(const OrderedPass &ordered) const {
            switch (ordered.kind) {
                case PassKind::Render: return pass_usage_of(render_passes_[ordered.index]);
                case PassKind::Blit: return pass_usage_of(blit_passes_[ordered.index]);
                case PassKind::Compute: return pass_usage_of(compute_passes_[ordered.index]);
                case PassKind::Copy: return pass_usage_of(copy_passes_[ordered.index]);
            }
            return {};
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

RenderGraphComputePassBuilder::RenderGraphComputePassBuilder(string label) : label_(std::move(label)) {}

RenderGraphComputePassBuilder &RenderGraphComputePassBuilder::add_sampled_texture(RenderGraphTextureHandle texture) {
            sampled_texture_reads_.push_back(texture);
            return *this;
        }

RenderGraphComputePassBuilder &RenderGraphComputePassBuilder::add_storage_texture(const RenderGraphStorageTextureAccessDesc &access) {
            storage_textures_.push_back(access);
            return *this;
        }

RenderGraphComputePassBuilder &RenderGraphComputePassBuilder::set_execute(RenderGraphComputeExecuteFn execute) noexcept {
            execute_ = std::move(execute);
            return *this;
        }

[[nodiscard]] RenderGraphTextureHandle RenderGraph::import_texture(const RenderGraphImportedTextureDesc &desc) {
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
                .final_layout = desc.final_layout,
                .final_stage = desc.final_stage,
                .final_access = desc.final_access,
                .label = desc.label ? desc.label : "",
            });
            return handle;
        }

[[nodiscard]] RenderGraphTextureHandle RenderGraph::create_texture(const RenderGraphTextureDesc &desc) {
            // No physical_slot yet — aliasing assigns one per compiled-graph lifetime analysis inside
            // create_transient_resources(), which runs once execute() knows every pass in the frame.
            const RenderGraphTextureHandle handle{static_cast<u32>(textures_.size())};
            textures_.push_back(TextureRecord{
                .transient = desc,
                .is_transient = true,
                .physical_slot = ~0u,
                .format = desc.format,
                .extent = desc.extent,
                .mip_levels = std::max(desc.mip_levels, 1u),
                .final_layout = desc.final_layout,
                .final_stage = desc.final_stage,
                .final_access = desc.final_access,
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

[[nodiscard]] RenderGraphComputePassBuilder &RenderGraph::add_compute_pass(string_view label) {
            const u32 index = static_cast<u32>(compute_passes_.size());
            compute_passes_.emplace_back(string{label});
            ordered_passes_.push_back(OrderedPass{.kind = PassKind::Compute, .index = index});
            return compute_passes_.back();
        }

void RenderGraph::add_copy_pass(const RenderGraphCopyDesc &desc) {
            const u32 index = static_cast<u32>(copy_passes_.size());
            copy_passes_.push_back(desc);
            ordered_passes_.push_back(OrderedPass{.kind = PassKind::Copy, .index = index});
        }

[[nodiscard]] RenderGraphTextureAccess RenderGraph::texture_access(RenderGraphTextureHandle handle) const noexcept {
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

// Derives execution order from resource dependencies and culls dead passes — see the header's doc
// comment on compile() and the PassUsage helpers above for what's being derived from what.
// Algorithm, in three passes over the (small, per-frame) `ordered_passes_` list:
//
// 0. Validation: every handle any pass declared must resolve to a texture this graph actually
//    created/imported (UnknownTextureHandle otherwise), and every transient texture a pass reads
//    must already have an earlier producer — either an earlier pass's write, or this same pass also
//    writing it (the depth/stencil Load-op case below) — since an imported texture's entry content
//    is always valid but an uninitialized transient read is a genuine bug in the caller's graph
//    (MissingProducer). Both stop compilation before any GPU work happens.
// 1. Build a `depends_on[i]` edge list: pass i depends on the most recent earlier pass that wrote
//    a texture pass i reads (RAW), and on the most recent earlier pass that wrote a texture pass i
//    ALSO writes (WAW — keeps two writers of the same texture in their original relative order;
//    the topo-sort below is never free to swap them). Multiple writes to the same texture are
//    intentional in several existing passes (presentation + overlay, bloom mip updates) and are
//    never rejected — only an uninitialized *read* is a compile error.
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
[[nodiscard]] RenderGraph::CompileResult RenderGraph::compile() const {
            const usize pass_count = ordered_passes_.size();
            vector<PassUsage> usage(pass_count);
            for (usize i = 0; i < pass_count; ++i) {
                usage[i] = usage_of_ordered(ordered_passes_[i]);
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
            }

            vector<i64> last_writer(textures_.size(), -1);
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
                        return std::unexpected(RenderGraphCompileError{
                            .code = RenderGraphCompileErrorCode::MissingProducer,
                            .message = string("Render graph pass reads transient texture '") + record->label +
                                       "' before any earlier pass wrote it.",
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
            return CompiledPlan{.order = std::move(order)};
        }

[[nodiscard]] Core::RendererResult RenderGraph::execute(RHI::RhiDevice &device, RHI::CommandEncoder &encoder) {
            // Order first: aliasing needs the culled, topo-sorted order to compute accurate lifetimes,
            // and compile() never touches physical resource state, so this is safe to run before any
            // GPU texture exists yet.
            CompileResult compiled = compile();
            if (!compiled.has_value()) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    compiled.error().message);
            }
            const vector<OrderedPass> &execution_order = compiled->order;
            if (Core::RendererResult created = create_transient_resources(device, execution_order); !created.has_value()) {
                destroy_transient_resources(device);
                return created;
            }

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
                    case PassKind::Compute: {
                        RenderGraphComputePassBuilder &pass = compute_passes_[ordered.index];
                        result = with_debug_group(encoder, pass.label_, [&]() {
                            return execute_compute_pass(encoder, pass);
                        });
                        break;
                    }
                    case PassKind::Copy: {
                        const RenderGraphCopyDesc &pass = copy_passes_[ordered.index];
                        result = with_debug_group(encoder, pass.label ? pass.label : "render graph copy", [&]() {
                            return execute_copy_pass(encoder, pass);
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
            // Iterates physical_slots_, not textures_: aliasing means several virtual transient textures
            // can share one slot, so destroying per-virtual-texture would double-destroy.
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

void RenderGraph::take_transient_resources(vector<RHI::TextureHandle> &textures,
                                      vector<RHI::TextureViewHandle> &views) {
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

void RenderGraph::reset() noexcept {
            ordered_passes_.clear();
            render_passes_.clear();
            blit_passes_.clear();
            compute_passes_.clear();
            copy_passes_.clear();
            textures_.clear();
            physical_slots_.clear();
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

[[nodiscard]] RenderGraph::PhysicalSlot *RenderGraph::physical_slot_for(RenderGraphTextureHandle handle) noexcept {
            TextureRecord *record = texture_record(handle);
            if (record == nullptr || record->physical_slot >= physical_slots_.size()) {
                return nullptr;
            }
            return &physical_slots_[record->physical_slot];
        }

[[nodiscard]] const RenderGraph::PhysicalSlot *RenderGraph::physical_slot_for(RenderGraphTextureHandle handle) const noexcept {
            const TextureRecord *record = texture_record(handle);
            if (record == nullptr || record->physical_slot >= physical_slots_.size()) {
                return nullptr;
            }
            return &physical_slots_[record->physical_slot];
        }

[[nodiscard]] Core::RendererResult RenderGraph::transition_texture(RHI::CommandEncoder &encoder,
                                                              RenderGraphTextureHandle handle,
                                                              RHI::TextureLayout next_layout,
                                                              RHI::PipelineStage next_stage,
                                                              RHI::AccessFlags next_access,
                                                              RHI::TextureSubresourceRange subresources) {
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

[[nodiscard]] Core::RendererResult RenderGraph::execute_render_pass(RHI::CommandEncoder &encoder,
                                                               RenderGraphRenderPassBuilder &pass) {
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
                Core::RendererResult transition = transition_texture(encoder,
                                                                     attachment.texture,
                                                                     RHI::TextureLayout::ColorAttachment,
                                                                     RHI::PipelineStage::ColorAttachmentOutput,
                                                                     RHI::AccessFlags::ColorAttachmentWrite,
                                                                     attachment.subresources);
                if (!transition.has_value()) {
                    return transition;
                }
                color_attachments.push_back(RHI::ColorAttachment{
                    .view = attachment.view ? attachment.view : slot->default_view,
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
                depth_stencil = RHI::DepthStencilAttachment{
                    .view = attachment.view ? attachment.view : slot->default_view,
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

[[nodiscard]] Core::RendererResult RenderGraph::execute_compute_pass(RHI::CommandEncoder &encoder,
                                                                 RenderGraphComputePassBuilder &pass) {
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
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    string("begin render graph compute pass '") + pass.label_ + "' failed: " + compute_pass.error().message);
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

[[nodiscard]] Core::RendererResult RenderGraph::execute_copy_pass(RHI::CommandEncoder &encoder, const RenderGraphCopyDesc &pass) {
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

[[nodiscard]] vector<RenderGraph::TextureLifetime> RenderGraph::compute_transient_lifetimes(const vector<OrderedPass> &execution_order) const {
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

// Interval-graph aliasing: assigns each *virtual* transient texture (created via create_texture()) a
// PhysicalSlot, sharing one slot — and therefore one GPU allocation — across any number of virtual
// textures whose [first_use, last_use] ranges (in the compiled execution order) never overlap. Two
// virtual textures can only ever share a slot if their creation desc matches exactly (format/extent/
// mips/samples/usage) — the graph never reasons about reinterpreting a resource as a different shape.
// Assignment is greedy, sorted by first_use ascending (linear-scan register allocation): within a
// signature bucket, a texture reuses the first open slot whose current occupant already finished
// (last_use < this texture's first_use), or gets a brand new slot if none is free. A slot's
// per-mip state is only seeded from its *first* occupant's declared initial state — later occupants
// inherit whatever state the previous occupant actually left the physical resource in,
// which is correct (and cheaper than resetting) since it's literally the same VkImage, not a separate
// aliased allocation requiring its own hazard tracking.
[[nodiscard]] Core::RendererResult RenderGraph::create_transient_resources(RHI::RhiDevice &device,
                                                                      const vector<OrderedPass> &execution_order) {
            const vector<TextureLifetime> lifetimes = compute_transient_lifetimes(execution_order);

            struct PendingSlot {
                RenderGraphTextureDesc desc;
                string label;
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
                    // create_texture() was called but no live pass ever reads or writes it (dead code,
                    // or a texture the caller created and simply never used) — leave physical_slot at
                    // its default ~0u and skip straight to the next texture. No physical GPU resource
                    // is allocated for it; nothing can legitimately dereference it later since nothing
                    // live ever declared a use.
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
                vector<std::pair<u32, i32>> open_slots; // (physical_slots_ index, current occupant's last_use)
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
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        string("create render graph transient texture '") + pending_slot.label + "' failed: " + texture_handle.error().message);
                }

                auto view_handle = device.create_texture_view(RHI::TextureViewDesc{
                    .texture = *texture_handle,
                    .view_type = RHI::TextureViewType::View2D,
                    .label = pending_slot.label.empty() ? "render graph transient texture view" : pending_slot.label.c_str(),
                });
                if (!view_handle) {
                    device.destroy_texture(*texture_handle);
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        string("create render graph transient texture view '") + pending_slot.label + "' failed: " + view_handle.error().message);
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

[[nodiscard]] Core::RendererResult RenderGraph::transition_to_final_states(RHI::CommandEncoder &encoder) {
            for (usize i = 0; i < textures_.size(); ++i) {
                TextureRecord &record = textures_[i];
                if (record.final_layout == RHI::TextureLayout::Undefined) {
                    continue;
                }
                // A transient texture create_transient_resources() never allocated (dead: no live pass
                // used it) has no physical_slot to transition — nothing to do, not an error.
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

RenderGraphComputeContext::RenderGraphComputeContext(RenderGraph &graph,
                                                  RHI::CommandEncoder &command_encoder,
                                                  RHI::ComputePassEncoder &compute_pass) noexcept
        : graph_(&graph), command_encoder_(&command_encoder), compute_pass_(&compute_pass) {}

RHI::CommandEncoder &RenderGraphComputeContext::command_encoder() const noexcept {
        return *command_encoder_;
    }

RHI::ComputePassEncoder &RenderGraphComputeContext::compute_pass() const noexcept {
        return *compute_pass_;
    }

RenderGraphTextureAccess RenderGraphComputeContext::texture(RenderGraphTextureHandle handle) const noexcept {
        return graph_ != nullptr ? graph_->texture_access(handle) : RenderGraphTextureAccess{};
    }

} // namespace SFT::Renderer
