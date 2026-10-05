#include <Renderer/CameraEmulation.hpp>

#include <algorithm>
#include <cmath>
#include <span>

#include <Renderer/RendererModule.hpp>
#include <Renderer/TemporalUpscaler.hpp>

namespace SFT::Renderer {

    namespace {

        // Field order and types must match `CameraEmulationConstants` in Shaders/camera_emulation.slang.
        struct CameraEmulationConstants {
            f32 fisheye = 0.0f;
            f32 chromatic_aberration = 0.0f;
            f32 vignette = 0.0f;
            f32 sensor_noise = 0.0f;

            f32 sharpen = 0.0f;
            f32 saturation = 1.0f;
            f32 contrast = 1.0f;
            f32 housing = 0.0f;

            f32 tint_r = 1.0f;
            f32 tint_g = 1.0f;
            f32 tint_b = 1.0f;
            u32 frame_index = 0;

            f32 aspect = 1.0f;
            f32 overscan = 1.0f;
            f32 pad1 = 0.0f;
            f32 pad2 = 0.0f;
        };
        static_assert(sizeof(CameraEmulationConstants) == 64);

    } // namespace

    CustomPostProcessEffect camera_emulation_effect(const CameraEmulationSettings &settings, u32 frame_index, f32 aspect) {
        const CameraEmulationConstants constants{
            // With the vertex-warp lens the camera passes already produced the distorted image.
            .fisheye = settings.lens_strength > 0.0f ? 0.0f : std::clamp(settings.fisheye_strength, 0.0f, 1.0f),
            .chromatic_aberration = std::max(settings.chromatic_aberration, 0.0f),
            .vignette = std::clamp(settings.vignette_strength, 0.0f, 1.0f),
            .sensor_noise = std::max(settings.sensor_noise, 0.0f),
            .sharpen = std::max(settings.sharpen, 0.0f),
            .saturation = std::max(settings.saturation, 0.0f),
            .contrast = std::max(settings.contrast, 0.0f),
            .housing = std::clamp(settings.housing, 0.0f, 1.0f),
            .tint_r = settings.tint[0],
            .tint_g = settings.tint[1],
            .tint_b = settings.tint[2],
            .frame_index = frame_index,
            .aspect = std::max(aspect, 0.01f),
            .overscan = std::max(settings.overscan, 1.0f),
        };
        CustomPostProcessEffect effect;
        effect.shader_path = "Shaders/camera_emulation.slang";
        effect.module_name = "camera_emulation";
        effect.fragment_entry_point = "fragmentMain";
        const std::span<const std::byte> bytes = std::as_bytes(std::span<const CameraEmulationConstants>{&constants, 1});
        effect.push_constants.assign(bytes.begin(), bytes.end());
        effect.label = UString{"camera emulation"};
        return effect;
    }

    Core::RendererResult add_camera_emulation_pass(FrameBuildContext &frame, RenderGraphTextureHandle source,
                                                   RenderGraphTextureHandle destination, Core::Extent2D extent,
                                                   RHI::Format format, const RenderGraphSettings &settings) {
        if (!source || !destination) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Camera emulation needs a source and a destination texture.");
        }
        CustomPostProcessEffect effect = camera_emulation_effect(
            settings.frame.camera_emulation, static_cast<u32>(frame.frame_index),
            static_cast<f32>(extent.x) / static_cast<f32>(std::max(extent.y, 1u)));
        if (Core::RendererResult ready = frame.renderer.prepare_fullscreen_effect(effect, format); !ready.has_value()) {
            return ready;
        }
        frame.graph.add_render_pass("camera emulation"_ustr)
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = destination,
                .load_op = RHI::LoadOp::DontCare,
                .store_op = RHI::StoreOp::Store,
            })
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = source})
            .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = extent.x, .height = extent.y})
            .set_execute([&renderer = frame.renderer, &bind_groups = frame.transient_bind_groups, source, format, extent,
                          effect = std::move(effect)](RenderGraphContext &context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = context.render_pass();
                pass.set_viewport(RHI::Viewport{
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

    Core::RendererResult build_camera_emulation_feature(FrameBuildContext &frame) {
        if (!frame.settings.frame.camera_emulation.enabled || frame.direct_overlay_presentation) {
            return {};
        }
        const RenderGraphTextureHandle source = frame.resources.texture<RenderGraphSemantics::SceneHdrColor>();
        if (!source) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Camera emulation needs the SceneHdrColor texture, but no earlier feature published it.");
        }
        // The frame was rendered `overscan` times wider than what is shown; this pass maps it back down to the
        // un-overscanned size, which is the resolution the centre of the image keeps 1:1.
        const f32 overscan = std::max(frame.settings.frame.camera_emulation.overscan, 1.0f);
        // After a temporal upscaler the scene colour is already at the output size; only a render-sized (overscanned)
        // image is mapped back down.
        const Core::Extent2D input_extent = scene_color_extent(frame);
        const bool render_sized = input_extent == frame.module.render_extent;
        const Core::Extent2D output_extent =
            render_sized ? Core::Extent2D{
                               std::max<u32>(1u, static_cast<u32>(std::lround(static_cast<f64>(input_extent.x) / overscan))),
                               std::max<u32>(1u, static_cast<u32>(std::lround(static_cast<f64>(input_extent.y) / overscan)))}
                         : input_extent;
        const RenderGraphTextureHandle destination = frame.graph.create_texture(RenderGraphTextureDesc{
            .format = frame.deferred_formats.scene_color,
            .extent = RHI::Extent3D{.width = output_extent.x, .height = output_extent.y, .depth_or_layers = 1},
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled | RHI::TextureUsage::Storage |
                     RHI::TextureUsage::TransferSrc | RHI::TextureUsage::TransferDst,
            .label = "camera emulation target",
        });
        if (Core::RendererResult added = add_camera_emulation_pass(frame, source, destination, output_extent,
                                                                    frame.deferred_formats.scene_color, frame.settings);
            !added.has_value()) {
            return added;
        }
        frame.resources.publish_texture<RenderGraphSemantics::SceneHdrColor>(destination);
        return {};
    }

} // namespace SFT::Renderer
