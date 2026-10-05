#include <Renderer/Reflections.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string>

#include <Renderer/FrameUpload.hpp>
#include <Renderer/RendererModule.hpp>

namespace SFT::Renderer {

    namespace {

        // Distinct from the other temporal effects' ids (ScreenSpaceGi 1-2, TemporalUpscaler 3, VolumetricFog 4).
        constexpr u32 kEffectEnvironment = 5;
        constexpr u32 kEffectColorChain = 6; // slot = blur level

        // Must match Shaders/sturdy_specular.slang.
        constexpr u32 kEnvironmentLevels = 6;
        constexpr u32 kEnvironmentAtlasWidth = 192;
        constexpr u32 kEnvironmentAtlasHeight = 128;
        constexpr u32 kPrefilterSamples = 48;
        // Must match SSR_PYRAMID_LEVELS in Shaders/ssr_common.slang.
        constexpr u32 kPyramidLevels = 6;
        // Blurred levels of last frame's lit colour (level 0 at half resolution, each next level half the size again).
        constexpr u32 kColorLevels = 5;
        // Radiance kept in the history is clamped so one super-bright texel cannot flare into every later frame.
        constexpr f32 kHistoryMaxRadiance = 64.0f;

        // Must match the push-constant / constant blocks in the shaders of the same name.
        struct PrefilterConstants {
            u32 level_mask = 0;
            u32 sample_count = 0;
            u32 pad[2]{};
        };
        static_assert(sizeof(PrefilterConstants) == 16);

        struct PyramidConstants {
            glm::uvec2 full_extent{};
            glm::uvec2 half_extent{};
        };
        static_assert(sizeof(PyramidConstants) == 16);

        struct SsrConstants {
            glm::uvec4 extents{};
            glm::uvec4 params{};
            glm::vec4 tuning{};
        };
        static_assert(sizeof(SsrConstants) == 48);

        struct DownsampleConstants {
            glm::uvec2 output_extent{};
            glm::uvec2 pad{};
        };
        static_assert(sizeof(DownsampleConstants) == 16);

        struct HistoryConstants {
            f32 inverse_exposure = 1.0f;
            f32 max_luminance = kHistoryMaxRadiance;
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

        [[nodiscard]] glm::uvec2 half_of(Core::Extent2D extent) noexcept {
            return glm::uvec2{std::max((extent.x + 1u) / 2u, 1u), std::max((extent.y + 1u) / 2u, 1u)};
        }

        [[nodiscard]] u32 level_extent(u32 half, u32 level) noexcept { return std::max((half + (1u << level) - 1u) >> level, 1u); }

        [[nodiscard]] f32 finite_or(f32 value, f32 fallback) noexcept { return std::isfinite(value) ? value : fallback; }

        [[nodiscard]] Core::GraphicsBackendError reflection_error(std::string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

        [[nodiscard]] RenderGraphTextureHandle color_level(const RenderGraphBlackboard &resources, u32 level) {
            switch (level) {
                case 0: return resources.texture<RenderGraphSemantics::ReflectionColorLevel0>();
                case 1: return resources.texture<RenderGraphSemantics::ReflectionColorLevel1>();
                case 2: return resources.texture<RenderGraphSemantics::ReflectionColorLevel2>();
                case 3: return resources.texture<RenderGraphSemantics::ReflectionColorLevel3>();
                default: return resources.texture<RenderGraphSemantics::ReflectionColorLevel4>();
            }
        }

        void publish_color_level(RenderGraphBlackboard &resources, u32 level, RenderGraphTextureHandle texture) {
            switch (level) {
                case 0: resources.publish_texture<RenderGraphSemantics::ReflectionColorLevel0>(texture); break;
                case 1: resources.publish_texture<RenderGraphSemantics::ReflectionColorLevel1>(texture); break;
                case 2: resources.publish_texture<RenderGraphSemantics::ReflectionColorLevel2>(texture); break;
                case 3: resources.publish_texture<RenderGraphSemantics::ReflectionColorLevel3>(texture); break;
                default: resources.publish_texture<RenderGraphSemantics::ReflectionColorLevel4>(texture); break;
            }
        }

        [[nodiscard]] HistoryTextureCache::Desc color_level_desc(glm::uvec2 half, u32 level) {
            return HistoryTextureCache::Desc{
                .extent = RHI::Extent3D{.width = level_extent(half.x, level), .height = level_extent(half.y, level), .depth_or_layers = 1},
                .label = "reflection colour chain"};
        }

    } // namespace

    bool reflections_environment_active(const RenderGraphSettings &settings) noexcept {
        return settings.frame.reflections.environment && settings.render_scene &&
               settings.spectral_path_tracing.mode != SpectralRenderMode::FullPathTracing;
    }

    bool reflections_screen_space_active(const RenderGraphSettings &settings, const CameraView &camera) noexcept {
        const bool orthographic = std::abs(camera.projection[3][3]) > 0.5f;
        return settings.frame.reflections.screen_space && settings.render_scene &&
               settings.spectral_path_tracing.mode != SpectralRenderMode::FullPathTracing && !orthographic;
    }

    u32 reflection_environment_level_mask(u64 frame_index, bool atlas_has_contents) noexcept {
        constexpr u32 all_levels = (1u << kEnvironmentLevels) - 1u;
        if (!atlas_has_contents) {
            return all_levels;
        }
        // Level 0 is the sky itself; rough levels take turns, so each is refreshed every (levels - 1) frames.
        const u32 rough = 1u + static_cast<u32>(frame_index % (kEnvironmentLevels - 1u));
        return 1u | (1u << rough);
    }

    glm::uvec2 reflection_color_level_extent(glm::uvec2 half_extent, u32 level) noexcept {
        return glm::uvec2{level_extent(half_extent.x, level), level_extent(half_extent.y, level)};
    }

    glm::uvec2 reflection_pyramid_extent(glm::uvec2 half_extent) noexcept {
        u32 column_height = 0;
        for (u32 level = 1; level < kPyramidLevels; ++level) {
            column_height += level_extent(half_extent.y, level);
        }
        return glm::uvec2{half_extent.x + level_extent(half_extent.x, 1u), std::max(half_extent.y, column_height)};
    }

    Core::RendererExpected<ReflectionResult> add_reflection_passes(
        Renderer &renderer, RHI::RhiDevice &device, HistoryTextureCache &histories, RenderGraph &graph,
        std::vector<RHI::BindGroupHandle> &transient_bind_groups, std::vector<RHI::BufferHandle> &transient_buffers,
        const ReflectionDescription &description, const RenderGraphSettings &settings) {
        if (!description.sky_view_lut || !description.atmosphere_constants) {
            return std::unexpected(reflection_error("Reflections need the atmosphere sky-view LUT and its constants."));
        }
        const RenderSettings::ReflectionSettings &reflections = settings.frame.reflections;
        const u64 frame = description.frame_index;
        ReflectionResult result;

        // ---- environment: the sky prefiltered per roughness level -----------------------------------------------------
        {
            const auto kernel = renderer.prepare_compute_kernel(kernel_description("environment_prefilter", "prefilterMain"));
            if (!kernel) return std::unexpected(kernel.error());
            const HistoryTextureCache::Desc atlas_desc{
                .format = RHI::Format::RGBA16Float,
                .extent = RHI::Extent3D{.width = kEnvironmentAtlasWidth, .height = kEnvironmentAtlasHeight, .depth_or_layers = 1},
                .label = "reflection environment atlas"};
            const u64 key = history_texture_key(description.history_key, kEffectEnvironment);
            // Before acquiring for write: has the atlas ever been filled (and at this size)?
            auto lease = histories.acquire(device, key, atlas_desc, frame);
            if (!lease) return std::unexpected(lease.error());
            const bool has_contents = lease->ever_written();
            result.environment = import_history_texture(graph, *lease, has_contents, "reflection environment atlas");
            const PrefilterConstants constants{.level_mask = reflection_environment_level_mask(frame, has_contents),
                                               .sample_count = kPrefilterSamples};
            graph.add_compute_pass("reflection environment prefilter"_ustr)
                .add_sampled_texture(description.sky_view_lut)
                .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = result.environment, .read = true, .write = true})
                .add_buffer(RenderGraphBufferAccessDesc{.buffer = description.atmosphere_constants,
                                                        .stages = RHI::PipelineStage::ComputeShader,
                                                        .access = RHI::AccessFlags::UniformRead})
                .set_execute([&renderer, &transient_bind_groups, kernel = *kernel, sky = description.sky_view_lut,
                              atmosphere = description.atmosphere_constants, environment = result.environment,
                              constants](RenderGraphComputeContext &context) -> Core::RendererResult {
                    const auto atmosphere_buffer = context.buffer(atmosphere);
                    const ComputeBufferBinding buffers[] = {
                        {.name = "atmosphereData", .buffer = atmosphere_buffer.buffer, .size = atmosphere_buffer.size}};
                    const ComputeBinding bindings[] = {{"skyViewLut", context.texture(sky).default_view},
                                                       {"environmentOut", context.texture(environment).default_view}};
                    return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, bytes_of(constants),
                                                          glm::uvec3{kEnvironmentAtlasWidth / 8u, kEnvironmentAtlasHeight / 8u, 1u},
                                                          transient_bind_groups, buffers);
                });
        }

        // ---- screen-space reflections ---------------------------------------------------------------------------------
        if (!reflections_screen_space_active(settings, description.camera)) {
            return result;
        }
        if (!description.lighting_constants) {
            return result; // nothing published the lighting constants (no deferred lighting this frame): environment only
        }
        if (!description.depth || !description.normal || !description.material || !description.motion) {
            return std::unexpected(reflection_error("Screen-space reflections need depth, normal, material and motion textures."));
        }
        const auto pyramid_kernel = renderer.prepare_compute_kernel(kernel_description("ssr_depth_pyramid", "pyramidMain"));
        if (!pyramid_kernel) return std::unexpected(pyramid_kernel.error());
        const auto trace_kernel = renderer.prepare_compute_kernel(kernel_description("ssr_trace", "traceMain"));
        if (!trace_kernel) return std::unexpected(trace_kernel.error());
        const auto filter_kernel = renderer.prepare_compute_kernel(kernel_description("ssr_filter", "filterMain"));
        if (!filter_kernel) return std::unexpected(filter_kernel.error());

        const Core::Extent2D full = description.extent;
        const glm::uvec2 half = half_of(full);

        // Last frame's lit colour and its blurred levels (written by `reflections_history` at the end of the previous frame).
        // Level 0 stands for the whole chain: every level is stamped in the same frame.
        std::array<RenderGraphTextureHandle, kColorLevels> chain{};
        bool history_valid = false;
        for (u32 level = 0; level < kColorLevels; ++level) {
            auto lease = histories.acquire(device, history_texture_key(description.history_key, kEffectColorChain, level),
                                           color_level_desc(half, level));
            if (!lease) return std::unexpected(lease.error());
            if (level == 0) history_valid = lease->written_in_previous_frame(frame);
            chain[level] = import_history_texture(graph, *lease, lease->ever_written(), "reflection colour chain");
            publish_color_level(description.resources, level, chain[level]);
        }

        constexpr RHI::TextureUsage storage_sampled = RHI::TextureUsage::Storage | RHI::TextureUsage::Sampled;
        const glm::uvec2 pyramid_extent = reflection_pyramid_extent(half);
        const RenderGraphTextureHandle depth_pyramid = graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::R32Float,
            .extent = RHI::Extent3D{.width = pyramid_extent.x, .height = pyramid_extent.y, .depth_or_layers = 1},
            .usage = storage_sampled,
            .label = "ssr min-depth pyramid"});
        const RHI::Extent3D full_extent3{.width = full.x, .height = full.y, .depth_or_layers = 1};
        const RenderGraphTextureHandle traced = graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::RGBA16Float, .extent = full_extent3, .usage = storage_sampled, .label = "ssr trace"});
        result.screen_space = graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::RGBA16Float, .extent = full_extent3, .usage = storage_sampled, .label = "screen-space reflections"});

        const SsrConstants constants{
            .extents = glm::uvec4{full.x, full.y, half.x, half.y},
            .params = glm::uvec4{std::clamp(reflections.max_steps, 8u, 128u), kColorLevels, history_valid ? 1u : 0u, 0u},
            .tuning = glm::vec4{std::clamp(finite_or(reflections.max_roughness, 0.6f), 0.0f, 1.0f),
                                std::clamp(finite_or(reflections.thickness, 0.02f), 0.001f, 0.5f),
                                std::clamp(finite_or(reflections.glossy_blur, 1.0f), 0.0f, 8.0f), 0.0f},
        };
        auto constants_buffer = upload_transient_buffer(device, transient_buffers, RHI::BufferUsage::Uniform, bytes_of(constants),
                                                        "ssr constants");
        if (!constants_buffer) return std::unexpected(constants_buffer.error());
        const RHI::BufferHandle ssr_buffer = *constants_buffer;
        const PyramidConstants pyramid_constants{.full_extent = glm::uvec2{full.x, full.y}, .half_extent = half};

        graph.add_compute_pass("ssr depth pyramid"_ustr)
            .add_sampled_texture(description.depth)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = depth_pyramid, .read = false, .write = true})
            .set_execute([&renderer, &transient_bind_groups, kernel = *pyramid_kernel, depth = description.depth, depth_pyramid, full,
                          pyramid_constants](RenderGraphComputeContext &context) -> Core::RendererResult {
                const ComputeBinding bindings[] = {{"sceneDepth", context.texture(depth).default_view},
                                                   {"depthPyramidOut", context.texture(depth_pyramid).default_view}};
                // One 16x16 group reduces a 64x64 tile of the full-resolution depth.
                return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, bytes_of(pyramid_constants),
                                                      glm::uvec3{(full.x + 63u) / 64u, (full.y + 63u) / 64u, 1u}, transient_bind_groups);
            });

        RenderGraphComputePassBuilder &trace = graph.add_compute_pass("ssr trace"_ustr);
        trace.add_sampled_texture(description.depth)
            .add_sampled_texture(description.normal)
            .add_sampled_texture(description.material)
            .add_sampled_texture(description.motion)
            .add_sampled_texture(depth_pyramid)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = traced, .read = false, .write = true})
            .add_buffer(RenderGraphBufferAccessDesc{.buffer = description.lighting_constants,
                                                    .stages = RHI::PipelineStage::ComputeShader,
                                                    .access = RHI::AccessFlags::UniformRead});
        for (const RenderGraphTextureHandle level : chain) {
            trace.add_sampled_texture(level);
        }
        trace.set_execute([&renderer, &transient_bind_groups, kernel = *trace_kernel, description, depth_pyramid, chain,
                           reflection = traced, ssr_buffer, full](RenderGraphComputeContext &context) -> Core::RendererResult {
            const auto lighting = context.buffer(description.lighting_constants);
            const ComputeBufferBinding buffers[] = {{.name = "lightingData", .buffer = lighting.buffer, .size = lighting.size},
                                                    {.name = "ssr", .buffer = ssr_buffer, .size = sizeof(SsrConstants)}};
            const ComputeBinding bindings[] = {
                {"sceneDepth", context.texture(description.depth).default_view},
                {"gbufferNormal", context.texture(description.normal).default_view},
                {"gbufferMaterial", context.texture(description.material).default_view},
                {"gbufferMotion", context.texture(description.motion).default_view},
                {"depthPyramid", context.texture(depth_pyramid).default_view},
                {"colorLevel0", context.texture(chain[0]).default_view},
                {"colorLevel1", context.texture(chain[1]).default_view},
                {"colorLevel2", context.texture(chain[2]).default_view},
                {"colorLevel3", context.texture(chain[3]).default_view},
                {"colorLevel4", context.texture(chain[4]).default_view},
                {"reflectionOut", context.texture(reflection).default_view},
            };
            return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, {},
                                                  glm::uvec3{(full.x + 7u) / 8u, (full.y + 7u) / 8u, 1u}, transient_bind_groups, buffers);
        });

        // Edge-aware clean-up: turns the per-pixel hit/miss flips along a reflection's boundary into a smooth gradient.
        graph.add_compute_pass("ssr filter"_ustr)
            .add_sampled_texture(description.depth)
            .add_sampled_texture(description.normal)
            .add_sampled_texture(description.material)
            .add_sampled_texture(traced)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = result.screen_space, .read = false, .write = true})
            .add_buffer(RenderGraphBufferAccessDesc{.buffer = description.lighting_constants,
                                                    .stages = RHI::PipelineStage::ComputeShader,
                                                    .access = RHI::AccessFlags::UniformRead})
            .set_execute([&renderer, &transient_bind_groups, kernel = *filter_kernel, description, traced,
                          filtered = result.screen_space, ssr_buffer, full](RenderGraphComputeContext &context) -> Core::RendererResult {
                const auto lighting = context.buffer(description.lighting_constants);
                const ComputeBufferBinding buffers[] = {{.name = "lightingData", .buffer = lighting.buffer, .size = lighting.size},
                                                        {.name = "ssr", .buffer = ssr_buffer, .size = sizeof(SsrConstants)}};
                const ComputeBinding bindings[] = {
                    {"sceneDepth", context.texture(description.depth).default_view},
                    {"gbufferNormal", context.texture(description.normal).default_view},
                    {"gbufferMaterial", context.texture(description.material).default_view},
                    {"traced", context.texture(traced).default_view},
                    {"filteredOut", context.texture(filtered).default_view},
                };
                return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, {},
                                                      glm::uvec3{(full.x + 7u) / 8u, (full.y + 7u) / 8u, 1u}, transient_bind_groups, buffers);
            });
        return result;
    }

    Core::RendererResult add_reflection_history_pass(
        Renderer &renderer, RHI::RhiDevice &device, HistoryTextureCache &histories, RenderGraph &graph,
        std::vector<RHI::BindGroupHandle> &transient_bind_groups, RenderGraphBlackboard &resources,
        RenderGraphTextureHandle scene_color, Core::Extent2D extent, f32 exposure, u64 history_key, u64 frame_index) {
        if (!scene_color) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "The reflection history pass needs the scene colour.");
        }
        // Level 0 is the exposure-free half-resolution lit colour plus the nearest view depth per texel (what the trace
        // validates a hit against); the blurred levels come from the dual-filter downsample.
        const RenderGraphTextureHandle depth = resources.texture<RenderGraphSemantics::ResolvedSceneDepth>();
        const RenderGraphBufferHandle lighting_constants = resources.buffer<RenderGraphSemantics::LightingConstants>();
        if (!depth || !lighting_constants) {
            return {};
        }
        const auto copy_kernel = renderer.prepare_compute_kernel(kernel_description("ssr_color_history", "historyMain"));
        if (!copy_kernel) return std::unexpected(copy_kernel.error());
        const auto downsample_kernel = renderer.prepare_compute_kernel(kernel_description("ssr_color_downsample", "downsampleMain"));
        if (!downsample_kernel) return std::unexpected(downsample_kernel.error());
        const glm::uvec2 half = half_of(extent);

        // The handles `reflections` published are the ones its trace reads: writing through them (rather than importing the
        // textures again) is what orders this frame's writes after that read.
        std::array<RenderGraphTextureHandle, kColorLevels> chain{};
        for (u32 level = 0; level < kColorLevels; ++level) {
            chain[level] = color_level(resources, level);
            if (!chain[level]) {
                return {};
            }
            // Stamps the texture as written this frame (same description, so it is not recreated).
            auto lease = histories.acquire(device, history_texture_key(history_key, kEffectColorChain, level),
                                           color_level_desc(half, level), frame_index);
            if (!lease) return std::unexpected(lease.error());
        }

        const HistoryConstants constants{.inverse_exposure = exposure > 1.0e-6f && std::isfinite(exposure) ? 1.0f / exposure : 1.0f,
                                         .max_luminance = kHistoryMaxRadiance,
                                         .output_extent = half};
        graph.add_compute_pass("reflection colour chain 0"_ustr)
            .add_sampled_texture(scene_color)
            .add_sampled_texture(depth)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = chain[0], .read = false, .write = true})
            .add_buffer(RenderGraphBufferAccessDesc{.buffer = lighting_constants,
                                                    .stages = RHI::PipelineStage::ComputeShader,
                                                    .access = RHI::AccessFlags::UniformRead})
            .set_side_effect(true)
            .set_execute([&renderer, &transient_bind_groups, kernel = *copy_kernel, scene_color, depth, lighting_constants,
                          level0 = chain[0], half, constants](RenderGraphComputeContext &context) -> Core::RendererResult {
                const auto lighting = context.buffer(lighting_constants);
                const ComputeBufferBinding buffers[] = {{.name = "lightingData", .buffer = lighting.buffer, .size = lighting.size}};
                const ComputeBinding bindings[] = {{"sceneColor", context.texture(scene_color).default_view},
                                                   {"sceneDepth", context.texture(depth).default_view},
                                                   {"historyOut", context.texture(level0).default_view}};
                return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, bytes_of(constants),
                                                      glm::uvec3{(half.x + 7u) / 8u, (half.y + 7u) / 8u, 1u}, transient_bind_groups,
                                                      buffers);
            });
        const ustr downsample_labels[kColorLevels - 1] = {"reflection colour chain 1"_ustr, "reflection colour chain 2"_ustr,
                                                          "reflection colour chain 3"_ustr, "reflection colour chain 4"_ustr};
        for (u32 level = 1; level < kColorLevels; ++level) {
            const glm::uvec2 size = reflection_color_level_extent(half, level);
            const DownsampleConstants downsample{.output_extent = size};
            graph.add_compute_pass(downsample_labels[level - 1])
                .add_sampled_texture(chain[level - 1])
                .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = chain[level], .read = false, .write = true})
                .set_side_effect(true)
                .set_execute([&renderer, &transient_bind_groups, kernel = *downsample_kernel, source = chain[level - 1],
                              destination = chain[level], size, downsample](RenderGraphComputeContext &context) -> Core::RendererResult {
                    const ComputeBinding bindings[] = {{"source", context.texture(source).default_view},
                                                       {"destinationOut", context.texture(destination).default_view}};
                    return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, bytes_of(downsample),
                                                          glm::uvec3{(size.x + 7u) / 8u, (size.y + 7u) / 8u, 1u}, transient_bind_groups);
                });
        }
        return {};
    }

    Core::RendererResult build_reflections_feature(FrameBuildContext &frame) {
        if (frame.direct_overlay_presentation || !reflections_environment_active(frame.settings) || frame.transient_buffers == nullptr) {
            return {};
        }
        const RenderGraphTextureHandle sky_view_lut = frame.resources.texture<RenderGraphSemantics::SkyViewLut>();
        const RenderGraphBufferHandle atmosphere = frame.resources.buffer<RenderGraphSemantics::AtmosphereConstants>();
        if (!sky_view_lut || !atmosphere) {
            return {}; // no atmosphere this frame: lighting falls back to its neutral stand-ins
        }
        auto result = add_reflection_passes(
            frame.renderer, frame.device, frame.renderer.history_textures(), frame.graph, frame.transient_bind_groups,
            *frame.transient_buffers,
            ReflectionDescription{
                .depth = frame.resources.texture<RenderGraphSemantics::ResolvedSceneDepth>(),
                .normal = frame.resources.texture<RenderGraphSemantics::GBufferNormal>(),
                .material = frame.resources.texture<RenderGraphSemantics::GBufferMaterial>(),
                .motion = frame.resources.texture<RenderGraphSemantics::GBufferMotion>(),
                .sky_view_lut = sky_view_lut,
                .atmosphere_constants = atmosphere,
                .lighting_constants = frame.resources.buffer<RenderGraphSemantics::LightingConstants>(),
                .resources = frame.resources,
                .extent = frame.module.render_extent,
                .camera = frame.camera,
                .frame_index = frame.frame_index,
                .history_key = static_cast<u64>(frame.surface.window_id),
            },
            frame.settings);
        if (!result) {
            return std::unexpected(result.error());
        }
        frame.resources.publish_texture<RenderGraphSemantics::ReflectionEnvironment>(result->environment);
        if (result->screen_space) {
            frame.resources.publish_texture<RenderGraphSemantics::ScreenSpaceReflections>(result->screen_space);
        }
        return {};
    }

    Core::RendererResult build_reflections_history_feature(FrameBuildContext &frame) {
        // Only when this frame traced reflections (the trace published the colour chain this pass writes).
        if (!frame.resources.texture<RenderGraphSemantics::ScreenSpaceReflections>()) {
            return {};
        }
        return add_reflection_history_pass(
            frame.renderer, frame.device, frame.renderer.history_textures(), frame.graph, frame.transient_bind_groups,
            frame.resources, frame.resources.texture<RenderGraphSemantics::SceneHdrColor>(), frame.module.render_extent,
            frame.exposure, static_cast<u64>(frame.surface.window_id), frame.frame_index);
    }

} // namespace SFT::Renderer
