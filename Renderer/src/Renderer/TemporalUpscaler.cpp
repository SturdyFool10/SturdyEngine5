#include <glm/gtc/type_ptr.hpp>
#include <Renderer/TemporalUpscaler.hpp>

#include <algorithm>
#include <cmath>
#include <span>

#include <Renderer/RendererModule.hpp>

namespace SFT::Renderer {

    namespace {

        constexpr u32 kEffectTemporalUpscale = 3;

        // Must match UpscaleConstants in Shaders/temporal_upscale.slang.
        struct UpscaleConstants {
            glm::vec2 input_size{};
            glm::vec2 output_size{};
            glm::vec2 jitter_uv{};
            glm::vec2 previous_jitter_uv{};
            f32 current_weight = 0.1f;
            f32 sharpness = 0.5f;
            u32 has_history = 0;
            u32 pad0 = 0;
        };
        static_assert(sizeof(UpscaleConstants) == 48);

    } // namespace

    Core::Extent2D scene_color_extent(const FrameBuildContext &frame) noexcept {
        const RHI::Extent3D extent =
            frame.graph.texture_extent(frame.resources.texture<RenderGraphSemantics::SceneHdrColor>());
        if (extent.width == 0 || extent.height == 0) {
            return frame.module.render_extent;
        }
        return Core::Extent2D{extent.width, extent.height};
    }

    Core::RendererExpected<RenderGraphTextureHandle> add_temporal_upscale_pass(
        Renderer &renderer, RHI::RhiDevice &device, HistoryTextureCache &histories, RenderGraph &graph,
        std::vector<RHI::BindGroupHandle> &transient_bind_groups, const TemporalUpscaleDescription &description,
        const RenderGraphSettings &settings) {
        if (!description.color || !description.depth || !description.motion) {
            return std::unexpected(Core::GraphicsBackendError{
                Core::GraphicsBackendErrorCode::OperationFailed,
                "The temporal upscaler needs colour, depth and motion-vector render-graph textures."});
        }
        const auto kernel = renderer.prepare_compute_kernel(ComputeKernelDescription{
            .shader_path = "Shaders/temporal_upscale.slang",
            .module_name = "temporal_upscale",
            .entry_point = "upscaleMain",
            .label = "temporal_upscale",
        });
        if (!kernel) return std::unexpected(kernel.error());

        const Core::Extent2D output = description.output_extent;
        const HistoryTextureCache::Desc history_desc{
            .format = RHI::Format::RGBA16Float,
            .extent = RHI::Extent3D{.width = output.x, .height = output.y, .depth_or_layers = 1},
            .label = "temporal upscale history",
        };
        const u64 frame = description.frame_index;
        auto previous = histories.acquire(
            device, history_texture_key(description.history_key, kEffectTemporalUpscale, static_cast<u32>((frame + 1) & 1u)),
            history_desc);
        if (!previous) return std::unexpected(previous.error());
        auto current = histories.acquire(
            device, history_texture_key(description.history_key, kEffectTemporalUpscale, static_cast<u32>(frame & 1u)),
            history_desc, frame);
        if (!current) return std::unexpected(current.error());
        const RenderGraphTextureHandle history =
            import_history_texture(graph, *previous, previous->ever_written(), "temporal upscale history (previous)");
        const RenderGraphTextureHandle upscaled = import_history_texture(graph, *current, false, "temporal upscale output");

        const TemporalUpscalerSettings &taa = settings.frame.temporal_upscaler;
        const UpscaleConstants constants{
            .input_size = glm::vec2{static_cast<f32>(description.input_extent.x), static_cast<f32>(description.input_extent.y)},
            .output_size = glm::vec2{static_cast<f32>(output.x), static_cast<f32>(output.y)},
            .jitter_uv = glm::make_vec2(taa.jitter_uv),
            .previous_jitter_uv = glm::make_vec2(taa.previous_jitter_uv),
            .current_weight = std::clamp(std::isfinite(taa.current_frame_weight) ? taa.current_frame_weight : 0.1f, 0.02f, 1.0f),
            .sharpness = std::clamp(std::isfinite(taa.sharpness) ? taa.sharpness : 0.5f, 0.0f, 1.0f),
            .has_history = previous->written_in_previous_frame(frame) ? 1u : 0u,
        };

        graph.add_compute_pass("temporal upscale"_ustr)
            .add_sampled_texture(description.color)
            .add_sampled_texture(description.depth)
            .add_sampled_texture(description.motion)
            .add_sampled_texture(history)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = upscaled, .read = false, .write = true})
            .set_execute([&renderer, &transient_bind_groups, kernel = *kernel, color = description.color,
                          depth = description.depth, motion = description.motion, history, upscaled, output,
                          constants](RenderGraphComputeContext &context) -> Core::RendererResult {
                const ComputeBinding bindings[] = {
                    {"sceneColor", context.texture(color).default_view},
                    {"sceneDepth", context.texture(depth).default_view},
                    {"gbufferMotion", context.texture(motion).default_view},
                    {"historyColor", context.texture(history).default_view},
                    {"upscaledOut", context.texture(upscaled).default_view},
                };
                return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings,
                                                      std::as_bytes(std::span<const UpscaleConstants>{&constants, 1}),
                                                      glm::uvec3{(output.x + 7u) / 8u, (output.y + 7u) / 8u, 1u},
                                                      transient_bind_groups);
            });
        return upscaled;
    }

    Core::RendererResult build_temporal_upscale_feature(FrameBuildContext &frame) {
        if (!frame.settings.frame.temporal_upscaler.enabled || !frame.settings.render_scene || frame.direct_overlay_presentation) {
            return {};
        }
        // The jitter only exists for perspective cameras (see the engine's frame preparation).
        if (std::abs(frame.camera.projection[3][3]) > 0.5f) {
            return {};
        }
        const RenderGraphTextureHandle color = frame.resources.texture<RenderGraphSemantics::SceneHdrColor>();
        const RenderGraphTextureHandle depth = frame.resources.texture<RenderGraphSemantics::ResolvedSceneDepth>();
        const RenderGraphTextureHandle motion = frame.resources.texture<RenderGraphSemantics::GBufferMotion>();
        if (!color || !depth || !motion) {
            return {};
        }
        auto upscaled = add_temporal_upscale_pass(
            frame.renderer, frame.device, frame.renderer.history_textures(), frame.graph, frame.transient_bind_groups,
            TemporalUpscaleDescription{
                .color = color,
                .depth = depth,
                .motion = motion,
                .input_extent = scene_color_extent(frame),
                .output_extent = frame.module.presentation_extent,
                .frame_index = frame.frame_index,
                .history_key = static_cast<u64>(frame.surface.window_id),
            },
            frame.settings);
        if (!upscaled.has_value()) {
            return std::unexpected(upscaled.error());
        }
        frame.resources.publish_texture<RenderGraphSemantics::SceneHdrColor>(*upscaled);
        return {};
    }

} // namespace SFT::Renderer
