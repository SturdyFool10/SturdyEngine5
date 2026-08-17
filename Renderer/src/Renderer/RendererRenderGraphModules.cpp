#include <Foundation/src/Foundation.hpp>


#include <Renderer/Renderer.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Renderer {

    namespace {

        [[nodiscard]] Core::RendererResult missing_module_texture(const ustr &module, const ustr &semantic) {
            UString message{module};
            message.append(" requires the render-graph semantic texture '"_ustr);
            message.append(semantic);
            message.append("'."_ustr);
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed, message.cpp_string());
        }

    } // namespace

    Core::RendererResult Renderer::build_deferred_msaa_module(
        RenderGraphModuleBuildContext &context,
        FrameSubmission &submission,
        RHI::SampleCount samples) {
        ZoneScopedN("Renderer::build_deferred_msaa_module");
        using namespace RenderGraphSemantics;

        const RenderGraphTextureHandle scene_color = context.resources.texture<SceneHdrColor>();
        const RenderGraphTextureHandle resolved_depth = context.resources.texture<ResolvedSceneDepth>();
        const RenderGraphTextureHandle raster_depth = context.resources.texture<RasterVisibilityDepth>();
        if (!scene_color) {
            return missing_module_texture("deferred MSAA reconstruction"_ustr, "SceneHdrColor"_ustr);
        }
        if (!resolved_depth) {
            return missing_module_texture("deferred MSAA reconstruction"_ustr, "ResolvedSceneDepth"_ustr);
        }
        if (!raster_depth) {
            return missing_module_texture("deferred MSAA reconstruction"_ustr, "RasterVisibilityDepth"_ustr);
        }

        RenderGraphTextureHandle destination = context.resources.texture<ReusableSceneHdrScratch>();
        if (!destination) {
            destination = context.graph.create_texture(RenderGraphTextureDesc{
                .format = submission.deferred_formats.scene_color,
                .extent = context.render_texture_extent(),
                .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled |
                         RHI::TextureUsage::Storage | RHI::TextureUsage::TransferSrc |
                         RHI::TextureUsage::TransferDst,
                .label = "deferred MSAA reconstruction target",
            });
        }

        context.graph.add_render_pass("deferred MSAA reconstruction"_ustr)
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = destination,
                .load_op = RHI::LoadOp::DontCare,
                .store_op = RHI::StoreOp::Store,
            })
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = scene_color})
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = resolved_depth})
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = raster_depth})
            .set_render_area(RHI::Rect2D{
                .x = 0,
                .y = 0,
                .width = context.render_extent.x,
                .height = context.render_extent.y,
            })
            .set_execute([this, &submission, extent = context.render_extent, samples,
                          scene_color, resolved_depth, raster_depth](RenderGraphContext &graph_context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = graph_context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .width = static_cast<f32>(extent.x),
                    .height = static_cast<f32>(extent.y),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = extent.x,
                    .height = extent.y,
                });
                return record_deferred_msaa_reconstruction(
                    pass,
                    graph_context.texture(scene_color).default_view,
                    graph_context.texture(resolved_depth).default_view,
                    graph_context.texture(raster_depth).default_view,
                    submission.deferred_formats.scene_color,
                    extent,
                    samples,
                    submission.camera.near_plane,
                    submission.camera.far_plane,
                    submission.transient_bind_groups);
            });

        context.resources.publish_texture<SceneHdrColor>(destination);
        return {};
    }

    Core::RendererResult Renderer::build_post_process_aa_module(
        RenderGraphModuleBuildContext &context,
        FrameSubmission &submission) {
        ZoneScopedN("Renderer::build_post_process_aa_module");
        using namespace RenderGraphSemantics;

        if (submission.render_graph.post_process_aa == 0) {
            return {};
        }
        const RenderGraphTextureHandle source = context.resources.texture<SceneHdrColor>();
        if (!source) {
            return missing_module_texture("scene-linear spatial anti-aliasing"_ustr, "SceneHdrColor"_ustr);
        }

        RenderGraphTextureHandle destination = context.resources.texture<ReusableSceneHdrScratch>();
        if (!destination || destination == source) {
            destination = context.graph.create_texture(RenderGraphTextureDesc{
                .format = submission.deferred_formats.scene_color,
                .extent = context.render_texture_extent(),
                .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled |
                         RHI::TextureUsage::Storage | RHI::TextureUsage::TransferSrc |
                         RHI::TextureUsage::TransferDst,
                .label = "scene-linear spatial anti-aliasing target",
            });
        }

        context.graph.add_render_pass("scene-linear spatial anti-aliasing"_ustr)
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = destination,
                .load_op = RHI::LoadOp::DontCare,
                .store_op = RHI::StoreOp::Store,
            })
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = source})
            .set_render_area(RHI::Rect2D{
                .x = 0,
                .y = 0,
                .width = context.render_extent.x,
                .height = context.render_extent.y,
            })
            .set_execute([this, &submission, source,
                          extent = context.render_extent](RenderGraphContext &graph_context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = graph_context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .width = static_cast<f32>(extent.x),
                    .height = static_cast<f32>(extent.y),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = extent.x,
                    .height = extent.y,
                });
                return record_post_process_aa(
                    pass,
                    graph_context.texture(source).default_view,
                    submission.deferred_formats.scene_color,
                    submission.render_graph,
                    submission.transient_bind_groups);
            });

        context.resources.publish_texture<SceneHdrColor>(destination);
        return {};
    }

    Core::RendererResult Renderer::build_custom_graph_stage(
        RenderGraphModuleBuildContext &context,
        FrameSubmission &submission,
        PostProcessStage stage,
        span<RenderGraphTextureHandle> logical_textures) {
        ZoneScopedN("Renderer::build_custom_graph_stage");
        using namespace RenderGraphSemantics;

        RenderGraphTextureHandle source = context.resources.texture<SceneHdrColor>();
        if (!source) {
            return missing_module_texture("custom graph stage"_ustr, "SceneHdrColor"_ustr);
        }

        constexpr RHI::TextureUsage custom_hdr_usage =
            RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled |
            RHI::TextureUsage::Storage | RHI::TextureUsage::TransferSrc | RHI::TextureUsage::TransferDst;
        const CustomGraphProgram &program = submission.render_graph.custom_graph;
        const auto concrete_texture = [logical_textures](LogicalRenderGraphTexture logical) {
            return logical && logical.index < logical_textures.size()
                ? logical_textures[logical.index]
                : RenderGraphTextureHandle{};
        };

        for (usize pass_index = 0; pass_index < program.passes.size(); ++pass_index) {
            const CustomGraphPass &custom_pass = program.passes[pass_index];
            if (custom_pass.stage != stage) {
                continue;
            }
            if (!custom_pass.output || custom_pass.output.index >= logical_textures.size()) {
                return Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "Custom graph pass has an invalid logical output texture.");
            }
            const RenderGraphTextureHandle input = concrete_texture(custom_pass.input);
            if (!input) {
                return Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "Custom graph pass input was not produced before the pass was lowered.");
            }

            const RenderGraphTextureHandle output = context.graph.create_texture(RenderGraphTextureDesc{
                .format = submission.deferred_formats.scene_color,
                .extent = context.render_texture_extent(),
                .usage = custom_hdr_usage,
                .label = "custom graph HDR target",
            });
            switch (custom_pass.kind) {
                case CustomGraphPassKind::RasterEffect:
                    context.graph.add_render_pass("custom graph raster effect"_ustr)
                        .add_color_attachment(RenderGraphColorAttachmentDesc{
                            .texture = output,
                            .load_op = RHI::LoadOp::DontCare,
                            .store_op = RHI::StoreOp::Store,
                        })
                        .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = input})
                        .set_render_area(RHI::Rect2D{
                            .x = 0,
                            .y = 0,
                            .width = context.render_extent.x,
                            .height = context.render_extent.y,
                        })
                        .set_execute([this, &submission, input, pass_index,
                                      extent = context.render_extent](RenderGraphContext &graph_context) -> Core::RendererResult {
                            RHI::RenderPassEncoder &pass = graph_context.render_pass();
                            pass.set_viewport(RHI::Viewport{
                                .width = static_cast<f32>(extent.x),
                                .height = static_cast<f32>(extent.y),
                                .min_depth = 0.0f,
                                .max_depth = 1.0f,
                            });
                            pass.set_scissor(RHI::Rect2D{
                                .x = 0,
                                .y = 0,
                                .width = extent.x,
                                .height = extent.y,
                            });
                            return record_custom_post_process(
                                pass,
                                graph_context.texture(input).default_view,
                                submission.deferred_formats.scene_color,
                                submission.render_graph.custom_graph.passes[pass_index].raster,
                                submission.transient_bind_groups);
                        });
                    break;
                case CustomGraphPassKind::ComputeEffect:
                    if (submission.deferred_formats.scene_color != RHI::Format::RGBA16Float) {
                        return Core::graphics_backend_error(
                            Core::GraphicsBackendErrorCode::OperationFailed,
                            "Custom compute effects currently require the default RGBA16Float scene HDR format.");
                    }
                    context.graph.add_compute_pass("custom graph compute effect"_ustr)
                        .add_sampled_texture(input)
                        .add_storage_texture(RenderGraphStorageTextureAccessDesc{
                            .texture = output,
                            .read = false,
                            .write = true,
                        })
                        .set_execute([this, &submission, input, output, pass_index,
                                      extent = context.render_extent](RenderGraphComputeContext &graph_context) -> Core::RendererResult {
                            return record_custom_compute_effect(
                                graph_context.compute_pass(),
                                graph_context.texture(input).default_view,
                                graph_context.texture(output).default_view,
                                extent,
                                submission.render_graph.custom_graph.passes[pass_index].compute,
                                submission.transient_bind_groups);
                        });
                    break;
                case CustomGraphPassKind::Copy:
                    context.graph.add_copy_pass(RenderGraphCopyDesc{
                        .source = input,
                        .destination = output,
                        .label = UString{"custom graph exact HDR copy"_ustr},
                    });
                    break;
            }
            logical_textures[custom_pass.output.index] = output;
        }

        const LogicalRenderGraphTexture selected = stage == PostProcessStage::BeforeBloom
            ? program.before_bloom_presentation_output
            : program.after_bloom_presentation_output;
        if (selected) {
            source = concrete_texture(selected);
            if (!source) {
                return Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "Custom graph presentation output was not produced in its declared HDR stage.");
            }
        }



        for (usize effect_index = 0; effect_index < submission.render_graph.custom_post_processes.size(); ++effect_index) {
            if (submission.render_graph.custom_post_processes[effect_index].stage != stage) {
                continue;
            }
            const RenderGraphTextureHandle input = source;
            const RenderGraphTextureHandle output = context.graph.create_texture(RenderGraphTextureDesc{
                .format = submission.deferred_formats.scene_color,
                .extent = context.render_texture_extent(),
                .usage = custom_hdr_usage,
                .label = "legacy custom HDR post-process target",
            });
            context.graph.add_render_pass("legacy custom HDR post-process"_ustr)
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = output,
                    .load_op = RHI::LoadOp::DontCare,
                    .store_op = RHI::StoreOp::Store,
                })
                .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = input})
                .set_render_area(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = context.render_extent.x,
                    .height = context.render_extent.y,
                })
                .set_execute([this, &submission, input, effect_index,
                              extent = context.render_extent](RenderGraphContext &graph_context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = graph_context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .width = static_cast<f32>(extent.x),
                        .height = static_cast<f32>(extent.y),
                        .min_depth = 0.0f,
                        .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{
                        .x = 0,
                        .y = 0,
                        .width = extent.x,
                        .height = extent.y,
                    });
                    return record_custom_post_process(
                        pass,
                        graph_context.texture(input).default_view,
                        submission.deferred_formats.scene_color,
                        submission.render_graph.custom_post_processes[effect_index],
                        submission.transient_bind_groups);
                });
            source = output;
        }

        for (LogicalRenderGraphTexture logical_output : program.outputs) {
            const RenderGraphTextureHandle output = concrete_texture(logical_output);
            if (output) {
                context.graph.mark_output(output);
            }
        }
        context.resources.publish_texture<SceneHdrColor>(source);
        return {};
    }

    Core::RendererResult Renderer::build_bloom_module(
        RenderGraphModuleBuildContext &context,
        FrameSubmission &submission,
        FrameInFlight &frame_slot,
        bool enabled,
        RHI::Format bloom_format) {
        ZoneScopedN("Renderer::build_bloom_module");
        using namespace RenderGraphSemantics;

        if (!enabled) {
            return {};
        }
        const RenderGraphTextureHandle scene_source = context.resources.texture<SceneHdrColor>();
        if (!scene_source) {
            return missing_module_texture("bloom"_ustr, "SceneHdrColor"_ustr);
        }

        const vector<Core::Extent2D> &bloom_extents = frame_slot.bloom_targets.extents;
        vector<RenderGraphTextureHandle> bloom_levels;
        bloom_levels.reserve(bloom_extents.size());
        for (usize level = 0; level < bloom_extents.size(); ++level) {
            bloom_levels.push_back(context.graph.import_texture(RenderGraphImportedTextureDesc{
                .texture = frame_slot.bloom_targets.textures[level],
                .default_view = frame_slot.bloom_targets.views[level],
                .format = bloom_format,
                .extent = RHI::Extent3D{
                    .width = bloom_extents[level].x,
                    .height = bloom_extents[level].y,
                    .depth_or_layers = 1,
                },
                .mip_levels = 1,
                .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled,
                .initial_layout = RHI::TextureLayout::Undefined,
                .initial_stage = RHI::PipelineStage::None,
                .initial_access = RHI::AccessFlags::None,
                .label = "persistent fractional bloom level",
            }));
        }

        for (usize level = 0; level < bloom_extents.size(); ++level) {
            const RenderGraphTextureHandle source = level == 0 ? scene_source : bloom_levels[level - 1];
            const RHI::TextureViewHandle level_source_view = level == 0
                ? RHI::TextureViewHandle{}
                : frame_slot.bloom_targets.views[level - 1];
            const Core::Extent2D source_extent = level == 0
                ? context.render_extent
                : bloom_extents[level - 1];
            const Core::Extent2D destination_extent = bloom_extents[level];
            const RHI::BindGroupHandle cached_bind_group =
                frame_slot.bloom_targets.downsample_bind_groups[level];

            context.graph.add_render_pass("bloom downsample"_ustr)
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = bloom_levels[level],
                    .view = frame_slot.bloom_targets.views[level],
                    .load_op = RHI::LoadOp::DontCare,
                    .store_op = RHI::StoreOp::Store,
                })
                .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = source})
                .set_render_area(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = destination_extent.x,
                    .height = destination_extent.y,
                })
                .set_execute([this, &submission, scene_source, level_source_view, source_extent,
                              destination_extent, level, cached_bind_group](
                                 RenderGraphContext &graph_context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = graph_context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .width = static_cast<f32>(destination_extent.x),
                        .height = static_cast<f32>(destination_extent.y),
                        .min_depth = 0.0f,
                        .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{
                        .x = 0,
                        .y = 0,
                        .width = destination_extent.x,
                        .height = destination_extent.y,
                    });
                    RHI::TextureViewHandle source_view = level_source_view;
                    RHI::BindGroupHandle bind_group = cached_bind_group;
                    if (level == 0) {
                        source_view = graph_context.texture(scene_source).default_view;
                        auto dynamic_bind_group = create_bloom_source_bind_group(source_view);
                        if (!dynamic_bind_group.has_value()) {
                            return unexpected(dynamic_bind_group.error());
                        }
                        {
                            auto bind_groups_guard = transient_bind_groups_lock_.lock();
                            submission.transient_bind_groups.push_back(*dynamic_bind_group);
                        }
                        bind_group = *dynamic_bind_group;
                    }
                    return record_bloom_downsample(
                        pass,
                        source_view,
                        glm::vec2{
                            1.0f / static_cast<f32>(source_extent.x),
                            1.0f / static_cast<f32>(source_extent.y),
                        },
                        submission.render_graph,
                        glm::vec2{
                            0.5f * static_cast<f32>(source_extent.x) /
                                static_cast<f32>(destination_extent.x),
                            0.5f * static_cast<f32>(source_extent.y) /
                                static_cast<f32>(destination_extent.y),
                        },
                        level == 0,
                        bind_group);
                });
        }

        for (usize level = bloom_extents.size(); level-- > 1;) {
            const Core::Extent2D source_extent = bloom_extents[level];
            const Core::Extent2D destination_extent = bloom_extents[level - 1];
            const RHI::TextureViewHandle source_view = frame_slot.bloom_targets.views[level];
            const RHI::BindGroupHandle bind_group = frame_slot.bloom_targets.upsample_bind_groups[level];

            context.graph.add_render_pass("bloom upsample"_ustr)
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = bloom_levels[level - 1],
                    .view = frame_slot.bloom_targets.views[level - 1],
                    .load_op = RHI::LoadOp::Load,
                    .store_op = RHI::StoreOp::Store,
                })
                .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = bloom_levels[level]})
                .set_render_area(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = destination_extent.x,
                    .height = destination_extent.y,
                })
                .set_execute([this, &submission, source_view, source_extent,
                              destination_extent, bind_group](RenderGraphContext &graph_context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = graph_context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .width = static_cast<f32>(destination_extent.x),
                        .height = static_cast<f32>(destination_extent.y),
                        .min_depth = 0.0f,
                        .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{
                        .x = 0,
                        .y = 0,
                        .width = destination_extent.x,
                        .height = destination_extent.y,
                    });
                    return record_bloom_upsample(
                        pass,
                        source_view,
                        glm::vec2{
                            1.0f / static_cast<f32>(source_extent.x),
                            1.0f / static_cast<f32>(source_extent.y),
                        },
                        submission.render_graph,
                        bind_group);
                });
        }

        const RenderGraphTextureHandle composite_destination =
            context.graph.import_texture(RenderGraphImportedTextureDesc{
                .texture = frame_slot.composite_target.texture,
                .default_view = frame_slot.composite_target.view,
                .format = submission.deferred_formats.scene_color,
                .extent = context.render_texture_extent(),
                .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled |
                         RHI::TextureUsage::TransferSrc,
                .initial_layout = RHI::TextureLayout::Undefined,
                .initial_stage = RHI::PipelineStage::None,
                .initial_access = RHI::AccessFlags::None,
                .label = "bloom composite target",
            });
        const RenderGraphTextureHandle reconstructed_bloom = bloom_levels.front();
        context.graph.add_render_pass("bloom composite"_ustr)
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = composite_destination,
                .load_op = RHI::LoadOp::DontCare,
                .store_op = RHI::StoreOp::Store,
            })
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = scene_source})
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = reconstructed_bloom})
            .set_render_area(RHI::Rect2D{
                .x = 0,
                .y = 0,
                .width = context.render_extent.x,
                .height = context.render_extent.y,
            })
            .set_execute([this, &submission, scene_source, reconstructed_bloom,
                          extent = context.render_extent](RenderGraphContext &graph_context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = graph_context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .width = static_cast<f32>(extent.x),
                    .height = static_cast<f32>(extent.y),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = extent.x,
                    .height = extent.y,
                });
                return record_bloom_composite(
                    pass,
                    graph_context.texture(scene_source).default_view,
                    graph_context.texture(reconstructed_bloom).default_view,
                    submission.deferred_formats.scene_color,
                    submission.render_graph.bloom_intensity,
                    submission.render_graph.bloom_threshold > 0.0f,
                    submission.transient_bind_groups);
            });

        context.resources.publish_texture<SceneHdrColor>(composite_destination);
        return {};
    }

    Core::RendererResult Renderer::build_tonemap_module(
        RenderGraphModuleBuildContext &context,
        FrameSubmission &submission,
        RHI::Format presentation_format,
        bool hdr_output,
        Core::HdrColorSpaceMode hdr_color_space) {
        ZoneScopedN("Renderer::build_tonemap_module");
        using namespace RenderGraphSemantics;

        const RenderGraphTextureHandle source = context.resources.texture<SceneHdrColor>();
        const RenderGraphTextureHandle destination = context.resources.texture<PresentationTarget>();
        if (!source) {
            return missing_module_texture("tone mapping"_ustr, "SceneHdrColor"_ustr);
        }
        if (!destination) {
            return missing_module_texture("tone mapping"_ustr, "PresentationTarget"_ustr);
        }

        submission.render_graph.tone_mapping_hdr_output = hdr_output;
        submission.render_graph.tone_mapping_hdr_color_space = hdr_color_space;
        context.graph.add_render_pass(submission.render_graph.tone_mapping ? "tonemap"_ustr : "present scene color"_ustr)
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = destination,
                .load_op = RHI::LoadOp::DontCare,
                .store_op = RHI::StoreOp::Store,
            })
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{
                .texture = source,
                .stages = RHI::PipelineStage::FragmentShader,
                .access = RHI::AccessFlags::ShaderRead,
            })
            .set_render_area(RHI::Rect2D{
                .x = 0,
                .y = 0,
                .width = context.presentation_extent.x,
                .height = context.presentation_extent.y,
            })
            .set_execute([this, &submission, source, presentation_format,
                          extent = context.presentation_extent](RenderGraphContext &graph_context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = graph_context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .x = 0.0f,
                    .y = 0.0f,
                    .width = static_cast<f32>(extent.x),
                    .height = static_cast<f32>(extent.y),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = extent.x,
                    .height = extent.y,
                });
                return record_tonemap(
                    pass,
                    graph_context.texture(source).default_view,
                    presentation_format,
                    submission.render_graph,
                    submission.transient_bind_groups);
            });
        return {};
    }

} // namespace SFT::Renderer
