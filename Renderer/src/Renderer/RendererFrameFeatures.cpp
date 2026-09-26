#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <expected>
#include <span>
#include <vector>

#include <Renderer/FramePipeline.hpp>
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
        [[maybe_unused]] const RenderGraphTextureHandle ui_overlay_target = state.ui_overlay_target;
        [[maybe_unused]] const bool bloom_active = state.bloom_active;
        [[maybe_unused]] const RHI::Format bloom_format = state.bloom_format;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &logical_graph_textures = *state.logical_graph_textures;
        [[maybe_unused]] const auto &map_logical_texture = state.map_logical_texture;
        [[maybe_unused]] vector<TextDrawBatch> &text_overlay_batches = *state.text_overlay_batches;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &ui_glow_bloom_outputs = *state.ui_glow_bloom_outputs;
        [[maybe_unused]] const bool direct_overlay_display_transform = state.direct_overlay_display_transform;
        [[maybe_unused]] const f32 ui_reference_white_nits = state.ui_reference_white_nits;
        [[maybe_unused]] const u32 frame_slot_index = state.frame_slot_index;
        [[maybe_unused]] const glm::vec4 background = state.background;

        if (Core::RendererResult motion_blurred = build_motion_blur_module(
                module_context, submission, gbuffer_motion, depth_texture);
            !motion_blurred.has_value()) {
            return motion_blurred;
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
        [[maybe_unused]] const RenderGraphTextureHandle ui_overlay_target = state.ui_overlay_target;
        [[maybe_unused]] const bool bloom_active = state.bloom_active;
        [[maybe_unused]] const RHI::Format bloom_format = state.bloom_format;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &logical_graph_textures = *state.logical_graph_textures;
        [[maybe_unused]] const auto &map_logical_texture = state.map_logical_texture;
        [[maybe_unused]] vector<TextDrawBatch> &text_overlay_batches = *state.text_overlay_batches;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &ui_glow_bloom_outputs = *state.ui_glow_bloom_outputs;
        [[maybe_unused]] const bool direct_overlay_display_transform = state.direct_overlay_display_transform;
        [[maybe_unused]] const f32 ui_reference_white_nits = state.ui_reference_white_nits;
        [[maybe_unused]] const u32 frame_slot_index = state.frame_slot_index;
        [[maybe_unused]] const glm::vec4 background = state.background;

        if (Core::RendererResult anti_aliased = build_post_process_aa_module(module_context, submission);
            !anti_aliased.has_value()) {
            return anti_aliased;
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
        [[maybe_unused]] const RenderGraphTextureHandle ui_overlay_target = state.ui_overlay_target;
        [[maybe_unused]] const bool bloom_active = state.bloom_active;
        [[maybe_unused]] const RHI::Format bloom_format = state.bloom_format;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &logical_graph_textures = *state.logical_graph_textures;
        [[maybe_unused]] const auto &map_logical_texture = state.map_logical_texture;
        [[maybe_unused]] vector<TextDrawBatch> &text_overlay_batches = *state.text_overlay_batches;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &ui_glow_bloom_outputs = *state.ui_glow_bloom_outputs;
        [[maybe_unused]] const bool direct_overlay_display_transform = state.direct_overlay_display_transform;
        [[maybe_unused]] const f32 ui_reference_white_nits = state.ui_reference_white_nits;
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
        [[maybe_unused]] const RenderGraphTextureHandle ui_overlay_target = state.ui_overlay_target;
        [[maybe_unused]] const bool bloom_active = state.bloom_active;
        [[maybe_unused]] const RHI::Format bloom_format = state.bloom_format;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &logical_graph_textures = *state.logical_graph_textures;
        [[maybe_unused]] const auto &map_logical_texture = state.map_logical_texture;
        [[maybe_unused]] vector<TextDrawBatch> &text_overlay_batches = *state.text_overlay_batches;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &ui_glow_bloom_outputs = *state.ui_glow_bloom_outputs;
        [[maybe_unused]] const bool direct_overlay_display_transform = state.direct_overlay_display_transform;
        [[maybe_unused]] const f32 ui_reference_white_nits = state.ui_reference_white_nits;
        [[maybe_unused]] const u32 frame_slot_index = state.frame_slot_index;
        [[maybe_unused]] const glm::vec4 background = state.background;

        if (Core::RendererResult bloom = build_bloom_module(
                module_context, submission, slot, bloom_active, bloom_format);
            !bloom.has_value()) {
            return bloom;
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
        [[maybe_unused]] const RenderGraphTextureHandle ui_overlay_target = state.ui_overlay_target;
        [[maybe_unused]] const bool bloom_active = state.bloom_active;
        [[maybe_unused]] const RHI::Format bloom_format = state.bloom_format;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &logical_graph_textures = *state.logical_graph_textures;
        [[maybe_unused]] const auto &map_logical_texture = state.map_logical_texture;
        [[maybe_unused]] vector<TextDrawBatch> &text_overlay_batches = *state.text_overlay_batches;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &ui_glow_bloom_outputs = *state.ui_glow_bloom_outputs;
        [[maybe_unused]] const bool direct_overlay_display_transform = state.direct_overlay_display_transform;
        [[maybe_unused]] const f32 ui_reference_white_nits = state.ui_reference_white_nits;
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
        [[maybe_unused]] const RenderGraphTextureHandle ui_overlay_target = state.ui_overlay_target;
        [[maybe_unused]] const bool bloom_active = state.bloom_active;
        [[maybe_unused]] const RHI::Format bloom_format = state.bloom_format;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &logical_graph_textures = *state.logical_graph_textures;
        [[maybe_unused]] const auto &map_logical_texture = state.map_logical_texture;
        [[maybe_unused]] vector<TextDrawBatch> &text_overlay_batches = *state.text_overlay_batches;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &ui_glow_bloom_outputs = *state.ui_glow_bloom_outputs;
        [[maybe_unused]] const bool direct_overlay_display_transform = state.direct_overlay_display_transform;
        [[maybe_unused]] const f32 ui_reference_white_nits = state.ui_reference_white_nits;
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

    Core::RendererResult Renderer::build_frame_feature_debug_text_overlay(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::debug_text_overlay");
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
        [[maybe_unused]] const RenderGraphTextureHandle ui_overlay_target = state.ui_overlay_target;
        [[maybe_unused]] const bool bloom_active = state.bloom_active;
        [[maybe_unused]] const RHI::Format bloom_format = state.bloom_format;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &logical_graph_textures = *state.logical_graph_textures;
        [[maybe_unused]] const auto &map_logical_texture = state.map_logical_texture;
        [[maybe_unused]] vector<TextDrawBatch> &text_overlay_batches = *state.text_overlay_batches;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &ui_glow_bloom_outputs = *state.ui_glow_bloom_outputs;
        [[maybe_unused]] const bool direct_overlay_display_transform = state.direct_overlay_display_transform;
        [[maybe_unused]] const f32 ui_reference_white_nits = state.ui_reference_white_nits;
        [[maybe_unused]] const u32 frame_slot_index = state.frame_slot_index;
        [[maybe_unused]] const glm::vec4 background = state.background;

        if (submission.render_graph.debug_overlay && submission.render_graph.draw_overlay_text) {


            graph.add_render_pass("debug text overlay"_ustr)
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = final_output,
                    .load_op = RHI::LoadOp::Load,
                    .store_op = RHI::StoreOp::Store,
                })
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = presentation_extent.x, .height = presentation_extent.y})
                .set_execute([this, presentation_extent, &text_overlay_batches](RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .x = 0.0f,
                        .y = 0.0f,
                        .width = static_cast<f32>(presentation_extent.x),
                        .height = static_cast<f32>(presentation_extent.y),
                        .min_depth = 0.0f,
                        .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = presentation_extent.x, .height = presentation_extent.y});
                    const glm::vec2 viewport_size{presentation_extent};


                    return draw_text_overlay(pass, text_overlay_batches, viewport_size);
                });
        }
        return {};
    }

    Core::RendererResult Renderer::build_frame_feature_ui_overlay(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::ui_overlay");
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
        [[maybe_unused]] const RenderGraphTextureHandle ui_overlay_target = state.ui_overlay_target;
        [[maybe_unused]] const bool bloom_active = state.bloom_active;
        [[maybe_unused]] const RHI::Format bloom_format = state.bloom_format;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &logical_graph_textures = *state.logical_graph_textures;
        [[maybe_unused]] const auto &map_logical_texture = state.map_logical_texture;
        [[maybe_unused]] vector<TextDrawBatch> &text_overlay_batches = *state.text_overlay_batches;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &ui_glow_bloom_outputs = *state.ui_glow_bloom_outputs;
        [[maybe_unused]] const bool direct_overlay_display_transform = state.direct_overlay_display_transform;
        [[maybe_unused]] const f32 ui_reference_white_nits = state.ui_reference_white_nits;
        [[maybe_unused]] const u32 frame_slot_index = state.frame_slot_index;
        [[maybe_unused]] const glm::vec4 background = state.background;

        if (submission.render_graph.ui_overlay) {


            RenderGraphRenderPassBuilder &ui_pass = graph.add_render_pass("UI overlay"_ustr)
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = ui_overlay_target,
                    .load_op = direct_overlay_presentation ? RHI::LoadOp::Clear : RHI::LoadOp::Load,
                    .store_op = RHI::StoreOp::Store,
                    .clear_color = static_cast<bool>(record.presentation.transparent_composition)
                                       ? RHI::ClearColor{0.0f, 0.0f, 0.0f, 0.0f}
                                       : RHI::ClearColor{background.r, background.g, background.b, 1.0f},
                });
            // Every glow-bloom output UI::UiRenderer's own prepare() hook queued this frame (see
            // ui_glow_bloom_outputs above) must be declared as a read dependency here — otherwise the
            // graph's transient-memory aliasing has no reason to know this pass still needs that
            // texture's memory, and could reuse/corrupt it before draw() below samples it.
            for (const RenderGraphTextureHandle glow_output : ui_glow_bloom_outputs) {
                ui_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = glow_output});
            }
            ui_pass.set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = presentation_extent.x, .height = presentation_extent.y})
                .set_execute([presentation_extent, surface = record.surface, frame_slot_index,
                              &submission](RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .x = 0.0f,
                        .y = 0.0f,
                        .width = static_cast<f32>(presentation_extent.x),
                        .height = static_cast<f32>(presentation_extent.y),
                        .min_depth = 0.0f,
                        .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = presentation_extent.x, .height = presentation_extent.y});
                    const glm::vec2 viewport_size{presentation_extent};


                    return submission.render_graph.ui_overlay.draw(pass, viewport_size, surface, frame_slot_index);
                });
        }
        return {};
    }

    Core::RendererResult Renderer::build_frame_feature_ui_display_encode(FrameBuildContext &context) {
        ZoneScopedN("Renderer::frame_feature::ui_display_encode");
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
        [[maybe_unused]] const RenderGraphTextureHandle ui_overlay_target = state.ui_overlay_target;
        [[maybe_unused]] const bool bloom_active = state.bloom_active;
        [[maybe_unused]] const RHI::Format bloom_format = state.bloom_format;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &logical_graph_textures = *state.logical_graph_textures;
        [[maybe_unused]] const auto &map_logical_texture = state.map_logical_texture;
        [[maybe_unused]] vector<TextDrawBatch> &text_overlay_batches = *state.text_overlay_batches;
        [[maybe_unused]] vector<RenderGraphTextureHandle> &ui_glow_bloom_outputs = *state.ui_glow_bloom_outputs;
        [[maybe_unused]] const bool direct_overlay_display_transform = state.direct_overlay_display_transform;
        [[maybe_unused]] const f32 ui_reference_white_nits = state.ui_reference_white_nits;
        [[maybe_unused]] const u32 frame_slot_index = state.frame_slot_index;
        [[maybe_unused]] const glm::vec4 background = state.background;

        if (direct_overlay_display_transform) {
            RenderGraphSettings ui_display_settings{};
            ui_display_settings.tone_mapping = false;
            ui_display_settings.tone_mapping_exposure = 1.0f;
            ui_display_settings.tone_mapping_white_point = 1.0f;
            ui_display_settings.tone_mapping_saturation = 1.0f;
            ui_display_settings.tone_mapping_hdr_output = hdr_output;
            ui_display_settings.tone_mapping_hdr_color_space = record.presentation.hdr_color_space;
            ui_display_settings.tone_mapping_hdr_paper_white_nits = ui_reference_white_nits;
            ui_display_settings.tone_mapping_hdr_peak_nits = submission.render_graph.tone_mapping_hdr_peak_nits;

            if (Core::RendererResult encoded = add_tone_mapping_pass(
                    context, ui_overlay_target, final_output, ui_display_settings, true, "UI display encode");
                !encoded.has_value()) {
                return encoded;
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
        (void)pipeline->add("debug_text_overlay", [this](FrameBuildContext &context) { return build_frame_feature_debug_text_overlay(context); });
        (void)pipeline->add("ui_overlay", [this](FrameBuildContext &context) { return build_frame_feature_ui_overlay(context); });
        (void)pipeline->add("ui_display_encode", [this](FrameBuildContext &context) { return build_frame_feature_ui_display_encode(context); });
    }

} // namespace SFT::Renderer
