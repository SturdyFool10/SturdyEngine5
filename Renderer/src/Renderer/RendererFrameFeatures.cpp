#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <expected>
#include <span>
#include <vector>

#include <Renderer/AntiAliasing.hpp>
#include <Renderer/Bloom.hpp>
#include <Renderer/FramePipeline.hpp>
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

    // The engine's own frame features. They are registered in the same FramePipeline, under the same
    // kind of name, as anything an application adds -- see FramePipeline.hpp for how to replace one.

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

    void Renderer::register_builtin_frame_features() {
        auto pipeline = frame_pipeline_.lock();
        (void)pipeline->add("motion_blur", [this](FrameBuildContext &context) { return build_frame_feature_motion_blur(context); });
        (void)pipeline->add("post_process_aa", [this](FrameBuildContext &context) { return build_frame_feature_post_process_aa(context); });
        (void)pipeline->add("effects_before_bloom", [this](FrameBuildContext &context) { return build_frame_feature_effects_before_bloom(context); });
        (void)pipeline->add("bloom", [this](FrameBuildContext &context) { return build_frame_feature_bloom(context); });
        (void)pipeline->add("effects_after_bloom", [this](FrameBuildContext &context) { return build_frame_feature_effects_after_bloom(context); });
        (void)pipeline->add("tone_mapping", [this](FrameBuildContext &context) { return build_frame_feature_tone_mapping(context); });
        (void)pipeline->add("overlay_passes", [](FrameBuildContext &context) {
            return add_overlay_passes(context, std::span<const OverlayPass>{context.settings.overlay_passes});
        });
    }

} // namespace SFT::Renderer
