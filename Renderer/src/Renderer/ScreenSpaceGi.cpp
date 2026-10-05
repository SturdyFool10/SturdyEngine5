#include <Renderer/ScreenSpaceGi.hpp>

#include <algorithm>
#include <cmath>
#include <span>

#include <Renderer/RendererModule.hpp>

namespace SFT::Renderer {

    namespace {

        constexpr u32 kEffectRadianceHistory = 1;
        constexpr u32 kEffectAccumulation = 2;

        // Must match the push-constant blocks in Shaders/ssgi_{trace,resolve,history}.slang.
        struct TraceConstants {
            glm::vec4 ndc_to_view_mul_add{};
            glm::vec4 view_row0{};
            glm::vec4 view_row1{};
            glm::vec4 view_row2{};
            glm::vec4 depth_lens_radius{};
            glm::vec4 thickness_slices_steps_frame{};
            glm::uvec2 trace_extent{};
            glm::uvec2 full_extent{};
            u32 radiance_valid = 0;
            u32 pad[3]{};
        };
        static_assert(sizeof(TraceConstants) == 128);

        struct ResolveConstants {
            f32 depth_linearize_a = 0.0f;
            f32 depth_linearize_b = 0.0f;
            f32 temporal_alpha = 0.1f;
            u32 has_history = 0;
            glm::uvec2 trace_extent{};
            glm::uvec2 full_extent{};
        };
        static_assert(sizeof(ResolveConstants) == 32);

        struct HistoryConstants {
            f32 inverse_exposure = 1.0f;
            f32 max_luminance = 64.0f;
            glm::uvec2 output_extent{};
        };
        static_assert(sizeof(HistoryConstants) == 16);

        template <class Constants>
        [[nodiscard]] std::span<const std::byte> bytes_of(const Constants &constants) {
            return std::as_bytes(std::span<const Constants>{&constants, 1});
        }

        [[nodiscard]] ComputeKernelDescription kernel_description(std::string_view module, std::string_view entry) {
            return ComputeKernelDescription{
                .shader_path = "Shaders/" + std::string{module} + ".slang",
                .module_name = std::string{module},
                .entry_point = std::string{entry},
                .label = std::string{module},
            };
        }

        [[nodiscard]] Core::Extent2D half_extent(Core::Extent2D extent) noexcept {
            return Core::Extent2D{std::max((extent.x + 1u) / 2u, 1u), std::max((extent.y + 1u) / 2u, 1u)};
        }

        [[nodiscard]] f32 finite_or(f32 value, f32 fallback) noexcept { return std::isfinite(value) ? value : fallback; }

    } // namespace

    bool screen_space_gi_active(const RenderGraphSettings &settings, const CameraView &camera) noexcept {
        const bool orthographic = std::abs(camera.projection[3][3]) > 0.5f;
        return settings.frame.screen_space_gi.enabled && !settings.frame.restir_gi.enabled && settings.render_scene &&
               settings.spectral_path_tracing.mode != SpectralRenderMode::FullPathTracing && !orthographic;
    }

    Core::RendererExpected<ScreenSpaceGiResult> add_screen_space_gi_passes(
        Renderer &renderer, RHI::RhiDevice &device, HistoryTextureCache &histories, RenderGraph &graph,
        std::vector<RHI::BindGroupHandle> &transient_bind_groups, const ScreenSpaceGiDescription &description,
        const RenderGraphSettings &settings) {
        if (!description.depth || !description.normal || !description.motion) {
            return std::unexpected(Core::GraphicsBackendError{
                Core::GraphicsBackendErrorCode::OperationFailed,
                "Screen-space GI requires depth, normal and motion-vector render-graph textures."});
        }
        const ScreenSpaceGiSettings &gi = settings.frame.screen_space_gi;
        const auto trace_kernel = renderer.prepare_compute_kernel(kernel_description("ssgi_trace", "traceMain"));
        if (!trace_kernel) return std::unexpected(trace_kernel.error());
        const auto resolve_kernel = renderer.prepare_compute_kernel(kernel_description("ssgi_resolve", "resolveMain"));
        if (!resolve_kernel) return std::unexpected(resolve_kernel.error());

        const Core::Extent2D full = description.extent;
        const Core::Extent2D half = half_extent(full);
        const u64 frame = description.frame_index;

        // Last frame's radiance (written by the history pass at the end of the previous frame).
        auto radiance = histories.acquire(
            device, history_texture_key(description.history_key, kEffectRadianceHistory),
            HistoryTextureCache::Desc{.extent = RHI::Extent3D{.width = half.x, .height = half.y, .depth_or_layers = 1},
                                      .label = "ssgi radiance history"});
        if (!radiance) return std::unexpected(radiance.error());
        const bool radiance_valid = radiance->written_in_previous_frame(frame);
        const RenderGraphTextureHandle radiance_history =
            import_history_texture(graph, *radiance, radiance->ever_written(), "ssgi radiance history");

        // Accumulation ping-pong: write this frame's slot, read the other.
        const HistoryTextureCache::Desc accumulation_desc{
            .extent = RHI::Extent3D{.width = full.x, .height = full.y, .depth_or_layers = 1},
            .label = "ssgi accumulation"};
        auto previous = histories.acquire(
            device, history_texture_key(description.history_key, kEffectAccumulation, static_cast<u32>((frame + 1) & 1u)),
            accumulation_desc);
        if (!previous) return std::unexpected(previous.error());
        auto current = histories.acquire(
            device, history_texture_key(description.history_key, kEffectAccumulation, static_cast<u32>(frame & 1u)),
            accumulation_desc, frame);
        if (!current) return std::unexpected(current.error());
        const bool has_history = previous->written_in_previous_frame(frame);
        const RenderGraphTextureHandle accumulated_previous =
            import_history_texture(graph, *previous, previous->ever_written(), "ssgi accumulation (previous)");
        const RenderGraphTextureHandle accumulated_current =
            import_history_texture(graph, *current, false, "ssgi accumulation");

        const RenderGraphTextureHandle raw = graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::RGBA16Float,
            .extent = RHI::Extent3D{.width = half.x, .height = half.y, .depth_or_layers = 1},
            .usage = RHI::TextureUsage::Storage | RHI::TextureUsage::Sampled,
            .label = "ssgi raw irradiance",
        });

        const glm::mat4 &projection = description.camera.projection;
        const f32 focal_x = std::abs(projection[0][0]) > 1.0e-6f ? projection[0][0] : 1.0f;
        const f32 focal_y = std::abs(projection[1][1]) > 1.0e-6f ? projection[1][1] : 1.0f;
        const f32 near_plane = std::max(finite_or(description.camera.near_plane, 0.01f), 1.0e-4f);
        const f32 far_plane = std::max(finite_or(description.camera.far_plane, 1000.0f), near_plane * 1.001f);
        const f32 depth_a = near_plane * far_plane / (far_plane - near_plane);
        const f32 depth_b = far_plane / (far_plane - near_plane);
        const glm::mat4 &view = description.camera.view;

        const TraceConstants trace_constants{
            // Same derivation as GTAO's (RendererGtao.cpp): screen UV -> view ray, y flipped (uv runs down).
            .ndc_to_view_mul_add = glm::vec4{2.0f / focal_x, -2.0f / focal_y, -1.0f / focal_x, 1.0f / focal_y},
            .view_row0 = glm::vec4{view[0][0], view[1][0], view[2][0], 0.0f},
            .view_row1 = glm::vec4{view[0][1], view[1][1], view[2][1], 0.0f},
            .view_row2 = glm::vec4{view[0][2], view[1][2], view[2][2], 0.0f},
            .depth_lens_radius = glm::vec4{depth_a, depth_b, std::max(description.lens_strength, 0.0f),
                                           std::max(finite_or(gi.radius, 2.0f), 1.0e-3f)},
            .thickness_slices_steps_frame = glm::vec4{std::max(finite_or(gi.thickness, 0.25f), 0.0f),
                                                      static_cast<f32>(std::clamp(gi.slice_count, 1u, 8u)),
                                                      static_cast<f32>(std::clamp(gi.step_count, 1u, 32u)),
                                                      static_cast<f32>(frame % 65536u)},
            .trace_extent = glm::uvec2{half.x, half.y},
            .full_extent = glm::uvec2{full.x, full.y},
            .radiance_valid = radiance_valid ? 1u : 0u,
        };
        const ResolveConstants resolve_constants{
            .depth_linearize_a = depth_a,
            .depth_linearize_b = depth_b,
            .temporal_alpha = std::clamp(finite_or(gi.temporal_alpha, 0.1f), 0.01f, 1.0f),
            .has_history = has_history ? 1u : 0u,
            .trace_extent = glm::uvec2{half.x, half.y},
            .full_extent = glm::uvec2{full.x, full.y},
        };

        graph.add_compute_pass("ssgi trace"_ustr)
            .add_sampled_texture(description.depth)
            .add_sampled_texture(description.normal)
            .add_sampled_texture(description.motion)
            .add_sampled_texture(radiance_history)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = raw, .read = false, .write = true})
            .set_execute([&renderer, &transient_bind_groups, kernel = *trace_kernel, depth = description.depth,
                          normal = description.normal, motion = description.motion, radiance_history, raw, half,
                          trace_constants](RenderGraphComputeContext &context) -> Core::RendererResult {
                const ComputeBinding bindings[] = {
                    {"sceneDepth", context.texture(depth).default_view},
                    {"gbufferNormal", context.texture(normal).default_view},
                    {"gbufferMotion", context.texture(motion).default_view},
                    {"radianceHistory", context.texture(radiance_history).default_view},
                    {"rawIrradianceOut", context.texture(raw).default_view},
                };
                return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, bytes_of(trace_constants),
                                                      glm::uvec3{(half.x + 7u) / 8u, (half.y + 7u) / 8u, 1u},
                                                      transient_bind_groups);
            });

        graph.add_compute_pass("ssgi resolve"_ustr)
            .add_sampled_texture(description.depth)
            .add_sampled_texture(description.motion)
            .add_sampled_texture(raw)
            .add_sampled_texture(accumulated_previous)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = accumulated_current, .read = false, .write = true})
            .set_execute([&renderer, &transient_bind_groups, kernel = *resolve_kernel, depth = description.depth,
                          motion = description.motion, raw, accumulated_previous, accumulated_current, full,
                          resolve_constants](RenderGraphComputeContext &context) -> Core::RendererResult {
                const ComputeBinding bindings[] = {
                    {"sceneDepth", context.texture(depth).default_view},
                    {"gbufferMotion", context.texture(motion).default_view},
                    {"rawIrradiance", context.texture(raw).default_view},
                    {"accumulatedPrevious", context.texture(accumulated_previous).default_view},
                    {"accumulatedOut", context.texture(accumulated_current).default_view},
                };
                return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, bytes_of(resolve_constants),
                                                      glm::uvec3{(full.x + 7u) / 8u, (full.y + 7u) / 8u, 1u},
                                                      transient_bind_groups);
            });

        return ScreenSpaceGiResult{.irradiance = accumulated_current, .radiance_history = radiance_history};
    }

    Core::RendererResult add_screen_space_gi_history_pass(
        Renderer &renderer, RHI::RhiDevice &device, HistoryTextureCache &histories, RenderGraph &graph,
        std::vector<RHI::BindGroupHandle> &transient_bind_groups, RenderGraphTextureHandle scene_color,
        RenderGraphTextureHandle radiance_history, Core::Extent2D extent, f32 exposure, u64 history_key, u64 frame_index,
        const RenderGraphSettings &settings) {
        if (!scene_color || !radiance_history) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "The screen-space GI history pass needs the scene colour and the radiance history.");
        }
        const auto kernel = renderer.prepare_compute_kernel(kernel_description("ssgi_history", "historyMain"));
        if (!kernel) return std::unexpected(kernel.error());
        const Core::Extent2D half = half_extent(extent);
        // Stamps the texture as written this frame (same description, so it is not recreated).
        auto lease = histories.acquire(
            device, history_texture_key(history_key, kEffectRadianceHistory),
            HistoryTextureCache::Desc{.extent = RHI::Extent3D{.width = half.x, .height = half.y, .depth_or_layers = 1},
                                      .label = "ssgi radiance history"},
            frame_index);
        if (!lease) return std::unexpected(lease.error());
        const HistoryConstants constants{
            .inverse_exposure = exposure > 1.0e-6f && std::isfinite(exposure) ? 1.0f / exposure : 1.0f,
            .max_luminance = std::max(finite_or(settings.frame.screen_space_gi.max_radiance, 64.0f), 0.0f),
            .output_extent = glm::uvec2{half.x, half.y},
        };
        graph.add_compute_pass("ssgi radiance history"_ustr)
            .add_sampled_texture(scene_color)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = radiance_history, .read = false, .write = true})
            .set_side_effect(true)
            .set_execute([&renderer, &transient_bind_groups, kernel = *kernel, scene_color, radiance_history, half,
                          constants](RenderGraphComputeContext &context) -> Core::RendererResult {
                const ComputeBinding bindings[] = {
                    {"sceneColor", context.texture(scene_color).default_view},
                    {"radianceHistoryOut", context.texture(radiance_history).default_view},
                };
                return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, bytes_of(constants),
                                                      glm::uvec3{(half.x + 7u) / 8u, (half.y + 7u) / 8u, 1u},
                                                      transient_bind_groups);
            });
        return {};
    }

    Core::RendererResult build_screen_space_gi_feature(FrameBuildContext &frame) {
        if (frame.direct_overlay_presentation || !screen_space_gi_active(frame.settings, frame.camera)) {
            return {};
        }
        auto result = add_screen_space_gi_passes(
            frame.renderer, frame.device, frame.renderer.history_textures(), frame.graph, frame.transient_bind_groups,
            ScreenSpaceGiDescription{
                .depth = frame.resources.texture<RenderGraphSemantics::ResolvedSceneDepth>(),
                .normal = frame.resources.texture<RenderGraphSemantics::GBufferNormal>(),
                .motion = frame.resources.texture<RenderGraphSemantics::GBufferMotion>(),
                .extent = frame.module.render_extent,
                .camera = frame.camera,
                .lens_strength = frame.settings.frame.camera_emulation.lens_strength,
                .frame_index = frame.frame_index,
                .history_key = static_cast<u64>(frame.surface.window_id),
            },
            frame.settings);
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        frame.resources.publish_texture<RenderGraphSemantics::SurfelIrradiance>(result->irradiance);
        frame.resources.publish_texture<RenderGraphSemantics::ScreenSpaceGiRadianceHistory>(result->radiance_history);
        return {};
    }

    Core::RendererResult build_screen_space_gi_history_feature(FrameBuildContext &frame) {
        const RenderGraphTextureHandle radiance_history =
            frame.resources.texture<RenderGraphSemantics::ScreenSpaceGiRadianceHistory>();
        if (!radiance_history) {
            return {};
        }
        return add_screen_space_gi_history_pass(
            frame.renderer, frame.device, frame.renderer.history_textures(), frame.graph, frame.transient_bind_groups,
            frame.resources.texture<RenderGraphSemantics::SceneHdrColor>(), radiance_history, frame.module.render_extent,
            frame.exposure, static_cast<u64>(frame.surface.window_id), frame.frame_index, frame.settings);
    }

} // namespace SFT::Renderer
