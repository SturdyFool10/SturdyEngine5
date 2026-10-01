#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <expected>
#include <span>
#include <vector>

#include <Renderer/AutoExposure.hpp>
#include <Renderer/ScreenSpaceGi.hpp>
#include <Renderer/TemporalUpscaler.hpp>
#include <Renderer/CameraEmulation.hpp>
#include <Renderer/AntiAliasing.hpp>
#include <Renderer/Bloom.hpp>
#include <Renderer/FramePipeline.hpp>
#include <Core/Slang/ShaderLibrary.hpp>
#include <Renderer/MotionBlur.hpp>
#include <Renderer/Overlay.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/ToneMapping.hpp>

#include <tracy/Tracy.hpp>

using std::array;
using std::span;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {
        constexpr usize kParallelRecordThreshold = 128;

        /// The visibility rule of a pass that draws everything it is given.
        const ItemVisibilityFn &accept_all_items() {
            static const ItemVisibilityFn rule = [](const RenderItem &, const ItemCuller &) { return true; };
            return rule;
        }
    } // namespace

    // The engine's own frame features. They are registered in the same FramePipeline, under the same
    // kind of name, as anything an application adds -- see FramePipeline.hpp for how to replace one.

    Core::RendererResult Renderer::record_draw_items(RHI::RenderPassEncoder &pass,
                                                     const DrawItemPass &desc,
                                                     FrameBuildContext &frame) {
        const FrameSubmission &submission = *static_cast<BuiltinFrameState *>(frame.builtin)->submission;
        const span<const RenderItem> items = desc.set == DrawItemSet::Custom  ? desc.items
                                             : desc.set == DrawItemSet::Gizmos ? span<const RenderItem>{submission.gizmo_draws}
                                                                               : span<const RenderItem>{submission.draws};
        pass.set_viewport(desc.viewport);
        pass.set_scissor(desc.scissor);
        const ItemCuller default_culler =
            desc.culler != nullptr ? ItemCuller{} : ItemCuller::from_view_projection(desc.view_projection);
        const ItemCuller pass_culler = desc.frustum_cull ? (desc.culler != nullptr ? *desc.culler : default_culler)
                                                          : ItemCuller{.custom = &accept_all_items()};
        usize visible = 0;
        for (const RenderItem &item : items) {
            if (pass_culler(item)) {
                ++visible;
            }
        }
        const bool use_bundles = desc.allow_bundles && frame.transient_render_bundles != nullptr &&
                                 frame.device.is_enabled(RHI::Feature::RenderBundles) && visible >= kParallelRecordThreshold &&
                                 Async::Scheduler::worker_count() > 1;
        vector<RHI::RenderBundleHandle> unused_bundles;
        return record_render_items_culled(
            pass, items, pass_culler, desc.color_formats, desc.depth_format, desc.frame_index, desc.view_projection,
            desc.depth_only, desc.standard_depth_test, desc.label, use_bundles,
            frame.transient_render_bundles != nullptr ? *frame.transient_render_bundles : unused_bundles, false, 0.0f,
            0.0f, desc.samples, false, RHI::BindGroupHandle{}, desc.viewport, desc.scissor, desc.camera_lens);
    }

    /// Feature `instance_culling` (Scene stage): GPU frustum + HiZ culling of instanced batches. Consumes HiZPyramid. Publishes InstanceIndirectCommands and CompactedInstanceIndices (buffers).
    Core::RendererResult Renderer::build_frame_feature_instance_culling(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::instance_culling");
        BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        [[maybe_unused]] FrameSubmission &submission = *state.submission;
        [[maybe_unused]] WindowSurfaceRecord &record = *state.record;
        [[maybe_unused]] FrameInFlight &slot = *state.slot;
        [[maybe_unused]] SceneFrameState &scene = *state.scene;
        [[maybe_unused]] RenderGraph &graph = context.graph;
        [[maybe_unused]] RenderGraphBlackboard &graph_resources = context.resources;
        [[maybe_unused]] RHI::RhiDevice *device = &context.device;
        [[maybe_unused]] const Core::Extent2D render_extent = context.module.render_extent;
        [[maybe_unused]] const u64 frame_index = context.frame_index;
        [[maybe_unused]] const auto &instanced_batches = *scene.instanced_batches;
        [[maybe_unused]] SceneFrameGpuResources &instance_cull_resources = *scene.instance_cull_resources;
        [[maybe_unused]] HiZCullInput &hiz_cull_input = *scene.hiz_cull_input;
        [[maybe_unused]] const PreparedShadowFrame &shadow_frame = *scene.shadow_frame;
        [[maybe_unused]] const ItemCuller &culler = scene.culler;
        [[maybe_unused]] const bool multisampled = scene.multisampled;
        [[maybe_unused]] const RHI::BindGroupHandle object_history_group = scene.object_history_group;
        [[maybe_unused]] const std::span<const RenderItem> gbuffer_draws = scene.gbuffer_draws;
        [[maybe_unused]] const RenderGraphTextureHandle depth_texture = graph_resources.texture<RenderGraphSemantics::ResolvedSceneDepth>();
        [[maybe_unused]] const RenderGraphTextureHandle hiz_pyramid_texture = graph_resources.texture<RenderGraphSemantics::HiZPyramid>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_albedo = graph_resources.texture<RenderGraphSemantics::GBufferAlbedo>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_normal = graph_resources.texture<RenderGraphSemantics::GBufferNormal>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_material = graph_resources.texture<RenderGraphSemantics::GBufferMaterial>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_emissive = graph_resources.texture<RenderGraphSemantics::GBufferEmissive>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_motion = graph_resources.texture<RenderGraphSemantics::GBufferMotion>();
        [[maybe_unused]] const RenderGraphTextureHandle directional_shadow_atlas = graph_resources.texture<RenderGraphSemantics::DirectionalShadowAtlas>();
        [[maybe_unused]] const RenderGraphTextureHandle shadow_atlas = graph_resources.texture<RenderGraphSemantics::PunctualShadowAtlas>();
        RenderGraphBufferHandle instance_indirect_commands{};
        RenderGraphBufferHandle compacted_instance_indices{};
        if (!instanced_batches.empty()) {
            instance_indirect_commands = graph.import_buffer(RenderGraphImportedBufferDesc{
                .buffer = instance_cull_resources.indirect_commands_buffer,
                .size = instance_cull_resources.indirect_commands_capacity * sizeof(GpuDrawIndexedIndirectCommand),
                .initial_stage = RHI::PipelineStage::DrawIndirect,
                .initial_access = RHI::AccessFlags::IndirectCommandRead,
                .label = "GPU-culling indirect commands",
            });
            compacted_instance_indices = graph.import_buffer(RenderGraphImportedBufferDesc{
                .buffer = instance_cull_resources.compacted_indices_buffer,
                .size = instance_cull_resources.compacted_indices_capacity * sizeof(u32),
                .initial_stage = RHI::PipelineStage::VertexShader,
                .initial_access = RHI::AccessFlags::ShaderRead,
                .label = "GPU-culling compacted instance indices",
            });
            graph.add_compute_pass("gpu instance cull"_ustr)
                .add_sampled_texture(hiz_pyramid_texture)
                .add_buffer(RenderGraphBufferAccessDesc{
                    .buffer = instance_indirect_commands,
                    .stages = RHI::PipelineStage::ComputeShader,
                    .access = RHI::AccessFlags::ShaderWrite,
                    .read = false,
                    .write = true,
                })
                .add_buffer(RenderGraphBufferAccessDesc{
                    .buffer = compacted_instance_indices,
                    .stages = RHI::PipelineStage::ComputeShader,
                    .access = RHI::AccessFlags::ShaderWrite,
                    .read = false,
                    .write = true,
                })
                .set_execute([this, &submission, &instanced_batches, &instance_cull_resources, &hiz_cull_input](
                                 RenderGraphComputeContext &context) -> Core::RendererResult {
                    return record_instance_cull(
                        context.compute_pass(), instanced_batches, submission.view_projection,
                        submission.camera.world_position, hiz_cull_input, instance_cull_resources,
                        submission.transient_bind_groups);
                });
            graph_resources.publish_buffer<RenderGraphSemantics::InstanceIndirectCommands>(instance_indirect_commands);
            graph_resources.publish_buffer<RenderGraphSemantics::CompactedInstanceIndices>(compacted_instance_indices);
        }

        return {};
    }

    /// Feature `shadow_maps` (Scene stage): renders the directional-cascade and punctual shadow atlases. Publishes DirectionalShadowAtlas and PunctualShadowAtlas (imported by the frame; this feature fills them).
    Core::RendererResult Renderer::build_frame_feature_shadow_maps(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::shadow_maps");
        BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        [[maybe_unused]] FrameSubmission &submission = *state.submission;
        [[maybe_unused]] WindowSurfaceRecord &record = *state.record;
        [[maybe_unused]] FrameInFlight &slot = *state.slot;
        [[maybe_unused]] SceneFrameState &scene = *state.scene;
        [[maybe_unused]] RenderGraph &graph = context.graph;
        [[maybe_unused]] RenderGraphBlackboard &graph_resources = context.resources;
        [[maybe_unused]] RHI::RhiDevice *device = &context.device;
        [[maybe_unused]] const Core::Extent2D render_extent = context.module.render_extent;
        [[maybe_unused]] const u64 frame_index = context.frame_index;
        [[maybe_unused]] const auto &instanced_batches = *scene.instanced_batches;
        [[maybe_unused]] SceneFrameGpuResources &instance_cull_resources = *scene.instance_cull_resources;
        [[maybe_unused]] HiZCullInput &hiz_cull_input = *scene.hiz_cull_input;
        [[maybe_unused]] const PreparedShadowFrame &shadow_frame = *scene.shadow_frame;
        [[maybe_unused]] const ItemCuller &culler = scene.culler;
        [[maybe_unused]] const bool multisampled = scene.multisampled;
        [[maybe_unused]] const RHI::BindGroupHandle object_history_group = scene.object_history_group;
        [[maybe_unused]] const std::span<const RenderItem> gbuffer_draws = scene.gbuffer_draws;
        [[maybe_unused]] const RenderGraphTextureHandle depth_texture = graph_resources.texture<RenderGraphSemantics::ResolvedSceneDepth>();
        [[maybe_unused]] const RenderGraphTextureHandle hiz_pyramid_texture = graph_resources.texture<RenderGraphSemantics::HiZPyramid>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_albedo = graph_resources.texture<RenderGraphSemantics::GBufferAlbedo>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_normal = graph_resources.texture<RenderGraphSemantics::GBufferNormal>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_material = graph_resources.texture<RenderGraphSemantics::GBufferMaterial>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_emissive = graph_resources.texture<RenderGraphSemantics::GBufferEmissive>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_motion = graph_resources.texture<RenderGraphSemantics::GBufferMotion>();
        [[maybe_unused]] const RenderGraphTextureHandle directional_shadow_atlas = graph_resources.texture<RenderGraphSemantics::DirectionalShadowAtlas>();
        [[maybe_unused]] const RenderGraphTextureHandle shadow_atlas = graph_resources.texture<RenderGraphSemantics::PunctualShadowAtlas>();
        if (!submission.render_graph.render_scene || scene.full_path_tracing) {
            return {};
        }
        // Directional cascades and punctual shadows render into separate atlases, so the pass
        // body is shared rather than duplicated. Both are depth-only, use identical state, and
        // parallelize the same way; only the target, the view list and the extent differ.
        const auto add_shadow_atlas_pass =
            [&](auto label, RenderGraphTextureHandle atlas,
                const vector<ShadowRenderView> &view_storage, u32 width, u32 height) {
                const bool uses_bundles = device->is_enabled(RHI::Feature::RenderBundles) &&
                                          view_storage.size() >= 2 &&
                                          Async::Scheduler::worker_count() > 1;
                graph.add_render_pass(label)
                    .set_depth_stencil_attachment(RenderGraphDepthStencilAttachmentDesc{
                        .texture = atlas,
                        .depth_load_op = RHI::LoadOp::Clear,
                        .depth_store_op = RHI::StoreOp::Store,
                        .clear_value = RHI::ClearDepthStencil{.depth = 1.0f, .stencil = 0},
                    })
                    .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = width, .height = height})
                    .set_allow_bundles(uses_bundles)
                    .set_execute([this, &submission, &view_storage, &slot, frame_index, uses_bundles](
                                     RenderGraphContext &context) -> Core::RendererResult {
                        RHI::RenderPassEncoder &pass = context.render_pass();
                        const f32 shadow_depth_bias = std::isfinite(submission.render_graph.shadow_depth_bias)
                                                          ? std::max(submission.render_graph.shadow_depth_bias, 0.0f)
                                                          : 0.75f;
                        const f32 shadow_slope_bias = std::isfinite(submission.render_graph.shadow_slope_bias)
                                                          ? std::max(submission.render_graph.shadow_slope_bias, 0.0f)
                                                          : 1.0f;
                        const span<const ShadowRenderView> views{view_storage.data(), view_storage.size()};
                        const RHI::Format depth_format = slot.shadow_targets.format;
                        const u32 worker_count = Async::Scheduler::worker_count();

                        if (!uses_bundles) {
                            return record_shadow_view_chunk(pass, views, submission.draws, depth_format,
                                                            frame_index, shadow_depth_bias,
                                                            shadow_slope_bias);
                        }

                        RHI::RhiDevice *device = rhi_device();
                        if (device == nullptr) {
                            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                                "Cannot record shadow atlas without an RHI device.");
                        }

                        const usize chunk_count = std::min<usize>(worker_count, views.size());
                        const usize chunk_size = (views.size() + chunk_count - 1) / chunk_count;

                        struct ShadowChunkResult {
                            Core::RendererResult status{};
                            RHI::RenderBundleHandle bundle{};
                            unique_ptr<RHI::RenderBundleEncoder> encoder;
                        };
                        vector<ShadowChunkResult> results(chunk_count);

                        for (usize chunk = 0; chunk < chunk_count; ++chunk) {
                            const usize begin = chunk * chunk_size;
                            const usize end = std::min(views.size(), begin + chunk_size);
                            if (begin >= end) {
                                continue;
                            }
                            const RHI::RenderBundleDesc bundle_desc{
                                .color_formats = {},
                                .depth_stencil_format = depth_format,
                                .samples = RHI::SampleCount::X1,
                                .view_mask = 0,
                                .label = "shadow view chunk",
                            };
                            auto encoder = device->create_render_bundle_encoder(bundle_desc);
                            if (!encoder) {
                                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                                    "Cannot create shadow bundle encoder.");
                            }
                            results[chunk].encoder = std::move(*encoder);
                        }

                        vector<Async::TaskHandle<void>> tasks;
                        tasks.reserve(chunk_count);
                        for (usize chunk = 0; chunk < chunk_count; ++chunk) {
                            const usize begin = chunk * chunk_size;
                            const usize end = std::min(views.size(), begin + chunk_size);
                            if (begin >= end) {
                                continue;
                            }
                            tasks.push_back(Async::Scheduler::spawn([this, &submission, &results, chunk, views,
                                                                      begin, end, depth_format,
                                                                      frame_index = frame_index,
                                                                      shadow_depth_bias, shadow_slope_bias]() {
                                RHI::RenderBundleEncoder &encoder = *results[chunk].encoder;
                                Core::RendererResult recorded = record_shadow_view_chunk(
                                    encoder, views.subspan(begin, end - begin), submission.draws, depth_format,
                                    frame_index, shadow_depth_bias, shadow_slope_bias);
                                if (!recorded.has_value()) {
                                    results[chunk].status = recorded;
                                    return;
                                }
                                auto finished = encoder.finish();
                                if (!finished) {
                                    results[chunk].status =
                                        unexpected(graphics_error_from_rhi(finished.error(), "finish shadow bundle"));
                                    return;
                                }
                                results[chunk].bundle = *finished;
                            }));
                        }
                        for (const Async::TaskHandle<void> &task : tasks) {
                            task.wait();
                        }

                        vector<RHI::RenderBundleHandle> bundles;
                        bundles.reserve(chunk_count);
                        Core::RendererResult first_error{};
                        bool has_error = false;
                        for (ShadowChunkResult &result : results) {
                            if (!result.status.has_value() && !has_error) {
                                first_error = result.status;
                                has_error = true;
                            }
                            if (result.bundle) {
                                bundles.push_back(result.bundle);
                            }
                        }
                        if (!bundles.empty()) {
                            pass.execute_bundles(span<const RHI::RenderBundleHandle>{bundles.data(), bundles.size()});
                        }

                        submission.transient_render_bundles.insert(submission.transient_render_bundles.end(),
                                                                    bundles.begin(), bundles.end());
                        if (has_error) {
                            return first_error;
                        }
                        return {};
                    });
            };

        if (shadow_frame.directional_atlas_used) {
            add_shadow_atlas_pass("directional shadow cascades"_ustr, directional_shadow_atlas,
                                  shadow_frame.directional_views,
                                  slot.shadow_targets.directional_layout.width,
                                  slot.shadow_targets.directional_layout.height);
        }
        if (shadow_frame.atlas_used) {
            add_shadow_atlas_pass("raster shadow atlas"_ustr, shadow_atlas,
                                  shadow_frame.punctual_views, slot.shadow_targets.atlas_size,
                                  slot.shadow_targets.atlas_size);
        }


        return {};
    }

    /// Feature `gbuffer` (Scene stage): the deferred base pass. Clears and fills the five G-buffer targets (albedo, normal, material, emissive, motion) and depth from the scene draws (plus GPU-culled instanced batches). Consumes GBuffer* and ResolvedSceneDepth (and InstanceIndirectCommands/CompactedInstanceIndices if present); a replacement writes the same GBuffer* textures.
    Core::RendererResult Renderer::build_frame_feature_gbuffer(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::gbuffer");
        BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        [[maybe_unused]] FrameSubmission &submission = *state.submission;
        [[maybe_unused]] WindowSurfaceRecord &record = *state.record;
        [[maybe_unused]] FrameInFlight &slot = *state.slot;
        [[maybe_unused]] SceneFrameState &scene = *state.scene;
        [[maybe_unused]] RenderGraph &graph = context.graph;
        [[maybe_unused]] RenderGraphBlackboard &graph_resources = context.resources;
        [[maybe_unused]] RHI::RhiDevice *device = &context.device;
        [[maybe_unused]] const Core::Extent2D render_extent = context.module.render_extent;
        [[maybe_unused]] const u64 frame_index = context.frame_index;
        [[maybe_unused]] const auto &instanced_batches = *scene.instanced_batches;
        [[maybe_unused]] SceneFrameGpuResources &instance_cull_resources = *scene.instance_cull_resources;
        [[maybe_unused]] HiZCullInput &hiz_cull_input = *scene.hiz_cull_input;
        [[maybe_unused]] const PreparedShadowFrame &shadow_frame = *scene.shadow_frame;
        [[maybe_unused]] const ItemCuller &culler = scene.culler;
        [[maybe_unused]] const bool multisampled = scene.multisampled;
        [[maybe_unused]] const RHI::BindGroupHandle object_history_group = scene.object_history_group;
        [[maybe_unused]] const std::span<const RenderItem> gbuffer_draws = scene.gbuffer_draws;
        [[maybe_unused]] const RenderGraphTextureHandle depth_texture = graph_resources.texture<RenderGraphSemantics::ResolvedSceneDepth>();
        [[maybe_unused]] const RenderGraphTextureHandle hiz_pyramid_texture = graph_resources.texture<RenderGraphSemantics::HiZPyramid>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_albedo = graph_resources.texture<RenderGraphSemantics::GBufferAlbedo>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_normal = graph_resources.texture<RenderGraphSemantics::GBufferNormal>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_material = graph_resources.texture<RenderGraphSemantics::GBufferMaterial>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_emissive = graph_resources.texture<RenderGraphSemantics::GBufferEmissive>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_motion = graph_resources.texture<RenderGraphSemantics::GBufferMotion>();
        [[maybe_unused]] const RenderGraphTextureHandle directional_shadow_atlas = graph_resources.texture<RenderGraphSemantics::DirectionalShadowAtlas>();
        [[maybe_unused]] const RenderGraphTextureHandle shadow_atlas = graph_resources.texture<RenderGraphSemantics::PunctualShadowAtlas>();
        if (!submission.render_graph.render_scene || scene.full_path_tracing) {
            return {};
        }
        const RenderGraphBufferHandle instance_indirect_commands = graph_resources.buffer<RenderGraphSemantics::InstanceIndirectCommands>();
        const RenderGraphBufferHandle compacted_instance_indices = graph_resources.buffer<RenderGraphSemantics::CompactedInstanceIndices>();
        usize gbuffer_visible_count = 0;
        for (const RenderItem &item : gbuffer_draws) {
            if (culler(item)) {
                ++gbuffer_visible_count;
            }
        }
        const bool gbuffer_uses_bundles =
            device->is_enabled(RHI::Feature::RenderBundles) &&
            gbuffer_visible_count >= kParallelRecordThreshold && Async::Scheduler::worker_count() > 1;
        RenderGraphRenderPassBuilder &gbuffer_pass = graph.add_render_pass("deferred gbuffer geometry"_ustr);
        gbuffer_pass.add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = gbuffer_albedo,
                .load_op = RHI::LoadOp::Clear,
                .store_op = RHI::StoreOp::Store,
                .clear_color = RHI::ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
            })
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = gbuffer_normal,
                .load_op = RHI::LoadOp::Clear,
                .store_op = RHI::StoreOp::Store,
                .clear_color = RHI::ClearColor{0.5f, 0.5f, 0.0f, 0.0f},
            })
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = gbuffer_material,
                .load_op = RHI::LoadOp::Clear,
                .store_op = RHI::StoreOp::Store,
                .clear_color = RHI::ClearColor{0.0f, 0.0f, 0.0f, 0.0f},
            })
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = gbuffer_emissive,
                .load_op = RHI::LoadOp::Clear,
                .store_op = RHI::StoreOp::Store,
                .clear_color = RHI::ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
            })
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = gbuffer_motion,
                .load_op = RHI::LoadOp::Clear,
                .store_op = RHI::StoreOp::Store,
                .clear_color = RHI::ClearColor{0.0f, 0.0f, 0.0f, 0.0f},
            })
            .set_depth_stencil_attachment(RenderGraphDepthStencilAttachmentDesc{
                .texture = depth_texture,


                .depth_load_op = multisampled ? RHI::LoadOp::Clear : RHI::LoadOp::Load,
                .depth_store_op = RHI::StoreOp::Store,
                .clear_value = RHI::ClearDepthStencil{.depth = 1.0f, .stencil = 0},
            })
            .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.x, .height = render_extent.y})
            .set_allow_bundles(gbuffer_uses_bundles);
        if (!instanced_batches.empty()) {
            gbuffer_pass
                .add_buffer(RenderGraphBufferAccessDesc{
                    .buffer = instance_indirect_commands,
                    .stages = RHI::PipelineStage::DrawIndirect,
                    .access = RHI::AccessFlags::IndirectCommandRead,
                })
                .add_buffer(RenderGraphBufferAccessDesc{
                    .buffer = compacted_instance_indices,
                    .stages = RHI::PipelineStage::VertexShader,
                    .access = RHI::AccessFlags::ShaderRead,
                });
        }
        gbuffer_pass.set_execute([this, &submission, render_extent, frame_index, culler, gbuffer_draws, &instanced_batches,
                         &instance_cull_resources, multisampled, object_history_group,
                         gbuffer_uses_bundles](RenderGraphContext &context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .x = 0.0f, .y = 0.0f,
                    .width = static_cast<f32>(render_extent.x),
                    .height = static_cast<f32>(render_extent.y),
                    .min_depth = 0.0f, .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.x, .height = render_extent.y});
                const array<RHI::Format, 5> gbuffer_formats{
                    submission.deferred_formats.albedo,
                    submission.deferred_formats.normal,
                    submission.deferred_formats.material,
                    submission.deferred_formats.emissive,
                    submission.deferred_formats.motion,
                };
                const span<const RHI::Format> gbuffer_formats_span{gbuffer_formats.data(), gbuffer_formats.size()};
                if (Core::RendererResult recorded = record_render_items_culled(
                        pass, gbuffer_draws, culler, gbuffer_formats_span, submission.deferred_formats.depth,
                        frame_index, submission.view_projection,                false,
                                                multisampled, "deferred gbuffer geometry",
                        gbuffer_uses_bundles, submission.transient_render_bundles,
                                       false, 0.0f, 0.0f, RHI::SampleCount::X1,
                                                true, object_history_group,
                        RHI::Viewport{.x = 0.0f, .y = 0.0f,
                                      .width = static_cast<f32>(render_extent.x),
                                      .height = static_cast<f32>(render_extent.y),
                                      .min_depth = 0.0f, .max_depth = 1.0f},
                        RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.x, .height = render_extent.y},
                        submission.render_graph.camera_emulation.lens_strength);
                    !recorded.has_value()) {
                    return recorded;
                }
                if (!instanced_batches.empty()) {
                    if (Core::RendererResult recorded_instanced = record_instanced_batches(
                            pass, instanced_batches, gbuffer_formats_span, submission.deferred_formats.depth,
                            frame_index, submission.view_projection, submission.camera.previous_view_projection,
                            instance_cull_resources, submission.transient_bind_groups, RHI::SampleCount::X1);
                        !recorded_instanced.has_value()) {
                        return recorded_instanced;
                    }
                }
                return {};
            });


        return {};
    }

    /// Feature `hiz_build` (Scene stage): builds the hierarchical depth pyramid from ResolvedSceneDepth for next frame's occlusion culling. Publishes HiZPyramid (imported by the frame).
    Core::RendererResult Renderer::build_frame_feature_hiz_build(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::hiz_build");
        BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        [[maybe_unused]] FrameSubmission &submission = *state.submission;
        [[maybe_unused]] WindowSurfaceRecord &record = *state.record;
        [[maybe_unused]] FrameInFlight &slot = *state.slot;
        [[maybe_unused]] SceneFrameState &scene = *state.scene;
        [[maybe_unused]] RenderGraph &graph = context.graph;
        [[maybe_unused]] RenderGraphBlackboard &graph_resources = context.resources;
        [[maybe_unused]] RHI::RhiDevice *device = &context.device;
        [[maybe_unused]] const Core::Extent2D render_extent = context.module.render_extent;
        [[maybe_unused]] const u64 frame_index = context.frame_index;
        [[maybe_unused]] const auto &instanced_batches = *scene.instanced_batches;
        [[maybe_unused]] SceneFrameGpuResources &instance_cull_resources = *scene.instance_cull_resources;
        [[maybe_unused]] HiZCullInput &hiz_cull_input = *scene.hiz_cull_input;
        [[maybe_unused]] const PreparedShadowFrame &shadow_frame = *scene.shadow_frame;
        [[maybe_unused]] const ItemCuller &culler = scene.culler;
        [[maybe_unused]] const bool multisampled = scene.multisampled;
        [[maybe_unused]] const RHI::BindGroupHandle object_history_group = scene.object_history_group;
        [[maybe_unused]] const std::span<const RenderItem> gbuffer_draws = scene.gbuffer_draws;
        [[maybe_unused]] const RenderGraphTextureHandle depth_texture = graph_resources.texture<RenderGraphSemantics::ResolvedSceneDepth>();
        [[maybe_unused]] const RenderGraphTextureHandle hiz_pyramid_texture = graph_resources.texture<RenderGraphSemantics::HiZPyramid>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_albedo = graph_resources.texture<RenderGraphSemantics::GBufferAlbedo>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_normal = graph_resources.texture<RenderGraphSemantics::GBufferNormal>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_material = graph_resources.texture<RenderGraphSemantics::GBufferMaterial>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_emissive = graph_resources.texture<RenderGraphSemantics::GBufferEmissive>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_motion = graph_resources.texture<RenderGraphSemantics::GBufferMotion>();
        [[maybe_unused]] const RenderGraphTextureHandle directional_shadow_atlas = graph_resources.texture<RenderGraphSemantics::DirectionalShadowAtlas>();
        [[maybe_unused]] const RenderGraphTextureHandle shadow_atlas = graph_resources.texture<RenderGraphSemantics::PunctualShadowAtlas>();
        if (!submission.render_graph.render_scene || scene.full_path_tracing) {
            return {};
        }
        if (Core::RendererResult hiz_built = record_hiz_build(
                graph, depth_texture, slot.deferred_targets.depth_view, render_extent,
                hiz_pyramid_texture, record.hiz_pyramid, submission.transient_bind_groups);
            !hiz_built.has_value()) {
            return hiz_built;
        }
        return {};
    }

    /// Feature `atmosphere_luts` (Scene stage): bakes the atmosphere transmittance, multiple-scattering and sky-view LUTs. Publishes TransmittanceLut, MultiScatteringLut, SkyViewLut.
    Core::RendererResult Renderer::build_frame_feature_atmosphere_luts(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::atmosphere_luts");
        BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        [[maybe_unused]] FrameSubmission &submission = *state.submission;
        [[maybe_unused]] WindowSurfaceRecord &record = *state.record;
        [[maybe_unused]] FrameInFlight &slot = *state.slot;
        [[maybe_unused]] SceneFrameState &scene = *state.scene;
        [[maybe_unused]] RenderGraph &graph = context.graph;
        [[maybe_unused]] RenderGraphBlackboard &graph_resources = context.resources;
        [[maybe_unused]] RenderGraphModuleBuildContext &module_context = context.module;
        [[maybe_unused]] RHI::RhiDevice *device = &context.device;
        [[maybe_unused]] const Core::Extent2D render_extent = context.module.render_extent;
        [[maybe_unused]] const u64 frame_index = context.frame_index;
        [[maybe_unused]] const bool full_path_tracing = scene.full_path_tracing;
        [[maybe_unused]] const bool multisampled = scene.multisampled;
        [[maybe_unused]] const RHI::SampleCount framebuffer_samples = state.framebuffer_samples;
        [[maybe_unused]] const PreparedShadowFrame &shadow_frame = *scene.shadow_frame;
        [[maybe_unused]] const RenderGraphTextureHandle scene_color = graph_resources.texture<RenderGraphSemantics::SceneHdrColor>();
        [[maybe_unused]] const RenderGraphTextureHandle depth_texture = graph_resources.texture<RenderGraphSemantics::ResolvedSceneDepth>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_albedo = graph_resources.texture<RenderGraphSemantics::GBufferAlbedo>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_normal = graph_resources.texture<RenderGraphSemantics::GBufferNormal>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_material = graph_resources.texture<RenderGraphSemantics::GBufferMaterial>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_emissive = graph_resources.texture<RenderGraphSemantics::GBufferEmissive>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_motion = graph_resources.texture<RenderGraphSemantics::GBufferMotion>();
        [[maybe_unused]] const RenderGraphTextureHandle directional_shadow_atlas = graph_resources.texture<RenderGraphSemantics::DirectionalShadowAtlas>();
        [[maybe_unused]] const RenderGraphTextureHandle shadow_atlas = graph_resources.texture<RenderGraphSemantics::PunctualShadowAtlas>();
        [[maybe_unused]] const RenderGraphTextureHandle transmittance_lut = graph_resources.texture<RenderGraphSemantics::TransmittanceLut>();
        [[maybe_unused]] const RenderGraphTextureHandle multi_scattering_lut = graph_resources.texture<RenderGraphSemantics::MultiScatteringLut>();
        [[maybe_unused]] const RenderGraphTextureHandle sky_view_lut = graph_resources.texture<RenderGraphSemantics::SkyViewLut>();
        RenderGraphTextureHandle transmittance_lut_out{};
        RenderGraphTextureHandle multi_scattering_lut_out{};
        RenderGraphTextureHandle sky_view_lut_out{};
        if (submission.render_graph.render_scene) {
            if (Core::RendererResult atmosphere_luts = record_atmosphere_lut_bakes(
                    graph, slot.atmosphere_targets.constants_buffer, transmittance_lut_out, multi_scattering_lut_out,
                    sky_view_lut_out, submission.transient_bind_groups);
                !atmosphere_luts.has_value()) {
                return atmosphere_luts;
            }
            graph_resources.publish_texture<RenderGraphSemantics::TransmittanceLut>(transmittance_lut_out);
            graph_resources.publish_texture<RenderGraphSemantics::MultiScatteringLut>(multi_scattering_lut_out);
            graph_resources.publish_texture<RenderGraphSemantics::SkyViewLut>(sky_view_lut_out);
        }

        return {};
    }

    /// Feature `spectral_path_tracing` (Scene stage): the hybrid spectral ray-query pass, photon emission/hash and the full spectral path tracer (with its depth commit), when the frame asks for them. Consumes GBuffer*, ResolvedSceneDepth and the atmosphere LUTs; writes SceneHdrColor in full-path mode.
    Core::RendererResult Renderer::build_frame_feature_spectral_path_tracing(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::spectral_path_tracing");
        BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        [[maybe_unused]] FrameSubmission &submission = *state.submission;
        [[maybe_unused]] WindowSurfaceRecord &record = *state.record;
        [[maybe_unused]] FrameInFlight &slot = *state.slot;
        [[maybe_unused]] SceneFrameState &scene = *state.scene;
        [[maybe_unused]] RenderGraph &graph = context.graph;
        [[maybe_unused]] RenderGraphBlackboard &graph_resources = context.resources;
        [[maybe_unused]] RenderGraphModuleBuildContext &module_context = context.module;
        [[maybe_unused]] RHI::RhiDevice *device = &context.device;
        [[maybe_unused]] const Core::Extent2D render_extent = context.module.render_extent;
        [[maybe_unused]] const u64 frame_index = context.frame_index;
        [[maybe_unused]] const bool full_path_tracing = scene.full_path_tracing;
        [[maybe_unused]] const bool multisampled = scene.multisampled;
        [[maybe_unused]] const RHI::SampleCount framebuffer_samples = state.framebuffer_samples;
        [[maybe_unused]] const PreparedShadowFrame &shadow_frame = *scene.shadow_frame;
        [[maybe_unused]] const RenderGraphTextureHandle scene_color = graph_resources.texture<RenderGraphSemantics::SceneHdrColor>();
        [[maybe_unused]] const RenderGraphTextureHandle depth_texture = graph_resources.texture<RenderGraphSemantics::ResolvedSceneDepth>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_albedo = graph_resources.texture<RenderGraphSemantics::GBufferAlbedo>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_normal = graph_resources.texture<RenderGraphSemantics::GBufferNormal>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_material = graph_resources.texture<RenderGraphSemantics::GBufferMaterial>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_emissive = graph_resources.texture<RenderGraphSemantics::GBufferEmissive>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_motion = graph_resources.texture<RenderGraphSemantics::GBufferMotion>();
        [[maybe_unused]] const RenderGraphTextureHandle directional_shadow_atlas = graph_resources.texture<RenderGraphSemantics::DirectionalShadowAtlas>();
        [[maybe_unused]] const RenderGraphTextureHandle shadow_atlas = graph_resources.texture<RenderGraphSemantics::PunctualShadowAtlas>();
        [[maybe_unused]] const RenderGraphTextureHandle transmittance_lut = graph_resources.texture<RenderGraphSemantics::TransmittanceLut>();
        [[maybe_unused]] const RenderGraphTextureHandle multi_scattering_lut = graph_resources.texture<RenderGraphSemantics::MultiScatteringLut>();
        [[maybe_unused]] const RenderGraphTextureHandle sky_view_lut = graph_resources.texture<RenderGraphSemantics::SkyViewLut>();
        [[maybe_unused]] const RenderGraphTextureHandle spectral_effect = scene.spectral_effect;
        [[maybe_unused]] const RenderGraphTextureHandle spectral_primary_depth = scene.spectral_primary_depth;
        [[maybe_unused]] const RenderGraphTextureHandle spectral_accumulation = scene.spectral_accumulation;
        [[maybe_unused]] const RenderGraphBufferHandle spectral_photons = scene.spectral_photons;
        [[maybe_unused]] const RenderGraphBufferHandle spectral_photon_count = scene.spectral_photon_count;
        [[maybe_unused]] const RenderGraphBufferHandle spectral_photon_hash_heads = scene.spectral_photon_hash_heads;
        [[maybe_unused]] const bool spectral_photon_emission_needed = scene.spectral_photon_emission_needed;
        [[maybe_unused]] const bool spectral_photon_mapping = scene.spectral_photon_mapping;
        [[maybe_unused]] const bool spectral_accumulation_reset = scene.spectral_accumulation_reset;
        [[maybe_unused]] const glm::vec4 background = state.background;
        const SpectralRenderMode spectral_mode = submission.render_graph.spectral_path_tracing.mode;
        const bool hybrid_spectral = submission.render_graph.render_scene &&
                                     spectral_mode != SpectralRenderMode::RasterDeferred &&
                                     spectral_mode != SpectralRenderMode::FullPathTracing;
        if (hybrid_spectral) {
            graph.add_compute_pass("hybrid spectral ray query"_ustr)
                .add_sampled_texture(gbuffer_albedo)
                .add_sampled_texture(gbuffer_normal)
                .add_sampled_texture(gbuffer_material)
                .add_sampled_texture(gbuffer_emissive)
                .add_sampled_texture(gbuffer_motion)
                .add_sampled_texture(depth_texture)
                .add_sampled_texture(transmittance_lut)
                .add_sampled_texture(sky_view_lut)
                .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = spectral_effect})
                .set_execute([this, &submission, &slot, render_extent, gbuffer_albedo, gbuffer_normal,
                              gbuffer_material, gbuffer_emissive, gbuffer_motion, depth_texture,
                              transmittance_lut, sky_view_lut,
                              spectral_effect](RenderGraphComputeContext &context) -> Core::RendererResult {
                    const RHI::TextureViewHandle output = context.texture(spectral_effect).default_view;
                    return record_spectral_integrator(
                        context.compute_pass(), slot, submission, render_extent,
                        SpectralIntegratorViews{
                            .raster_albedo = context.texture(gbuffer_albedo).default_view,
                            .raster_normal = context.texture(gbuffer_normal).default_view,
                            .raster_material = context.texture(gbuffer_material).default_view,
                            .raster_emissive = context.texture(gbuffer_emissive).default_view,
                            .raster_motion = context.texture(gbuffer_motion).default_view,
                            .raster_depth = context.texture(depth_texture).default_view,
                            .effect_output = output,
                            .scene_color_output = output,
                            .gbuffer_motion_output = output,
                            .primary_depth_output = output,
                            .accumulation_output = output,
                            .transmittance_lut = context.texture(transmittance_lut).default_view,
                            .sky_view_lut = context.texture(sky_view_lut).default_view,
                            .atmosphere_constants = slot.atmosphere_targets.constants_buffer,
                        }, false);
                });
        }

        if (spectral_photon_emission_needed) {
            graph.add_compute_pass("spectral photon emission"_ustr)
                .add_buffer(RenderGraphBufferAccessDesc{
                    .buffer = spectral_photons,
                    .stages = RHI::PipelineStage::ComputeShader,
                    .access = RHI::AccessFlags::ShaderWrite,
                    .read = false,
                    .write = true,
                })
                .add_buffer(RenderGraphBufferAccessDesc{
                    .buffer = spectral_photon_count,
                    .stages = RHI::PipelineStage::ComputeShader,
                    .access = RHI::AccessFlags::ShaderRead | RHI::AccessFlags::ShaderWrite,
                    .read = true,
                    .write = true,
                })
                .set_execute([this, &slot, &submission](
                                 RenderGraphComputeContext &context) -> Core::RendererResult {
                    return record_spectral_photon_emission(context.compute_pass(), slot, submission);
                });

            graph.add_compute_pass("spectral photon spatial hash"_ustr)
                .add_buffer(RenderGraphBufferAccessDesc{
                    .buffer = spectral_photons,
                    .stages = RHI::PipelineStage::ComputeShader,
                    .access = RHI::AccessFlags::ShaderRead | RHI::AccessFlags::ShaderWrite,
                    .read = true,
                    .write = true,
                })
                .add_buffer(RenderGraphBufferAccessDesc{
                    .buffer = spectral_photon_count,
                    .stages = RHI::PipelineStage::ComputeShader,
                    .access = RHI::AccessFlags::ShaderRead,
                    .read = true,
                    .write = false,
                })
                .add_buffer(RenderGraphBufferAccessDesc{
                    .buffer = spectral_photon_hash_heads,
                    .stages = RHI::PipelineStage::ComputeShader,
                    .access = RHI::AccessFlags::ShaderRead | RHI::AccessFlags::ShaderWrite,
                    .read = true,
                    .write = true,
                })
                .set_execute([this, &slot, &submission](
                                 RenderGraphComputeContext &context) -> Core::RendererResult {
                    return record_spectral_photon_hash(context.compute_pass(), slot, submission);
                });
        }

        if (submission.render_graph.render_scene && full_path_tracing) {
            RenderGraphComputePassBuilder &full_path_pass = graph.add_compute_pass("full spectral path tracing"_ustr);
            full_path_pass
                .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = spectral_effect})
                .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = scene_color})
                .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = gbuffer_motion})
                .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = spectral_primary_depth})
                .add_storage_texture(RenderGraphStorageTextureAccessDesc{
                    .texture = spectral_accumulation,
                    .read = true,
                    .write = true,
                })
                .add_sampled_texture(transmittance_lut)
                .add_sampled_texture(sky_view_lut);
            if (spectral_photon_mapping) {
                full_path_pass
                    .add_buffer(RenderGraphBufferAccessDesc{
                        .buffer = spectral_photons,
                        .stages = RHI::PipelineStage::ComputeShader,
                        .access = RHI::AccessFlags::ShaderRead,
                        .read = true,
                        .write = false,
                    })
                    .add_buffer(RenderGraphBufferAccessDesc{
                        .buffer = spectral_photon_count,
                        .stages = RHI::PipelineStage::ComputeShader,
                        .access = RHI::AccessFlags::ShaderRead,
                        .read = true,
                        .write = false,
                    })
                    .add_buffer(RenderGraphBufferAccessDesc{
                        .buffer = spectral_photon_hash_heads,
                        .stages = RHI::PipelineStage::ComputeShader,
                        .access = RHI::AccessFlags::ShaderRead,
                        .read = true,
                        .write = false,
                    });
            }
            full_path_pass.set_execute([this, &submission, &slot, render_extent, spectral_effect, scene_color,
                              gbuffer_motion, spectral_primary_depth, spectral_accumulation,
                              transmittance_lut, sky_view_lut,
                              spectral_accumulation_reset](
                                 RenderGraphComputeContext &context) -> Core::RendererResult {
                    const RHI::TextureViewHandle dummy = context.texture(spectral_effect).default_view;
                    return record_spectral_integrator(
                        context.compute_pass(), slot, submission, render_extent,
                        SpectralIntegratorViews{
                            .raster_albedo = dummy,
                            .raster_normal = dummy,
                            .raster_material = dummy,
                            .raster_emissive = dummy,
                            .raster_motion = dummy,
                            .raster_depth = dummy,
                            .effect_output = dummy,
                            .scene_color_output = context.texture(scene_color).default_view,
                            .gbuffer_motion_output = context.texture(gbuffer_motion).default_view,
                            .primary_depth_output = context.texture(spectral_primary_depth).default_view,
                            .accumulation_output = context.texture(spectral_accumulation).default_view,
                            .transmittance_lut = context.texture(transmittance_lut).default_view,
                            .sky_view_lut = context.texture(sky_view_lut).default_view,
                            .atmosphere_constants = slot.atmosphere_targets.constants_buffer,
                        }, spectral_accumulation_reset);
                });

            graph.add_render_pass("path traced depth commit"_ustr)
                .set_depth_stencil_attachment(RenderGraphDepthStencilAttachmentDesc{
                    .texture = depth_texture,
                    .depth_load_op = RHI::LoadOp::DontCare,
                    .depth_store_op = RHI::StoreOp::Store,
                })
                .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = spectral_primary_depth})
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0,
                                             .width = render_extent.x, .height = render_extent.y})
                .set_execute([this, &slot, spectral_primary_depth, render_extent](
                                 RenderGraphContext &context) -> Core::RendererResult {
                    return record_spectral_depth_commit(
                        context.render_pass(), slot,
                        context.texture(spectral_primary_depth).default_view, render_extent);
                });
        }

        return {};
    }

    /// Feature `ambient_occlusion` (Scene stage): XeGTAO ambient occlusion. Consumes GBufferNormal and ResolvedSceneDepth; publishes AmbientOcclusion.
    Core::RendererResult Renderer::build_frame_feature_ambient_occlusion(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::ambient_occlusion");
        BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        [[maybe_unused]] FrameSubmission &submission = *state.submission;
        [[maybe_unused]] WindowSurfaceRecord &record = *state.record;
        [[maybe_unused]] FrameInFlight &slot = *state.slot;
        [[maybe_unused]] SceneFrameState &scene = *state.scene;
        [[maybe_unused]] RenderGraph &graph = context.graph;
        [[maybe_unused]] RenderGraphBlackboard &graph_resources = context.resources;
        [[maybe_unused]] RenderGraphModuleBuildContext &module_context = context.module;
        [[maybe_unused]] RHI::RhiDevice *device = &context.device;
        [[maybe_unused]] const Core::Extent2D render_extent = context.module.render_extent;
        [[maybe_unused]] const u64 frame_index = context.frame_index;
        [[maybe_unused]] const bool full_path_tracing = scene.full_path_tracing;
        [[maybe_unused]] const bool multisampled = scene.multisampled;
        [[maybe_unused]] const RHI::SampleCount framebuffer_samples = state.framebuffer_samples;
        [[maybe_unused]] const PreparedShadowFrame &shadow_frame = *scene.shadow_frame;
        [[maybe_unused]] const RenderGraphTextureHandle scene_color = graph_resources.texture<RenderGraphSemantics::SceneHdrColor>();
        [[maybe_unused]] const RenderGraphTextureHandle depth_texture = graph_resources.texture<RenderGraphSemantics::ResolvedSceneDepth>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_albedo = graph_resources.texture<RenderGraphSemantics::GBufferAlbedo>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_normal = graph_resources.texture<RenderGraphSemantics::GBufferNormal>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_material = graph_resources.texture<RenderGraphSemantics::GBufferMaterial>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_emissive = graph_resources.texture<RenderGraphSemantics::GBufferEmissive>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_motion = graph_resources.texture<RenderGraphSemantics::GBufferMotion>();
        [[maybe_unused]] const RenderGraphTextureHandle directional_shadow_atlas = graph_resources.texture<RenderGraphSemantics::DirectionalShadowAtlas>();
        [[maybe_unused]] const RenderGraphTextureHandle shadow_atlas = graph_resources.texture<RenderGraphSemantics::PunctualShadowAtlas>();
        [[maybe_unused]] const RenderGraphTextureHandle transmittance_lut = graph_resources.texture<RenderGraphSemantics::TransmittanceLut>();
        [[maybe_unused]] const RenderGraphTextureHandle multi_scattering_lut = graph_resources.texture<RenderGraphSemantics::MultiScatteringLut>();
        [[maybe_unused]] const RenderGraphTextureHandle sky_view_lut = graph_resources.texture<RenderGraphSemantics::SkyViewLut>();
        // Screen-space ambient occlusion runs after the G-buffer is complete and before deferred
        // lighting consumes it. Full path tracing computes its own occlusion along the transport
        // path, so the screen-space approximation is skipped entirely there rather than layered on
        // top of a result that already accounts for it.
        RenderGraphTextureHandle gtao_ambient_occlusion{};
        if (submission.render_graph.render_scene && !full_path_tracing) {
            auto gtao_texture = build_gtao_module(module_context, submission, slot,
                                                  gbuffer_normal, depth_texture);
            if (!gtao_texture.has_value()) {
                return unexpected(gtao_texture.error());
            }
            gtao_ambient_occlusion = *gtao_texture;
            graph_resources.publish_texture<RenderGraphSemantics::AmbientOcclusion>(gtao_ambient_occlusion);
        }
        return {};
    }

    /// Feature `global_illumination` (Scene stage): ReSTIR GI surfel irradiance. Consumes GBuffer*, ResolvedSceneDepth, TransmittanceLut and SkyViewLut; publishes SurfelIrradiance.
    Core::RendererResult Renderer::build_frame_feature_global_illumination(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::global_illumination");
        BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        [[maybe_unused]] FrameSubmission &submission = *state.submission;
        [[maybe_unused]] WindowSurfaceRecord &record = *state.record;
        [[maybe_unused]] FrameInFlight &slot = *state.slot;
        [[maybe_unused]] SceneFrameState &scene = *state.scene;
        [[maybe_unused]] RenderGraph &graph = context.graph;
        [[maybe_unused]] RenderGraphBlackboard &graph_resources = context.resources;
        [[maybe_unused]] RenderGraphModuleBuildContext &module_context = context.module;
        [[maybe_unused]] RHI::RhiDevice *device = &context.device;
        [[maybe_unused]] const Core::Extent2D render_extent = context.module.render_extent;
        [[maybe_unused]] const u64 frame_index = context.frame_index;
        [[maybe_unused]] const bool full_path_tracing = scene.full_path_tracing;
        [[maybe_unused]] const bool multisampled = scene.multisampled;
        [[maybe_unused]] const RHI::SampleCount framebuffer_samples = state.framebuffer_samples;
        [[maybe_unused]] const PreparedShadowFrame &shadow_frame = *scene.shadow_frame;
        [[maybe_unused]] const RenderGraphTextureHandle scene_color = graph_resources.texture<RenderGraphSemantics::SceneHdrColor>();
        [[maybe_unused]] const RenderGraphTextureHandle depth_texture = graph_resources.texture<RenderGraphSemantics::ResolvedSceneDepth>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_albedo = graph_resources.texture<RenderGraphSemantics::GBufferAlbedo>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_normal = graph_resources.texture<RenderGraphSemantics::GBufferNormal>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_material = graph_resources.texture<RenderGraphSemantics::GBufferMaterial>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_emissive = graph_resources.texture<RenderGraphSemantics::GBufferEmissive>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_motion = graph_resources.texture<RenderGraphSemantics::GBufferMotion>();
        [[maybe_unused]] const RenderGraphTextureHandle directional_shadow_atlas = graph_resources.texture<RenderGraphSemantics::DirectionalShadowAtlas>();
        [[maybe_unused]] const RenderGraphTextureHandle shadow_atlas = graph_resources.texture<RenderGraphSemantics::PunctualShadowAtlas>();
        [[maybe_unused]] const RenderGraphTextureHandle transmittance_lut = graph_resources.texture<RenderGraphSemantics::TransmittanceLut>();
        [[maybe_unused]] const RenderGraphTextureHandle multi_scattering_lut = graph_resources.texture<RenderGraphSemantics::MultiScatteringLut>();
        [[maybe_unused]] const RenderGraphTextureHandle sky_view_lut = graph_resources.texture<RenderGraphSemantics::SkyViewLut>();
        RenderGraphTextureHandle surfel_irradiance{};
        if (submission.render_graph.render_scene && !full_path_tracing) {
            auto restir_gi_texture = build_restir_gi_module(
                module_context, submission, slot, gbuffer_normal, gbuffer_albedo, gbuffer_material,
                gbuffer_emissive, gbuffer_motion, depth_texture, transmittance_lut, sky_view_lut);
            if (!restir_gi_texture.has_value()) {
                return unexpected(restir_gi_texture.error());
            }
            surfel_irradiance = *restir_gi_texture;
            graph_resources.publish_texture<RenderGraphSemantics::SurfelIrradiance>(surfel_irradiance);
        }
        return {};
    }

    /// Feature `lighting` (Scene stage): deferred shadow lighting into SceneHdrColor. Consumes GBuffer*, ResolvedSceneDepth, both shadow atlases, the atmosphere LUTs, AmbientOcclusion and SurfelIrradiance.
    Core::RendererResult Renderer::build_frame_feature_lighting(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::lighting");
        BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        [[maybe_unused]] FrameSubmission &submission = *state.submission;
        [[maybe_unused]] WindowSurfaceRecord &record = *state.record;
        [[maybe_unused]] FrameInFlight &slot = *state.slot;
        [[maybe_unused]] SceneFrameState &scene = *state.scene;
        [[maybe_unused]] RenderGraph &graph = context.graph;
        [[maybe_unused]] RenderGraphBlackboard &graph_resources = context.resources;
        [[maybe_unused]] RenderGraphModuleBuildContext &module_context = context.module;
        [[maybe_unused]] RHI::RhiDevice *device = &context.device;
        [[maybe_unused]] const Core::Extent2D render_extent = context.module.render_extent;
        [[maybe_unused]] const u64 frame_index = context.frame_index;
        [[maybe_unused]] const bool full_path_tracing = scene.full_path_tracing;
        [[maybe_unused]] const bool multisampled = scene.multisampled;
        [[maybe_unused]] const RHI::SampleCount framebuffer_samples = state.framebuffer_samples;
        [[maybe_unused]] const PreparedShadowFrame &shadow_frame = *scene.shadow_frame;
        [[maybe_unused]] const RenderGraphTextureHandle scene_color = graph_resources.texture<RenderGraphSemantics::SceneHdrColor>();
        [[maybe_unused]] const RenderGraphTextureHandle depth_texture = graph_resources.texture<RenderGraphSemantics::ResolvedSceneDepth>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_albedo = graph_resources.texture<RenderGraphSemantics::GBufferAlbedo>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_normal = graph_resources.texture<RenderGraphSemantics::GBufferNormal>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_material = graph_resources.texture<RenderGraphSemantics::GBufferMaterial>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_emissive = graph_resources.texture<RenderGraphSemantics::GBufferEmissive>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_motion = graph_resources.texture<RenderGraphSemantics::GBufferMotion>();
        [[maybe_unused]] const RenderGraphTextureHandle directional_shadow_atlas = graph_resources.texture<RenderGraphSemantics::DirectionalShadowAtlas>();
        [[maybe_unused]] const RenderGraphTextureHandle shadow_atlas = graph_resources.texture<RenderGraphSemantics::PunctualShadowAtlas>();
        [[maybe_unused]] const RenderGraphTextureHandle transmittance_lut = graph_resources.texture<RenderGraphSemantics::TransmittanceLut>();
        [[maybe_unused]] const RenderGraphTextureHandle multi_scattering_lut = graph_resources.texture<RenderGraphSemantics::MultiScatteringLut>();
        [[maybe_unused]] const RenderGraphTextureHandle sky_view_lut = graph_resources.texture<RenderGraphSemantics::SkyViewLut>();
        [[maybe_unused]] const bool hybrid_spectral = scene.hybrid_spectral;
        [[maybe_unused]] const RenderGraphTextureHandle spectral_effect = scene.spectral_effect;
        [[maybe_unused]] const RenderGraphTextureHandle gtao_ambient_occlusion = graph_resources.texture<RenderGraphSemantics::AmbientOcclusion>();
        [[maybe_unused]] const RenderGraphTextureHandle surfel_irradiance = graph_resources.texture<RenderGraphSemantics::SurfelIrradiance>();
        if (submission.render_graph.render_scene && !full_path_tracing) {
            const RenderGraphTextureHandle lighting_spectral_effect = hybrid_spectral
                ? spectral_effect : gbuffer_emissive;
            RenderGraphRenderPassBuilder &lighting_pass = graph.add_render_pass("deferred shadow lighting"_ustr);
            lighting_pass.add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = scene_color,
                .load_op = RHI::LoadOp::DontCare,
                .store_op = RHI::StoreOp::Store,
            });
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = gbuffer_albedo});
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = gbuffer_normal});
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = gbuffer_material});
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = gbuffer_emissive});
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = depth_texture});
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = lighting_spectral_effect});
            if (shadow_frame.atlas_used) {
                lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = shadow_atlas});
            }
            if (shadow_frame.directional_atlas_used) {
                lighting_pass.add_sampled_texture(
                    RenderGraphSampledTextureReadDesc{.texture = directional_shadow_atlas});
            }
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = transmittance_lut});
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = multi_scattering_lut});
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = sky_view_lut});
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = surfel_irradiance});
            lighting_pass.add_sampled_texture(
                RenderGraphSampledTextureReadDesc{.texture = gtao_ambient_occlusion});
            lighting_pass
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.x, .height = render_extent.y})
                .set_execute([this, &submission, &slot, render_extent, gbuffer_albedo, gbuffer_normal,
                              gbuffer_material, gbuffer_emissive, depth_texture, lighting_spectral_effect,
                              shadow_atlas, directional_shadow_atlas, &shadow_frame, surfel_irradiance,
                              gtao_ambient_occlusion, transmittance_lut, multi_scattering_lut, sky_view_lut](
                                 RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .x = 0.0f, .y = 0.0f,
                        .width = static_cast<f32>(render_extent.x),
                        .height = static_cast<f32>(render_extent.y),
                        .min_depth = 0.0f, .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0,
                                                 .width = render_extent.x, .height = render_extent.y});
                    // The resolve always binds both atlas slots; when one is unused it aliases the
                    // scene depth view, which the shader never reads because no view indexes it.
                    const RHI::TextureViewHandle atlas_view = shadow_frame.atlas_used
                        ? context.texture(shadow_atlas).default_view
                        : context.texture(depth_texture).default_view;
                    const RHI::TextureViewHandle directional_atlas_view =
                        shadow_frame.directional_atlas_used
                            ? context.texture(directional_shadow_atlas).default_view
                            : context.texture(depth_texture).default_view;
                    return record_shadow_lighting(
                        pass,
                        context.texture(gbuffer_albedo).default_view,
                        context.texture(gbuffer_normal).default_view,
                        context.texture(gbuffer_material).default_view,
                        context.texture(gbuffer_emissive).default_view,
                        context.texture(depth_texture).default_view,
                        context.texture(lighting_spectral_effect).default_view,
                        atlas_view,
                        directional_atlas_view,
                        slot.shadow_targets.lighting_buffer,
                        context.texture(transmittance_lut).default_view,
                        context.texture(multi_scattering_lut).default_view,
                        context.texture(sky_view_lut).default_view,
                        context.texture(surfel_irradiance).default_view,
                        context.texture(gtao_ambient_occlusion).default_view,
                        slot.atmosphere_targets.constants_buffer,
                        submission.deferred_formats.scene_color,
                        submission.transient_bind_groups);
                });
        }

        return {};
    }

    /// Feature `restir_history_copy` (Scene stage): copies the finished scene colour into ReSTIR GI's history for next frame. Consumes SceneHdrColor.
    Core::RendererResult Renderer::build_frame_feature_restir_history_copy(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::restir_history_copy");
        BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        [[maybe_unused]] FrameSubmission &submission = *state.submission;
        [[maybe_unused]] WindowSurfaceRecord &record = *state.record;
        [[maybe_unused]] FrameInFlight &slot = *state.slot;
        [[maybe_unused]] SceneFrameState &scene = *state.scene;
        [[maybe_unused]] RenderGraph &graph = context.graph;
        [[maybe_unused]] RenderGraphBlackboard &graph_resources = context.resources;
        [[maybe_unused]] RenderGraphModuleBuildContext &module_context = context.module;
        [[maybe_unused]] RHI::RhiDevice *device = &context.device;
        [[maybe_unused]] const Core::Extent2D render_extent = context.module.render_extent;
        [[maybe_unused]] const u64 frame_index = context.frame_index;
        [[maybe_unused]] const bool full_path_tracing = scene.full_path_tracing;
        [[maybe_unused]] const bool multisampled = scene.multisampled;
        [[maybe_unused]] const RHI::SampleCount framebuffer_samples = state.framebuffer_samples;
        [[maybe_unused]] const PreparedShadowFrame &shadow_frame = *scene.shadow_frame;
        [[maybe_unused]] const RenderGraphTextureHandle scene_color = graph_resources.texture<RenderGraphSemantics::SceneHdrColor>();
        [[maybe_unused]] const RenderGraphTextureHandle depth_texture = graph_resources.texture<RenderGraphSemantics::ResolvedSceneDepth>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_albedo = graph_resources.texture<RenderGraphSemantics::GBufferAlbedo>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_normal = graph_resources.texture<RenderGraphSemantics::GBufferNormal>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_material = graph_resources.texture<RenderGraphSemantics::GBufferMaterial>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_emissive = graph_resources.texture<RenderGraphSemantics::GBufferEmissive>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_motion = graph_resources.texture<RenderGraphSemantics::GBufferMotion>();
        [[maybe_unused]] const RenderGraphTextureHandle directional_shadow_atlas = graph_resources.texture<RenderGraphSemantics::DirectionalShadowAtlas>();
        [[maybe_unused]] const RenderGraphTextureHandle shadow_atlas = graph_resources.texture<RenderGraphSemantics::PunctualShadowAtlas>();
        [[maybe_unused]] const RenderGraphTextureHandle transmittance_lut = graph_resources.texture<RenderGraphSemantics::TransmittanceLut>();
        [[maybe_unused]] const RenderGraphTextureHandle multi_scattering_lut = graph_resources.texture<RenderGraphSemantics::MultiScatteringLut>();
        [[maybe_unused]] const RenderGraphTextureHandle sky_view_lut = graph_resources.texture<RenderGraphSemantics::SkyViewLut>();
        // Copies this frame's just-finished final scene color into ReSTIR GI's history texture, read
        // back next frame by restir_gi_initial_sample.slang for multi-bounce feedback. Must run after
        // the lighting pass above has written `scene_color` and is gated identically to
        // build_restir_gi_module so the history texture only exists/updates while ReSTIR GI is enabled.
        if (submission.render_graph.render_scene && !full_path_tracing && submission.render_graph.restir_gi.enabled) {
            graph.add_compute_pass("restir gi history copy"_ustr)
                .add_sampled_texture(scene_color)
                .set_side_effect(true)
                .set_execute([this, &submission, scene_color](
                                 RenderGraphComputeContext &graph_context) -> Core::RendererResult {
                    return record_restir_gi_history_copy(
                        graph_context.compute_pass(),
                        graph_context.texture(scene_color).default_view,
                        submission.transient_bind_groups);
                });
        }

        return {};
    }

    /// Feature `msaa_resolve` (Scene stage): reconstructs the multisampled deferred result into SceneHdrColor when MSAA is on.
    Core::RendererResult Renderer::build_frame_feature_msaa_resolve(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::msaa_resolve");
        BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        [[maybe_unused]] FrameSubmission &submission = *state.submission;
        [[maybe_unused]] WindowSurfaceRecord &record = *state.record;
        [[maybe_unused]] FrameInFlight &slot = *state.slot;
        [[maybe_unused]] SceneFrameState &scene = *state.scene;
        [[maybe_unused]] RenderGraph &graph = context.graph;
        [[maybe_unused]] RenderGraphBlackboard &graph_resources = context.resources;
        [[maybe_unused]] RenderGraphModuleBuildContext &module_context = context.module;
        [[maybe_unused]] RHI::RhiDevice *device = &context.device;
        [[maybe_unused]] const Core::Extent2D render_extent = context.module.render_extent;
        [[maybe_unused]] const u64 frame_index = context.frame_index;
        [[maybe_unused]] const bool full_path_tracing = scene.full_path_tracing;
        [[maybe_unused]] const bool multisampled = scene.multisampled;
        [[maybe_unused]] const RHI::SampleCount framebuffer_samples = state.framebuffer_samples;
        [[maybe_unused]] const PreparedShadowFrame &shadow_frame = *scene.shadow_frame;
        [[maybe_unused]] const RenderGraphTextureHandle scene_color = graph_resources.texture<RenderGraphSemantics::SceneHdrColor>();
        [[maybe_unused]] const RenderGraphTextureHandle depth_texture = graph_resources.texture<RenderGraphSemantics::ResolvedSceneDepth>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_albedo = graph_resources.texture<RenderGraphSemantics::GBufferAlbedo>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_normal = graph_resources.texture<RenderGraphSemantics::GBufferNormal>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_material = graph_resources.texture<RenderGraphSemantics::GBufferMaterial>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_emissive = graph_resources.texture<RenderGraphSemantics::GBufferEmissive>();
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_motion = graph_resources.texture<RenderGraphSemantics::GBufferMotion>();
        [[maybe_unused]] const RenderGraphTextureHandle directional_shadow_atlas = graph_resources.texture<RenderGraphSemantics::DirectionalShadowAtlas>();
        [[maybe_unused]] const RenderGraphTextureHandle shadow_atlas = graph_resources.texture<RenderGraphSemantics::PunctualShadowAtlas>();
        [[maybe_unused]] const RenderGraphTextureHandle transmittance_lut = graph_resources.texture<RenderGraphSemantics::TransmittanceLut>();
        [[maybe_unused]] const RenderGraphTextureHandle multi_scattering_lut = graph_resources.texture<RenderGraphSemantics::MultiScatteringLut>();
        [[maybe_unused]] const RenderGraphTextureHandle sky_view_lut = graph_resources.texture<RenderGraphSemantics::SkyViewLut>();
        if (submission.render_graph.render_scene && multisampled) {
            if (Core::RendererResult reconstructed = build_deferred_msaa_module(
                    module_context, submission, framebuffer_samples);
                !reconstructed.has_value()) {
                return reconstructed;
            }
        }

        return {};
    }

    /// Feature `z_prepass` (Scene stage): depth-only pass over the scene draws into RasterVisibilityDepth, cleared
    /// to far. The G-buffer pass then tests "equal" against it. Consumes RasterVisibilityDepth.
    Core::RendererResult Renderer::build_frame_feature_z_prepass(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::z_prepass");
        BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        FrameSubmission &submission = *state.submission;
        if (!submission.render_graph.render_scene || state.scene->full_path_tracing) {
            return {};
        }
        const RenderGraphTextureHandle raster_depth = context.resources.texture<RenderGraphSemantics::RasterVisibilityDepth>();
        const Core::Extent2D extent = context.module.render_extent;
        const u64 frame_index = context.frame_index;
        const RHI::SampleCount samples = state.framebuffer_samples;
        usize visible = 0;
        const ItemCuller &culler = state.scene->culler;
        const ItemCuller *culler_ptr = &culler;
        for (const RenderItem &item : submission.draws) {
            if (culler(item)) {
                ++visible;
            }
        }
        const bool uses_bundles = context.device.is_enabled(RHI::Feature::RenderBundles) && visible >= kParallelRecordThreshold &&
                                  Async::Scheduler::worker_count() > 1;
        context.graph.add_render_pass("z prepass"_ustr)
            .set_depth_stencil_attachment(RenderGraphDepthStencilAttachmentDesc{
                .texture = raster_depth,
                .depth_load_op = RHI::LoadOp::Clear,
                .depth_store_op = RHI::StoreOp::Store,
                .clear_value = RHI::ClearDepthStencil{.depth = 1.0f, .stencil = 0},
            })
            .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = extent.x, .height = extent.y})
            .set_allow_bundles(uses_bundles)
            .set_execute([this, &context, &submission, extent, frame_index, samples, culler_ptr](
                             RenderGraphContext &pass_context) -> Core::RendererResult {
                return record_draw_items(
                    pass_context.render_pass(),
                    DrawItemPass{
                        .set = DrawItemSet::SceneDraws,
                        .depth_format = submission.deferred_formats.depth,
                        .samples = samples,
                        .frame_index = frame_index,
                        .view_projection = submission.view_projection,
                        .camera_lens = submission.render_graph.camera_emulation.lens_strength,
                        .depth_only = true,
                        .culler = culler_ptr,
                        .label = "z prepass",
                        .viewport = RHI::Viewport{.x = 0.0f, .y = 0.0f, .width = static_cast<f32>(extent.x),
                                                  .height = static_cast<f32>(extent.y), .min_depth = 0.0f,
                                                  .max_depth = 1.0f},
                        .scissor = RHI::Rect2D{.x = 0, .y = 0, .width = extent.x, .height = extent.y},
                    },
                    context);
            });
        return {};
    }

    /// Feature `scene_background`: with no scene to draw, clears SceneHdrColor to the background colour.
    /// Publishes nothing; consumes SceneHdrColor (published by the scene half).
    Core::RendererResult Renderer::build_frame_feature_scene_background(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::scene_background");
        const BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        if (context.settings.render_scene || context.direct_overlay_presentation) {
            return {};
        }
        const RenderGraphTextureHandle scene_color = context.resources.texture<RenderGraphSemantics::SceneHdrColor>();
        const Core::Extent2D extent = context.module.render_extent;
        context.graph.add_render_pass("scene background"_ustr)
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = scene_color,
                .load_op = RHI::LoadOp::Clear,
                .store_op = RHI::StoreOp::Store,
                .clear_color = RHI::ClearColor{state.background.r, state.background.g, state.background.b, state.background.a},
            })
            .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = extent.x, .height = extent.y});
        return {};
    }

    /// Feature `light_indicators`: draws the frame's gizmo items into SceneHdrColor before bloom, depth-tested
    /// against ResolvedSceneDepth. Consumes SceneHdrColor and ResolvedSceneDepth.
    Core::RendererResult Renderer::build_frame_feature_light_indicators(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::light_indicators");
        BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        FrameSubmission &submission = *state.submission;
        if (submission.gizmo_draws.empty()) {
            return {};
        }
        const RenderGraphTextureHandle scene_color = context.resources.texture<RenderGraphSemantics::SceneHdrColor>();
        const RenderGraphTextureHandle depth = context.resources.texture<RenderGraphSemantics::ResolvedSceneDepth>();
        const Core::Extent2D render_extent = context.module.render_extent;
        const u64 frame_index = context.frame_index;
        const array<RHI::Format, 1> gizmo_color_formats{submission.deferred_formats.scene_color};
        context.graph.add_render_pass("pre-bloom light indicators"_ustr)
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = scene_color,
                .load_op = RHI::LoadOp::Load,
                .store_op = RHI::StoreOp::Store,
            })
            .set_depth_stencil_attachment(RenderGraphDepthStencilAttachmentDesc{
                .texture = depth,
                .depth_load_op = submission.render_graph.render_scene ? RHI::LoadOp::Load : RHI::LoadOp::Clear,
                .depth_store_op = RHI::StoreOp::Store,
                .clear_value = RHI::ClearDepthStencil{.depth = 1.0f, .stencil = 0},
            })
            .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.x, .height = render_extent.y})
            .set_execute([this, &context, &submission, render_extent, frame_index, gizmo_color_formats](
                             RenderGraphContext &pass_context) -> Core::RendererResult {
                return record_draw_items(
                    pass_context.render_pass(),
                    DrawItemPass{
                        .set = DrawItemSet::Gizmos,
                        .color_formats = std::span<const RHI::Format>{gizmo_color_formats.data(), gizmo_color_formats.size()},
                        .depth_format = submission.deferred_formats.depth,
                        .frame_index = frame_index,
                        .view_projection = submission.view_projection,
                        .standard_depth_test = true,
                        .frustum_cull = false,
                        .allow_bundles = false,
                        .label = "light indicators",
                        .viewport = RHI::Viewport{.x = 0.0f, .y = 0.0f, .width = static_cast<f32>(render_extent.x),
                                                  .height = static_cast<f32>(render_extent.y), .min_depth = 0.0f,
                                                  .max_depth = 1.0f},
                        .scissor = RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.x, .height = render_extent.y},
                    },
                    context);
            });
        return {};
    }

    Core::RendererResult Renderer::build_frame_feature_motion_blur(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::motion_blur");
        BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        [[maybe_unused]] FrameSubmission &submission = *state.submission;
        [[maybe_unused]] WindowSurfaceRecord &record = *state.record;
        [[maybe_unused]] FrameInFlight &slot = *state.slot;
        [[maybe_unused]] RenderGraph &graph = context.graph;
        [[maybe_unused]] RenderGraphBlackboard &graph_resources = context.resources;
        [[maybe_unused]] RenderGraphModuleBuildContext &module_context = context.module;
        [[maybe_unused]] const Core::Extent2D presentation_extent = context.module.presentation_extent;
        [[maybe_unused]] const RHI::Format output_format = context.output_format;
        [[maybe_unused]] const bool hdr_output = context.hdr_output;
        [[maybe_unused]] const bool direct_overlay_presentation = context.direct_overlay_presentation;
        [[maybe_unused]] const RenderGraphTextureHandle final_output = context.final_output;
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_motion = state.gbuffer_motion;
        [[maybe_unused]] const RenderGraphTextureHandle depth_texture = state.depth_texture;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &logical_graph_textures = *state.logical_graph_textures;
        [[maybe_unused]] const auto &map_logical_texture = state.map_logical_texture;
        [[maybe_unused]] const u32 frame_slot_index = state.frame_slot_index;
        [[maybe_unused]] const glm::vec4 background = state.background;

        if (context.settings.motion_blur.enabled) {
            const RenderGraphTextureHandle source = graph_resources.texture<RenderGraphSemantics::SceneHdrColor>();
            if (!source) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Motion blur needs the SceneHdrColor texture, but no earlier feature published it.");
            }
            if (!gbuffer_motion || !depth_texture) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Motion blur requires both a motion-vector and a depth render-graph texture.");
            }
            auto blurred = add_motion_blur_passes(
                *this, graph, context.transient_bind_groups,
                MotionBlurDescription{
                    .source = source,
                    .motion = gbuffer_motion,
                    .depth = depth_texture,
                    .extent = module_context.render_extent,
                    .output_extent = module_context.render_texture_extent(),
                    .output_format = submission.deferred_formats.scene_color,
                },
                context.settings);
            if (!blurred.has_value()) {
                return unexpected(blurred.error());
            }
            graph_resources.publish_texture<RenderGraphSemantics::SceneHdrColor>(*blurred);
        }
        return {};
    }

    Core::RendererResult Renderer::build_frame_feature_post_process_aa(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::post_process_aa");
        BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        [[maybe_unused]] FrameSubmission &submission = *state.submission;
        [[maybe_unused]] WindowSurfaceRecord &record = *state.record;
        [[maybe_unused]] FrameInFlight &slot = *state.slot;
        [[maybe_unused]] RenderGraph &graph = context.graph;
        [[maybe_unused]] RenderGraphBlackboard &graph_resources = context.resources;
        [[maybe_unused]] RenderGraphModuleBuildContext &module_context = context.module;
        [[maybe_unused]] const Core::Extent2D presentation_extent = context.module.presentation_extent;
        [[maybe_unused]] const RHI::Format output_format = context.output_format;
        [[maybe_unused]] const bool hdr_output = context.hdr_output;
        [[maybe_unused]] const bool direct_overlay_presentation = context.direct_overlay_presentation;
        [[maybe_unused]] const RenderGraphTextureHandle final_output = context.final_output;
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_motion = state.gbuffer_motion;
        [[maybe_unused]] const RenderGraphTextureHandle depth_texture = state.depth_texture;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &logical_graph_textures = *state.logical_graph_textures;
        [[maybe_unused]] const auto &map_logical_texture = state.map_logical_texture;
        [[maybe_unused]] const u32 frame_slot_index = state.frame_slot_index;
        [[maybe_unused]] const glm::vec4 background = state.background;

        if (context.settings.post_process_aa != 0) {
            const RenderGraphTextureHandle source = graph_resources.texture<RenderGraphSemantics::SceneHdrColor>();
            if (!source) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Anti-aliasing needs the SceneHdrColor texture, but no earlier feature published it.");
            }
            // Write into the shared scratch texture when one exists, otherwise a transient of our own.
            RenderGraphTextureHandle destination = graph_resources.texture<RenderGraphSemantics::ReusableSceneHdrScratch>();
            if (!destination || destination == source) {
                destination = graph.create_texture(RenderGraphTextureDesc{
                    .format = submission.deferred_formats.scene_color,
                    .extent = module_context.render_texture_extent(),
                    .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled | RHI::TextureUsage::Storage |
                             RHI::TextureUsage::TransferSrc | RHI::TextureUsage::TransferDst,
                    .label = "scene-linear spatial anti-aliasing target",
                });
            }
            if (Core::RendererResult added = add_post_process_aa_pass(
                    *this, graph, context.transient_bind_groups, source, destination, module_context.render_extent,
                    submission.deferred_formats.scene_color, context.settings);
                !added.has_value()) {
                return added;
            }
            graph_resources.publish_texture<RenderGraphSemantics::SceneHdrColor>(destination);
        }
        map_logical_texture(
            submission.render_graph.custom_graph.anti_aliasing_output,
            graph_resources.texture<RenderGraphSemantics::SceneHdrColor>());
        return {};
    }

    Core::RendererResult Renderer::build_frame_feature_effects_before_bloom(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::effects_before_bloom");
        BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        [[maybe_unused]] FrameSubmission &submission = *state.submission;
        [[maybe_unused]] WindowSurfaceRecord &record = *state.record;
        [[maybe_unused]] FrameInFlight &slot = *state.slot;
        [[maybe_unused]] RenderGraph &graph = context.graph;
        [[maybe_unused]] RenderGraphBlackboard &graph_resources = context.resources;
        [[maybe_unused]] RenderGraphModuleBuildContext &module_context = context.module;
        [[maybe_unused]] const Core::Extent2D presentation_extent = context.module.presentation_extent;
        [[maybe_unused]] const RHI::Format output_format = context.output_format;
        [[maybe_unused]] const bool hdr_output = context.hdr_output;
        [[maybe_unused]] const bool direct_overlay_presentation = context.direct_overlay_presentation;
        [[maybe_unused]] const RenderGraphTextureHandle final_output = context.final_output;
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_motion = state.gbuffer_motion;
        [[maybe_unused]] const RenderGraphTextureHandle depth_texture = state.depth_texture;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &logical_graph_textures = *state.logical_graph_textures;
        [[maybe_unused]] const auto &map_logical_texture = state.map_logical_texture;
        [[maybe_unused]] const u32 frame_slot_index = state.frame_slot_index;
        [[maybe_unused]] const glm::vec4 background = state.background;

        if (Core::RendererResult effects = build_custom_graph_stage(
                module_context, submission, PostProcessStage::BeforeBloom, logical_graph_textures);
            !effects.has_value()) {
            return effects;
        }
        return {};
    }

    Core::RendererResult Renderer::build_frame_feature_bloom(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::bloom");
        BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        [[maybe_unused]] FrameSubmission &submission = *state.submission;
        [[maybe_unused]] WindowSurfaceRecord &record = *state.record;
        [[maybe_unused]] FrameInFlight &slot = *state.slot;
        [[maybe_unused]] RenderGraph &graph = context.graph;
        [[maybe_unused]] RenderGraphBlackboard &graph_resources = context.resources;
        [[maybe_unused]] RenderGraphModuleBuildContext &module_context = context.module;
        [[maybe_unused]] const Core::Extent2D presentation_extent = context.module.presentation_extent;
        [[maybe_unused]] const RHI::Format output_format = context.output_format;
        [[maybe_unused]] const bool hdr_output = context.hdr_output;
        [[maybe_unused]] const bool direct_overlay_presentation = context.direct_overlay_presentation;
        [[maybe_unused]] const RenderGraphTextureHandle final_output = context.final_output;
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_motion = state.gbuffer_motion;
        [[maybe_unused]] const RenderGraphTextureHandle depth_texture = state.depth_texture;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &logical_graph_textures = *state.logical_graph_textures;
        [[maybe_unused]] const auto &map_logical_texture = state.map_logical_texture;
        [[maybe_unused]] const u32 frame_slot_index = state.frame_slot_index;
        [[maybe_unused]] const glm::vec4 background = state.background;

        if (context.settings.bloom && context.settings.bloom_intensity > 0.0f) {
            const RenderGraphTextureHandle scene_source = graph_resources.texture<RenderGraphSemantics::SceneHdrColor>();
            if (!scene_source) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Bloom needs the SceneHdrColor texture, but no earlier feature published it.");
            }
            auto composite = add_bloom_passes(
                *this, graph, context.transient_bind_groups,
                BloomDescription{
                    .source = scene_source,
                    .source_extent = module_context.render_extent,
                    .max_levels = context.settings.bloom_max_levels,
                    .downsample_ratio = context.settings.bloom_downsample_ratio,
                    .output_extent = module_context.render_texture_extent(),
                    .output_format = submission.deferred_formats.scene_color,
                    // Thresholded bloom is an emission layer (additive); the no-threshold mode
                    // interpolates so a constant HDR image is conserved.
                    .additive_composite = context.settings.bloom_threshold > 0.0f,
                },
                context.settings);
            if (!composite.has_value()) {
                return unexpected(composite.error());
            }
            graph_resources.publish_texture<RenderGraphSemantics::SceneHdrColor>(*composite);
        }

        map_logical_texture(
            submission.render_graph.custom_graph.bloom_output,
            graph_resources.texture<RenderGraphSemantics::SceneHdrColor>());
        return {};
    }

    Core::RendererResult Renderer::build_frame_feature_effects_after_bloom(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::effects_after_bloom");
        BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        [[maybe_unused]] FrameSubmission &submission = *state.submission;
        [[maybe_unused]] WindowSurfaceRecord &record = *state.record;
        [[maybe_unused]] FrameInFlight &slot = *state.slot;
        [[maybe_unused]] RenderGraph &graph = context.graph;
        [[maybe_unused]] RenderGraphBlackboard &graph_resources = context.resources;
        [[maybe_unused]] RenderGraphModuleBuildContext &module_context = context.module;
        [[maybe_unused]] const Core::Extent2D presentation_extent = context.module.presentation_extent;
        [[maybe_unused]] const RHI::Format output_format = context.output_format;
        [[maybe_unused]] const bool hdr_output = context.hdr_output;
        [[maybe_unused]] const bool direct_overlay_presentation = context.direct_overlay_presentation;
        [[maybe_unused]] const RenderGraphTextureHandle final_output = context.final_output;
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_motion = state.gbuffer_motion;
        [[maybe_unused]] const RenderGraphTextureHandle depth_texture = state.depth_texture;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &logical_graph_textures = *state.logical_graph_textures;
        [[maybe_unused]] const auto &map_logical_texture = state.map_logical_texture;
        [[maybe_unused]] const u32 frame_slot_index = state.frame_slot_index;
        [[maybe_unused]] const glm::vec4 background = state.background;

        if (Core::RendererResult effects = build_custom_graph_stage(
                module_context, submission, PostProcessStage::AfterBloomBeforeToneMap, logical_graph_textures);
            !effects.has_value()) {
            return effects;
        }
        return {};
    }

    Core::RendererResult Renderer::build_frame_feature_tone_mapping(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::tone_mapping");
        BuiltinFrameState &state = *static_cast<BuiltinFrameState *>(context.builtin);
        [[maybe_unused]] FrameSubmission &submission = *state.submission;
        [[maybe_unused]] WindowSurfaceRecord &record = *state.record;
        [[maybe_unused]] FrameInFlight &slot = *state.slot;
        [[maybe_unused]] RenderGraph &graph = context.graph;
        [[maybe_unused]] RenderGraphBlackboard &graph_resources = context.resources;
        [[maybe_unused]] RenderGraphModuleBuildContext &module_context = context.module;
        [[maybe_unused]] const Core::Extent2D presentation_extent = context.module.presentation_extent;
        [[maybe_unused]] const RHI::Format output_format = context.output_format;
        [[maybe_unused]] const bool hdr_output = context.hdr_output;
        [[maybe_unused]] const bool direct_overlay_presentation = context.direct_overlay_presentation;
        [[maybe_unused]] const RenderGraphTextureHandle final_output = context.final_output;
        [[maybe_unused]] const RenderGraphTextureHandle gbuffer_motion = state.gbuffer_motion;
        [[maybe_unused]] const RenderGraphTextureHandle depth_texture = state.depth_texture;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &logical_graph_textures = *state.logical_graph_textures;
        [[maybe_unused]] const auto &map_logical_texture = state.map_logical_texture;
        [[maybe_unused]] const u32 frame_slot_index = state.frame_slot_index;
        [[maybe_unused]] const glm::vec4 background = state.background;

        if (!direct_overlay_presentation) {
            const bool has_display_effects = std::ranges::any_of(
                submission.render_graph.custom_graph.passes,
                [](const CustomGraphPass &pass) { return pass.stage == PostProcessStage::AfterToneMap; });
            // With display-space effects, tone mapping renders into an intermediate of the presentation
            // format and the effect chain's last pass writes the real target.
            const RenderGraphTextureHandle tonemap_destination = has_display_effects
                ? graph.create_texture(RenderGraphTextureDesc{
                      .format = output_format,
                      .extent = RHI::Extent3D{
                          .width = presentation_extent.x,
                          .height = presentation_extent.y,
                          .depth_or_layers = 1,
                      },
                      .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled,
                      .label = "display-encoded scene",
                  })
                : RenderGraphTextureHandle{};
            submission.render_graph.tone_mapping_hdr_output = hdr_output;
            submission.render_graph.tone_mapping_hdr_color_space = record.presentation.hdr_color_space;
            const RenderGraphTextureHandle scene_source = graph_resources.texture<RenderGraphSemantics::SceneHdrColor>();
            const RenderGraphTextureHandle tonemap_target = tonemap_destination
                ? tonemap_destination
                : graph_resources.texture<RenderGraphSemantics::PresentationTarget>();
            if (Core::RendererResult tone_mapped = add_tone_mapping_pass(
                    context, scene_source, tonemap_target, submission.render_graph, false,
                    submission.render_graph.tone_mapping ? "tonemap" : "present scene color");
                !tone_mapped.has_value()) {
                return tone_mapped;
            }
            if (has_display_effects) {
                if (Core::RendererResult display_effects = build_display_effects_stage(
                        module_context, submission, tonemap_destination, final_output, output_format,
                        logical_graph_textures);
                    !display_effects.has_value()) {
                    return display_effects;
                }
            }
        }
        return {};
    }

    Core::RendererResult Renderer::set_space_model(SpaceModel model) {
        ZoneScopedN("Renderer::set_space_model");
        const std::string model_name = model.name;
        if (model.item_visible) {
            model.gpu_culling = false;
        }
        if (model.shader_source && !model.shader_source->empty()) {
            Core::Slang::override_shader_module("sturdy_space", *model.shader_source);
        } else {
            Core::Slang::remove_shader_module_override("sturdy_space");
        }
        *space_model_.lock() = std::make_shared<const SpaceModel>(std::move(model));
        if (rhi_device() == nullptr) {
            return {};
        }
        // Existing materials compiled their vertex stage against the previous module.
        wait_idle();
        // The engine's own effect shaders compile lazily and cache their pipelines; dropping them makes the next
        // frame rebuild them against the new module.
        destroy_shadow_lighting_resources();
        destroy_restir_gi_resources();
        destroy_spectral_path_tracing_resources();
        auto guard = shader_hot_reload_lock_.lock();
        vector<pair<MaterialTemplateHandle, bool>> handles;
        for (const MaterialTemplateResource &tmpl : material_templates_) {
            if (tmpl.alive) {
                handles.emplace_back(tmpl.handle, tmpl.hot_reloadable);
            }
        }
        for (const auto &[handle, from_file] : handles) {
            if (Core::RendererResult reloaded = reload_material_template(handle, true); !reloaded.has_value()) {
                if (from_file) {
                    return reloaded;
                }
                Foundation::log_warn("Space model '{}': could not recompile an in-memory material template: {}",
                                     model_name, reloaded.error().message);
            }
        }
        return {};
    }

    std::shared_ptr<const SpaceModel> Renderer::space_model() const { return *space_model_.lock(); }

    void Renderer::register_builtin_frame_features() {
        auto pipeline = frame_pipeline_.lock();
        (void)pipeline->add("mesh_skinning", [this](FrameBuildContext &context) { return build_frame_feature_mesh_skinning(context); }, FrameStage::Scene);
        (void)pipeline->add("atmosphere_luts", [this](FrameBuildContext &context) { return build_frame_feature_atmosphere_luts(context); }, FrameStage::Scene);
        (void)pipeline->add("instance_culling", [this](FrameBuildContext &context) { return build_frame_feature_instance_culling(context); }, FrameStage::Scene);
        (void)pipeline->add("shadow_maps", [this](FrameBuildContext &context) { return build_frame_feature_shadow_maps(context); }, FrameStage::Scene);
        (void)pipeline->add("z_prepass", [this](FrameBuildContext &context) { return build_frame_feature_z_prepass(context); }, FrameStage::Scene);
        (void)pipeline->add("gbuffer", [this](FrameBuildContext &context) { return build_frame_feature_gbuffer(context); }, FrameStage::Scene);
        (void)pipeline->add("hiz_build", [this](FrameBuildContext &context) { return build_frame_feature_hiz_build(context); }, FrameStage::Scene);
        (void)pipeline->add("spectral_path_tracing", [this](FrameBuildContext &context) { return build_frame_feature_spectral_path_tracing(context); }, FrameStage::Scene);
        (void)pipeline->add("ambient_occlusion", [this](FrameBuildContext &context) { return build_frame_feature_ambient_occlusion(context); }, FrameStage::Scene);
        (void)pipeline->add("global_illumination", [this](FrameBuildContext &context) { return build_frame_feature_global_illumination(context); }, FrameStage::Scene);
        (void)pipeline->add("screen_space_gi", [](FrameBuildContext &context) { return build_screen_space_gi_feature(context); }, FrameStage::Scene);
        (void)pipeline->add("lighting", [this](FrameBuildContext &context) { return build_frame_feature_lighting(context); }, FrameStage::Scene);
        (void)pipeline->add("restir_history_copy", [this](FrameBuildContext &context) { return build_frame_feature_restir_history_copy(context); }, FrameStage::Scene);
        (void)pipeline->add("msaa_resolve", [this](FrameBuildContext &context) { return build_frame_feature_msaa_resolve(context); }, FrameStage::Scene);
        (void)pipeline->add("screen_space_gi_history", [](FrameBuildContext &context) { return build_screen_space_gi_history_feature(context); }, FrameStage::Scene);
        (void)pipeline->add("scene_background", [this](FrameBuildContext &context) { return build_frame_feature_scene_background(context); });
        (void)pipeline->add("light_indicators", [this](FrameBuildContext &context) { return build_frame_feature_light_indicators(context); });
        (void)pipeline->add("motion_blur", [this](FrameBuildContext &context) { return build_frame_feature_motion_blur(context); });
        (void)pipeline->add("post_process_aa", [this](FrameBuildContext &context) { return build_frame_feature_post_process_aa(context); });
        (void)pipeline->add("effects_before_bloom", [this](FrameBuildContext &context) { return build_frame_feature_effects_before_bloom(context); });
        (void)pipeline->add("bloom", [this](FrameBuildContext &context) { return build_frame_feature_bloom(context); });
        (void)pipeline->add("effects_after_bloom", [this](FrameBuildContext &context) { return build_frame_feature_effects_after_bloom(context); });
        (void)pipeline->add("temporal_upscale", [](FrameBuildContext &context) { return build_temporal_upscale_feature(context); });
        (void)pipeline->add("auto_exposure", [](FrameBuildContext &context) { return build_auto_exposure_feature(context); });
        (void)pipeline->add("camera_emulation", [](FrameBuildContext &context) { return build_camera_emulation_feature(context); });
        (void)pipeline->add("tone_mapping", [this](FrameBuildContext &context) { return build_frame_feature_tone_mapping(context); });
        (void)pipeline->add("overlay_passes", [](FrameBuildContext &context) {
            return add_overlay_passes(context, std::span<const OverlayPass>{context.settings.overlay_passes});
        });
    }

} // namespace SFT::Renderer
