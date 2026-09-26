#include <Renderer/Overlay.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <Renderer/RendererModule.hpp>
#include <Renderer/ToneMapping.hpp>

namespace SFT::Renderer {

    Core::RendererResult add_overlay_passes(FrameBuildContext &frame, std::span<const OverlayPass> overlays) {
        if (std::ranges::none_of(overlays, [](const OverlayPass &overlay) { return static_cast<bool>(overlay.draw); })) {
            return {};
        }
        const Core::Extent2D extent = frame.module.presentation_extent;

        // HDR output (or an overlay-only frame on a transparent surface): draw into a linear intermediate, then encode
        // it to the display at the reference white level and composite it over the frame.
        RenderGraphTextureHandle target = frame.final_output;
        RHI::Format format = frame.output_format;
        if (frame.overlay_display_transform) {
            format = RHI::Format::RGBA16Float;
            target = frame.graph.create_texture(RenderGraphTextureDesc{
                .format = format,
                .extent = RHI::Extent3D{.width = extent.x, .height = extent.y, .depth_or_layers = 1},
                .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled,
                .label = "linear overlay composition",
            });
        }

        bool first = true;
        for (const OverlayPass &overlay : overlays) {
            if (!overlay.draw) {
                continue;
            }
            std::vector<RenderGraphTextureHandle> sampled_textures;
            if (overlay.prepare) {
                if (frame.encoder == nullptr || frame.transient_buffers == nullptr || frame.retired_text_atlas_resources == nullptr) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Overlay prepare needs the frame's encoder and transient resource lists.");
                }
                OverlayPrepareContext prepare{
                    .device = frame.device,
                    .encoder = *frame.encoder,
                    .graph = frame.graph,
                    .viewport = glm::vec2{extent},
                    .surface = frame.surface,
                    .frame_slot_index = frame.frame_slot_index,
                    .transient_buffers = *frame.transient_buffers,
                    .retired_text_atlas_resources = *frame.retired_text_atlas_resources,
                    .transient_bind_groups = frame.transient_bind_groups,
                    .sampled_textures = sampled_textures,
                };
                if (Core::RendererResult prepared = overlay.prepare(prepare); !prepared.has_value()) {
                    return prepared;
                }
            }

            const UString label{overlay.name.empty() ? std::string{"overlay"} : overlay.name};
            auto &builder = frame.graph.add_render_pass(label);
            builder.add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = target,
                // Over a real frame the overlay loads it; over nothing (overlay-only) the first one clears.
                .load_op = first && (frame.direct_overlay_presentation || frame.overlay_display_transform) ? RHI::LoadOp::Clear
                                                                                                           : RHI::LoadOp::Load,
                .store_op = RHI::StoreOp::Store,
                // The linear layer over a scene starts empty; over nothing, the frame's own clear colour.
                .clear_color = frame.direct_overlay_presentation ? frame.overlay_clear_color : RHI::ClearColor{0.0f, 0.0f, 0.0f, 0.0f},
            });
            for (const RenderGraphTextureHandle sampled : sampled_textures) {
                builder.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = sampled});
            }
            builder
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = extent.x, .height = extent.y})
                .set_execute([draw = overlay.draw, extent, format, hdr_output = frame.hdr_output, surface = frame.surface,
                              slot = frame.frame_slot_index](RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .x = 0.0f,
                        .y = 0.0f,
                        .width = static_cast<f32>(extent.x),
                        .height = static_cast<f32>(extent.y),
                        .min_depth = 0.0f,
                        .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = extent.x, .height = extent.y});
                    OverlayPassContext overlay_context{
                        .pass = pass, .extent = extent, .format = format, .hdr_output = hdr_output, .surface = surface,
                        .frame_slot_index = slot};
                    return draw(overlay_context);
                });
            first = false;
        }

        if (frame.overlay_display_transform) {
            RenderGraphSettings display;
            display.tone_mapping = false;
            display.tone_mapping_exposure = 1.0f;
            display.tone_mapping_white_point = 1.0f;
            display.tone_mapping_saturation = 1.0f;
            display.tone_mapping_hdr_output = frame.hdr_output;
            display.tone_mapping_hdr_color_space = frame.hdr_color_space;
            display.tone_mapping_hdr_paper_white_nits = frame.overlay_reference_white_nits;
            display.tone_mapping_hdr_peak_nits = frame.settings.tone_mapping_hdr_peak_nits;
            // Over a scene the encoded layer is composited onto the frame; over nothing it is the frame.
            return add_tone_mapping_pass(frame, target, frame.final_output, display, true, "overlay display encode",
                                         RHI::Format::Undefined, /*composite_over=*/!frame.direct_overlay_presentation);
        }
        return {};
    }

} // namespace SFT::Renderer
