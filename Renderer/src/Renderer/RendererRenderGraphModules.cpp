#include <Foundation/src/Foundation.hpp>

#include <string>

#include <Renderer/Renderer.hpp>

namespace SFT::Renderer {

    namespace {

        [[nodiscard]] Core::RendererResult missing_module_texture(const char *module, const char *semantic) {
            return Core::graphics_backend_error(
                Core::GraphicsBackendErrorCode::OperationFailed,
                std::string{module} + " requires the render-graph semantic texture '" + semantic + "'.");
        }

    } // namespace

    Core::RendererResult Renderer::build_deferred_msaa_module(
        RenderGraphModuleBuildContext &context,
        FrameSubmission &submission,
        RHI::SampleCount samples) {
        using namespace RenderGraphSemantics;

        const RenderGraphTextureHandle scene_color = context.resources.texture<SceneHdrColor>();
        const RenderGraphTextureHandle resolved_depth = context.resources.texture<ResolvedSceneDepth>();
        const RenderGraphTextureHandle raster_depth = context.resources.texture<RasterVisibilityDepth>();
        if (!scene_color) {
            return missing_module_texture("deferred MSAA reconstruction", "SceneHdrColor");
        }
        if (!resolved_depth) {
            return missing_module_texture("deferred MSAA reconstruction", "ResolvedSceneDepth");
        }
        if (!raster_depth) {
            return missing_module_texture("deferred MSAA reconstruction", "RasterVisibilityDepth");
        }

        RenderGraphTextureHandle destination = context.resources.texture<ReusableSceneHdrScratch>();
        if (!destination) {
            destination = context.graph.create_texture(RenderGraphTextureDesc{
                .format = submission.deferred_formats.scene_color,
                .extent = context.render_texture_extent(),
                .label = "deferred MSAA reconstruction target",
            });
        }

        context.graph.add_render_pass("deferred MSAA reconstruction")
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
                .width = context.render_extent.width,
                .height = context.render_extent.height,
            })
            .set_execute([this, &submission, extent = context.render_extent, samples,
                          scene_color, resolved_depth, raster_depth](RenderGraphContext &graph_context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = graph_context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .width = static_cast<f32>(extent.width),
                    .height = static_cast<f32>(extent.height),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = extent.width,
                    .height = extent.height,
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

    Core::RendererResult Renderer::build_custom_post_process_modules(
        RenderGraphModuleBuildContext &context,
        FrameSubmission &submission,
        PostProcessStage stage) {
        using namespace RenderGraphSemantics;

        RenderGraphTextureHandle source = context.resources.texture<SceneHdrColor>();
        if (!source) {
            return missing_module_texture("custom fullscreen effect chain", "SceneHdrColor");
        }

        for (usize effect_index = 0; effect_index < submission.render_graph.custom_post_processes.size(); ++effect_index) {
            if (submission.render_graph.custom_post_processes[effect_index].stage != stage) {
                continue;
            }

            const RenderGraphTextureHandle input = source;
            const RenderGraphTextureHandle output = context.graph.create_texture(RenderGraphTextureDesc{
                .format = submission.deferred_formats.scene_color,
                .extent = context.render_texture_extent(),
                .label = "custom HDR post-process target",
            });
            context.graph.add_render_pass("custom HDR post-process")
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = output,
                    .load_op = RHI::LoadOp::DontCare,
                    .store_op = RHI::StoreOp::Store,
                })
                .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = input})
                .set_render_area(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = context.render_extent.width,
                    .height = context.render_extent.height,
                })
                .set_execute([this, &submission, input, effect_index,
                              extent = context.render_extent](RenderGraphContext &graph_context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = graph_context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .width = static_cast<f32>(extent.width),
                        .height = static_cast<f32>(extent.height),
                        .min_depth = 0.0f,
                        .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{
                        .x = 0,
                        .y = 0,
                        .width = extent.width,
                        .height = extent.height,
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

        context.resources.publish_texture<SceneHdrColor>(source);
        return {};
    }

    Core::RendererResult Renderer::build_bloom_module(
        RenderGraphModuleBuildContext &context,
        FrameSubmission &submission,
        FrameInFlight &frame_slot,
        bool enabled,
        RHI::Format bloom_format) {
        using namespace RenderGraphSemantics;

        if (!enabled) {
            return {};
        }
        const RenderGraphTextureHandle scene_source = context.resources.texture<SceneHdrColor>();
        if (!scene_source) {
            return missing_module_texture("bloom", "SceneHdrColor");
        }

        const vector<Core::Extent2D> &bloom_extents = frame_slot.bloom_targets.extents;
        const Core::Extent2D bloom_base_extent = bloom_extents.front();
        const RenderGraphTextureHandle bloom_chain = context.graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = frame_slot.bloom_targets.texture,
            .default_view = frame_slot.bloom_targets.views.front(),
            .format = bloom_format,
            .extent = RHI::Extent3D{
                .width = bloom_base_extent.width,
                .height = bloom_base_extent.height,
                .depth_or_layers = 1,
            },
            .mip_levels = static_cast<u32>(bloom_extents.size()),
            .initial_layout = RHI::TextureLayout::Undefined,
            .initial_stage = RHI::PipelineStage::None,
            .initial_access = RHI::AccessFlags::None,
            .label = "persistent bloom mip chain",
        });

        for (usize level = 0; level < bloom_extents.size(); ++level) {
            const RenderGraphTextureHandle source = level == 0 ? scene_source : bloom_chain;
            const RHI::TextureViewHandle mip_source_view = level == 0
                ? RHI::TextureViewHandle{}
                : frame_slot.bloom_targets.views[level - 1];
            const RHI::TextureSubresourceRange source_subresources = level == 0
                ? RHI::TextureSubresourceRange{}
                : RHI::TextureSubresourceRange{
                      .base_mip_level = static_cast<u32>(level - 1),
                      .mip_level_count = 1,
                  };
            const RHI::TextureSubresourceRange destination_subresources{
                .base_mip_level = static_cast<u32>(level),
                .mip_level_count = 1,
            };
            const Core::Extent2D source_extent = level == 0
                ? context.render_extent
                : bloom_extents[level - 1];
            const Core::Extent2D destination_extent = bloom_extents[level];
            const RHI::BindGroupHandle cached_bind_group =
                frame_slot.bloom_targets.downsample_bind_groups[level];

            context.graph.add_render_pass("bloom downsample")
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = bloom_chain,
                    .view = frame_slot.bloom_targets.views[level],
                    .subresources = destination_subresources,
                    .load_op = RHI::LoadOp::DontCare,
                    .store_op = RHI::StoreOp::Store,
                })
                .add_sampled_texture(RenderGraphSampledTextureReadDesc{
                    .texture = source,
                    .subresources = source_subresources,
                })
                .set_render_area(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = destination_extent.width,
                    .height = destination_extent.height,
                })
                .set_execute([this, &submission, scene_source, mip_source_view, source_extent,
                              destination_extent, level, cached_bind_group](
                                 RenderGraphContext &graph_context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = graph_context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .width = static_cast<f32>(destination_extent.width),
                        .height = static_cast<f32>(destination_extent.height),
                        .min_depth = 0.0f,
                        .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{
                        .x = 0,
                        .y = 0,
                        .width = destination_extent.width,
                        .height = destination_extent.height,
                    });
                    RHI::TextureViewHandle source_view = mip_source_view;
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
                            1.0f / static_cast<f32>(source_extent.width),
                            1.0f / static_cast<f32>(source_extent.height),
                        },
                        submission.render_graph,
                        level == 0,
                        bind_group);
                });
        }

        for (usize level = bloom_extents.size(); level-- > 1;) {
            const RHI::TextureSubresourceRange source_subresources{
                .base_mip_level = static_cast<u32>(level),
                .mip_level_count = 1,
            };
            const RHI::TextureSubresourceRange destination_subresources{
                .base_mip_level = static_cast<u32>(level - 1),
                .mip_level_count = 1,
            };
            const Core::Extent2D source_extent = bloom_extents[level];
            const Core::Extent2D destination_extent = bloom_extents[level - 1];
            const RHI::TextureViewHandle source_view = frame_slot.bloom_targets.views[level];
            const RHI::BindGroupHandle bind_group = frame_slot.bloom_targets.upsample_bind_groups[level];

            context.graph.add_render_pass("bloom upsample")
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = bloom_chain,
                    .view = frame_slot.bloom_targets.views[level - 1],
                    .subresources = destination_subresources,
                    .load_op = RHI::LoadOp::Load,
                    .store_op = RHI::StoreOp::Store,
                })
                .add_sampled_texture(RenderGraphSampledTextureReadDesc{
                    .texture = bloom_chain,
                    .subresources = source_subresources,
                })
                .set_render_area(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = destination_extent.width,
                    .height = destination_extent.height,
                })
                .set_execute([this, &submission, source_view, source_extent,
                              destination_extent, bind_group](RenderGraphContext &graph_context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = graph_context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .width = static_cast<f32>(destination_extent.width),
                        .height = static_cast<f32>(destination_extent.height),
                        .min_depth = 0.0f,
                        .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{
                        .x = 0,
                        .y = 0,
                        .width = destination_extent.width,
                        .height = destination_extent.height,
                    });
                    return record_bloom_upsample(
                        pass,
                        source_view,
                        glm::vec2{
                            1.0f / static_cast<f32>(source_extent.width),
                            1.0f / static_cast<f32>(source_extent.height),
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
                .initial_layout = RHI::TextureLayout::Undefined,
                .initial_stage = RHI::PipelineStage::None,
                .initial_access = RHI::AccessFlags::None,
                .label = "bloom composite target",
            });
        const RHI::TextureSubresourceRange bloom_mip0{
            .base_mip_level = 0,
            .mip_level_count = 1,
        };
        context.graph.add_render_pass("bloom composite")
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = composite_destination,
                .load_op = RHI::LoadOp::DontCare,
                .store_op = RHI::StoreOp::Store,
            })
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = scene_source})
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{
                .texture = bloom_chain,
                .subresources = bloom_mip0,
            })
            .set_render_area(RHI::Rect2D{
                .x = 0,
                .y = 0,
                .width = context.render_extent.width,
                .height = context.render_extent.height,
            })
            .set_execute([this, &submission, scene_source, bloom_chain,
                          extent = context.render_extent](RenderGraphContext &graph_context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = graph_context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .width = static_cast<f32>(extent.width),
                    .height = static_cast<f32>(extent.height),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = extent.width,
                    .height = extent.height,
                });
                return record_bloom_composite(
                    pass,
                    graph_context.texture(scene_source).default_view,
                    graph_context.texture(bloom_chain).default_view,
                    submission.deferred_formats.scene_color,
                    submission.render_graph.bloom_intensity,
                    submission.transient_bind_groups);
            });

        context.resources.publish_texture<SceneHdrColor>(composite_destination);
        return {};
    }

    Core::RendererResult Renderer::build_tonemap_module(
        RenderGraphModuleBuildContext &context,
        FrameSubmission &submission,
        RHI::Format presentation_format,
        bool hdr_output) {
        using namespace RenderGraphSemantics;

        const RenderGraphTextureHandle source = context.resources.texture<SceneHdrColor>();
        const RenderGraphTextureHandle destination = context.resources.texture<PresentationTarget>();
        if (!source) {
            return missing_module_texture("tone mapping", "SceneHdrColor");
        }
        if (!destination) {
            return missing_module_texture("tone mapping", "PresentationTarget");
        }

        submission.render_graph.tone_mapping_hdr_output = hdr_output;
        context.graph.add_render_pass(submission.render_graph.tone_mapping ? "tonemap" : "present scene color")
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
                .width = context.presentation_extent.width,
                .height = context.presentation_extent.height,
            })
            .set_execute([this, &submission, source, presentation_format,
                          extent = context.presentation_extent](RenderGraphContext &graph_context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = graph_context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .x = 0.0f,
                    .y = 0.0f,
                    .width = static_cast<f32>(extent.width),
                    .height = static_cast<f32>(extent.height),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = extent.width,
                    .height = extent.height,
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
