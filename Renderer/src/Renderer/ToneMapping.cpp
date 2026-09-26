#include <Renderer/ToneMapping.hpp>

#include <span>
#include <vector>

#include <Renderer/RendererModule.hpp>

namespace SFT::Renderer {

    namespace {

        // Field order and types must match `TonemapConstants` in Shaders/fullscreen_tonemap.slang: every
        // member is a plain 4-byte scalar so the C++ struct and the shader's push-constant block agree
        // byte for byte.
        struct TonemapConstants {
            f32 exposure = 1.0f;
            f32 white_point = 1.0f;
            f32 saturation = 1.0f;
            u32 operation = 0;

            u32 hdr_output = 0;

            u32 hdr_color_space = 0;
            f32 hdr_paper_white_nits = 203.0f;
            f32 hdr_peak_nits = 1000.0f;
            u32 preserve_alpha = 0;

            u32 agx_look = 0;

            f32 hermite_toe_strength = 0.5f;
            f32 hermite_toe_length = 0.5f;
            f32 hermite_shoulder_strength = 2.0f;
            f32 hermite_shoulder_length = 0.5f;
            f32 hermite_shoulder_angle = 1.0f;

            f32 psychov_highlights = 1.0f;
            f32 psychov_shadows = 1.0f;
            f32 psychov_contrast = 1.0f;
            f32 psychov_purity_scale = 1.0f;
            f32 psychov_gamut_compression = 1.0f;
            u32 psychov_gamut_compression_mode = 1;
            f32 psychov_compression = 0.0f;
            f32 psychov_adapted_gray_r = 0.18f;
            f32 psychov_adapted_gray_g = 0.18f;
            f32 psychov_adapted_gray_b = 0.18f;
            f32 psychov_background_gray_r = 0.18f;
            f32 psychov_background_gray_g = 0.18f;
            f32 psychov_background_gray_b = 0.18f;
        };
        static_assert(sizeof(TonemapConstants) == 112);

    } // namespace

    CustomPostProcessEffect tone_mapping_effect(const RenderGraphSettings &settings, bool preserve_alpha) {
        const ToneMappingOperator operation = settings.tone_mapping ? settings.tone_mapping_operator : ToneMappingOperator::None;
        const TonemapConstants constants{
            .exposure = settings.tone_mapping_exposure,
            .white_point = settings.tone_mapping_white_point,
            .saturation = settings.tone_mapping_saturation,
            .operation = static_cast<u32>(operation),
            .hdr_output = static_cast<u32>(settings.tone_mapping_hdr_output),
            .hdr_color_space = static_cast<u32>(settings.tone_mapping_hdr_color_space),
            .hdr_paper_white_nits = settings.tone_mapping_hdr_paper_white_nits,
            .hdr_peak_nits = settings.tone_mapping_hdr_peak_nits,
            .preserve_alpha = preserve_alpha ? 1u : 0u,
            .agx_look = static_cast<u32>(settings.agx_look),
            .hermite_toe_strength = settings.hermite_toe_strength,
            .hermite_toe_length = settings.hermite_toe_length,
            .hermite_shoulder_strength = settings.hermite_shoulder_strength,
            .hermite_shoulder_length = settings.hermite_shoulder_length,
            .hermite_shoulder_angle = settings.hermite_shoulder_angle,
            .psychov_highlights = settings.psychov_highlights,
            .psychov_shadows = settings.psychov_shadows,
            .psychov_contrast = settings.psychov_contrast,
            .psychov_purity_scale = settings.psychov_purity_scale,
            .psychov_gamut_compression = settings.psychov_gamut_compression,
            .psychov_gamut_compression_mode = settings.psychov_gamut_compression_use_bt2020 ? 1u : 0u,
            .psychov_compression = settings.psychov_compression,
            .psychov_adapted_gray_r = settings.psychov_adapted_gray_bt709.r,
            .psychov_adapted_gray_g = settings.psychov_adapted_gray_bt709.g,
            .psychov_adapted_gray_b = settings.psychov_adapted_gray_bt709.b,
            .psychov_background_gray_r = settings.psychov_background_gray_bt709.r,
            .psychov_background_gray_g = settings.psychov_background_gray_bt709.g,
            .psychov_background_gray_b = settings.psychov_background_gray_bt709.b,
        };
        CustomPostProcessEffect effect;
        effect.shader_path = "Shaders/fullscreen_tonemap.slang";
        effect.module_name = "fullscreen_tonemap";
        effect.fragment_entry_point = "fragmentMain";
        const std::span<const std::byte> bytes = std::as_bytes(std::span<const TonemapConstants>{&constants, 1});
        effect.push_constants.assign(bytes.begin(), bytes.end());
        effect.label = UString{"tone mapping"};
        return effect;
    }

    Core::RendererResult add_tone_mapping_pass(FrameBuildContext &frame, RenderGraphTextureHandle source, RenderGraphTextureHandle destination,
                                               const RenderGraphSettings &settings, bool preserve_alpha, std::string_view label,
                                               RHI::Format target_format) {
        if (!source || !destination) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Tone mapping needs a source and a destination texture.");
        }
        const RHI::Format format = target_format == RHI::Format::Undefined ? frame.output_format : target_format;
        CustomPostProcessEffect effect = tone_mapping_effect(settings, preserve_alpha);
        if (Core::RendererResult ready = frame.renderer.prepare_fullscreen_effect(effect, format); !ready.has_value()) {
            return ready;
        }
        const Core::Extent2D extent = frame.module.presentation_extent;
        const UString pass_label{label};
        frame.graph.add_render_pass(pass_label)
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
            .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = extent.x, .height = extent.y})
            .set_execute([&renderer = frame.renderer, &bind_groups = frame.transient_bind_groups, source, format, extent,
                          effect = std::move(effect)](RenderGraphContext &context) -> Core::RendererResult {
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
                return renderer.record_fullscreen_effect(pass, context.texture(source).default_view, format, effect, bind_groups);
            });
        return {};
    }

} // namespace SFT::Renderer
