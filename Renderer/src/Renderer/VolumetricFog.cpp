#include <Renderer/VolumetricFog.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <span>
#include <string>

#include <Renderer/FrameUpload.hpp>
#include <Renderer/RendererModule.hpp>

namespace SFT::Renderer {

    namespace {

        constexpr u32 kEffectFogInject = 4;
        // Element size of `LocalLightGpuData` (sturdy_lighting_data.slang; the C++ mirror is private to `Renderer`).
        constexpr u32 kLocalLightStride = 64;

        // Must match `FogConstants` in Shaders/volumetric_fog_common.slang. Only vec4/mat4 members, so the std140 and
        // C++ layouts agree without padding rules.
        struct FogConstants {
            glm::mat4 previous_view_projection{1.0f};
            glm::uvec4 grid{};
            glm::vec4 depth{};
            glm::vec4 medium{};
            glm::vec4 albedo_noise{};
            glm::vec4 emissive_noise_scale{};
            glm::vec4 wind_time{};
            glm::vec4 light_scales{};
            glm::uvec4 flags{};
            glm::vec4 render{};
        };
        static_assert(sizeof(FogConstants) == 208);

        [[nodiscard]] ComputeKernelDescription kernel_description(std::string_view module, std::string_view entry) {
            return ComputeKernelDescription{
                .shader_path = "Shaders/" + std::string{module} + ".slang",
                .module_name = std::string{module},
                .entry_point = std::string{entry},
                .label = std::string{module},
            };
        }

        [[nodiscard]] f32 finite_or(f32 value, f32 fallback) noexcept { return std::isfinite(value) ? value : fallback; }

        [[nodiscard]] Core::GraphicsBackendError fog_error(std::string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

        [[nodiscard]] f32 seconds_since_start() noexcept {
            static const auto start = std::chrono::steady_clock::now();
            return std::chrono::duration<f32>(std::chrono::steady_clock::now() - start).count();
        }

    } // namespace

    bool volumetric_fog_active(const RenderGraphSettings &settings, const CameraView &camera) noexcept {
        const bool orthographic = std::abs(camera.projection[3][3]) > 0.5f;
        return settings.frame.volumetric_fog.enabled && settings.render_scene &&
               settings.spectral_path_tracing.mode != SpectralRenderMode::FullPathTracing && !orthographic;
    }

    glm::uvec4 volumetric_fog_grid(Core::Extent2D extent, const RenderSettings::VolumetricFogSettings &fog) noexcept {
        const u32 tile = std::clamp(fog.tile_px, 2u, 64u);
        const u32 slices = std::clamp(fog.slice_count, 4u, 256u);
        const u32 tiles_x = std::max((extent.x + tile - 1u) / tile, 1u);
        const u32 tiles_y = std::max((extent.y + tile - 1u) / tile, 1u);
        // Roughly square atlas: about sqrt(slices) slices per row.
        const u32 columns = std::max(static_cast<u32>(std::ceil(std::sqrt(static_cast<f32>(slices)))), 1u);
        return glm::uvec4{tiles_x, tiles_y, slices, columns};
    }

    Core::RendererExpected<RenderGraphTextureHandle> add_volumetric_fog_passes(
        Renderer &renderer, RHI::RhiDevice &device, HistoryTextureCache &histories, RenderGraph &graph,
        std::vector<RHI::BindGroupHandle> &transient_bind_groups, std::vector<RHI::BufferHandle> &transient_buffers,
        const VolumetricFogDescription &description, const RenderGraphSettings &settings) {
        if (!description.scene_color || !description.depth || !description.lighting_constants) {
            return std::unexpected(fog_error("Volumetric fog needs the scene colour, the scene depth and the lighting constants."));
        }
        const auto inject_kernel = renderer.prepare_compute_kernel(kernel_description("volumetric_fog_inject", "injectMain"));
        if (!inject_kernel) return std::unexpected(inject_kernel.error());
        const auto integrate_kernel = renderer.prepare_compute_kernel(kernel_description("volumetric_fog_integrate", "integrateMain"));
        if (!integrate_kernel) return std::unexpected(integrate_kernel.error());
        const auto apply_kernel = renderer.prepare_compute_kernel(kernel_description("volumetric_fog_apply", "applyMain"));
        if (!apply_kernel) return std::unexpected(apply_kernel.error());

        const RenderSettings::VolumetricFogSettings &fog = settings.frame.volumetric_fog;
        const Core::Extent2D extent = description.extent;
        const glm::uvec4 grid = volumetric_fog_grid(extent, fog);
        const u32 tile = std::clamp(fog.tile_px, 2u, 64u);
        const u32 rows = (grid.z + grid.w - 1u) / grid.w;
        const RHI::Extent3D atlas_extent{.width = grid.x * grid.w, .height = grid.y * rows, .depth_or_layers = 1};
        const u64 frame = description.frame_index;

        // The injected volume ping-pongs through the history cache: write this frame's slot, reproject from the other.
        const HistoryTextureCache::Desc inject_desc{.extent = atlas_extent, .label = "volumetric fog froxels"};
        auto previous = histories.acquire(
            device, history_texture_key(description.history_key, kEffectFogInject, static_cast<u32>((frame + 1) & 1u)), inject_desc);
        if (!previous) return std::unexpected(previous.error());
        auto current = histories.acquire(
            device, history_texture_key(description.history_key, kEffectFogInject, static_cast<u32>(frame & 1u)), inject_desc, frame);
        if (!current) return std::unexpected(current.error());
        const bool has_history = previous->written_in_previous_frame(frame);
        const RenderGraphTextureHandle inject_history =
            import_history_texture(graph, *previous, previous->ever_written(), "volumetric fog froxels (previous)");
        const RenderGraphTextureHandle injected = import_history_texture(graph, *current, false, "volumetric fog froxels");

        const RenderGraphTextureHandle integrated = graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::RGBA16Float,
            .extent = atlas_extent,
            .usage = RHI::TextureUsage::Storage | RHI::TextureUsage::Sampled,
            .label = "volumetric fog integrated",
        });
        const RenderGraphTextureHandle destination = graph.create_texture(RenderGraphTextureDesc{
            .format = description.output_format,
            .extent = description.output_extent,
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled | RHI::TextureUsage::Storage |
                     RHI::TextureUsage::TransferSrc | RHI::TextureUsage::TransferDst,
            .label = "volumetric fog scene colour",
        });

        const bool clustered = description.local_lights && description.cluster_ranges && description.cluster_indices;
        const f32 near_plane = std::max(finite_or(description.camera.near_plane, 0.01f), 1.0e-3f);
        const FogConstants constants{
            .previous_view_projection = description.camera.previous_view_projection,
            .grid = grid,
            .depth = glm::vec4{near_plane, std::max(finite_or(fog.max_distance, 96.0f), near_plane * 2.0f),
                               std::clamp(finite_or(fog.slice_distribution, 2.0f), 1.0f, 8.0f),
                               std::clamp(finite_or(fog.temporal_blend, 0.1f), 0.01f, 1.0f)},
            .medium = glm::vec4{std::max(finite_or(fog.density, 0.0f), 0.0f), std::max(finite_or(fog.height_falloff, 0.0f), 0.0f),
                                finite_or(fog.base_height, 0.0f), std::clamp(finite_or(fog.anisotropy, 0.0f), -0.99f, 0.99f)},
            .albedo_noise = glm::vec4{glm::clamp(glm::vec3{fog.albedo[0], fog.albedo[1], fog.albedo[2]}, glm::vec3{0.0f}, glm::vec3{1.0f}),
                                      std::clamp(finite_or(fog.noise_strength, 0.0f), 0.0f, 1.0f)},
            .emissive_noise_scale = glm::vec4{glm::max(glm::vec3{fog.emissive[0], fog.emissive[1], fog.emissive[2]}, glm::vec3{0.0f}),
                                              std::max(finite_or(fog.noise_scale, 6.0f), 1.0e-3f)},
            .wind_time = glm::vec4{fog.wind[0], fog.wind[1], fog.wind[2], description.time_seconds},
            .light_scales = glm::vec4{std::max(finite_or(fog.sun_intensity, 1.0f), 0.0f), std::max(finite_or(fog.ambient_intensity, 1.0f), 0.0f),
                                      std::max(finite_or(fog.local_light_intensity, 1.0f), 0.0f),
                                      std::max(finite_or(description.exposure, 1.0f), 0.0f)},
            .flags = glm::uvec4{fog.sun_shadows && description.directional_shadow_atlas ? 1u : 0u,
                                fog.local_light_shadows && description.punctual_shadow_atlas ? 1u : 0u, has_history ? 1u : 0u,
                                static_cast<u32>(frame & 0xffffffffu)},
            .render = glm::vec4{static_cast<f32>(extent.x), static_cast<f32>(extent.y), static_cast<f32>(tile), clustered ? 1.0f : 0.0f},
        };
        auto constants_buffer = upload_transient_buffer(device, transient_buffers, RHI::BufferUsage::Uniform,
                                                 std::as_bytes(std::span<const FogConstants>{&constants, 1}), "volumetric fog constants");
        if (!constants_buffer) return std::unexpected(constants_buffer.error());
        const RHI::BufferHandle fog_constants = *constants_buffer;

        // Without clustered lights (or shadow atlases) the shaders still declare those resources; they get stand-ins
        // the flags above keep them from reading.
        RHI::BufferHandle placeholder{};
        if (!clustered) {
            auto created = upload_transient_buffer(device, transient_buffers, RHI::BufferUsage::Storage, {}, "volumetric fog placeholder");
            if (!created) return std::unexpected(created.error());
            placeholder = *created;
        }
        const RenderGraphTextureHandle directional_atlas =
            description.directional_shadow_atlas ? description.directional_shadow_atlas : description.depth;
        const RenderGraphTextureHandle punctual_atlas =
            description.punctual_shadow_atlas ? description.punctual_shadow_atlas : description.depth;

        RenderGraphComputePassBuilder &inject = graph.add_compute_pass("volumetric fog inject"_ustr);
        inject.add_sampled_texture(directional_atlas)
            .add_sampled_texture(punctual_atlas)
            .add_sampled_texture(inject_history)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = injected, .read = false, .write = true})
            .add_buffer(RenderGraphBufferAccessDesc{.buffer = description.lighting_constants,
                                                    .stages = RHI::PipelineStage::ComputeShader,
                                                    .access = RHI::AccessFlags::UniformRead});
        if (clustered) {
            for (const RenderGraphBufferHandle buffer : {description.local_lights, description.cluster_ranges, description.cluster_indices}) {
                inject.add_buffer(RenderGraphBufferAccessDesc{
                    .buffer = buffer, .stages = RHI::PipelineStage::ComputeShader, .access = RHI::AccessFlags::ShaderRead});
            }
        }
        inject.set_execute([&renderer, &transient_bind_groups, kernel = *inject_kernel, description, clustered, placeholder,
                            fog_constants, directional_atlas, punctual_atlas, inject_history, injected,
                            grid](RenderGraphComputeContext &context) -> Core::RendererResult {
            const auto lighting = context.buffer(description.lighting_constants);
            const auto structured = [&](RenderGraphBufferHandle handle, std::string_view name, u32 stride) {
                if (!clustered) {
                    return ComputeBufferBinding{.name = name, .buffer = placeholder, .structure_stride = stride, .size = 256};
                }
                const auto bound = context.buffer(handle);
                return ComputeBufferBinding{.name = name, .buffer = bound.buffer, .structure_stride = stride, .size = bound.size};
            };
            const ComputeBufferBinding buffers[] = {
                {.name = "lightingData", .buffer = lighting.buffer, .size = lighting.size},
                {.name = "fog", .buffer = fog_constants, .size = sizeof(FogConstants)},
                structured(description.local_lights, "localLights", kLocalLightStride),
                structured(description.cluster_ranges, "clusterRanges", static_cast<u32>(sizeof(glm::uvec2))),
                structured(description.cluster_indices, "clusterLightIndices", static_cast<u32>(sizeof(u32))),
            };
            const ComputeBinding bindings[] = {
                {"directionalShadowAtlas", context.texture(directional_atlas).default_view},
                {"shadowAtlas", context.texture(punctual_atlas).default_view},
                {"injectHistory", context.texture(inject_history).default_view},
                {"injectOut", context.texture(injected).default_view},
            };
            return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, {},
                                                  glm::uvec3{(grid.x + 7u) / 8u, (grid.y + 7u) / 8u, grid.z}, transient_bind_groups,
                                                  buffers);
        });

        graph.add_compute_pass("volumetric fog integrate"_ustr)
            .add_sampled_texture(injected)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = integrated, .read = false, .write = true})
            .add_buffer(RenderGraphBufferAccessDesc{.buffer = description.lighting_constants,
                                                    .stages = RHI::PipelineStage::ComputeShader,
                                                    .access = RHI::AccessFlags::UniformRead})
            .set_execute([&renderer, &transient_bind_groups, kernel = *integrate_kernel, lighting_constants = description.lighting_constants,
                          fog_constants, injected, integrated, grid](RenderGraphComputeContext &context) -> Core::RendererResult {
                const auto lighting = context.buffer(lighting_constants);
                const ComputeBufferBinding buffers[] = {
                    {.name = "lightingData", .buffer = lighting.buffer, .size = lighting.size},
                    {.name = "fog", .buffer = fog_constants, .size = sizeof(FogConstants)},
                };
                const ComputeBinding bindings[] = {
                    {"injected", context.texture(injected).default_view},
                    {"integratedOut", context.texture(integrated).default_view},
                };
                return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, {},
                                                      glm::uvec3{(grid.x + 7u) / 8u, (grid.y + 7u) / 8u, 1u}, transient_bind_groups,
                                                      buffers);
            });

        graph.add_compute_pass("volumetric fog apply"_ustr)
            .add_sampled_texture(description.scene_color)
            .add_sampled_texture(description.depth)
            .add_sampled_texture(integrated)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = destination, .read = false, .write = true})
            .add_buffer(RenderGraphBufferAccessDesc{.buffer = description.lighting_constants,
                                                    .stages = RHI::PipelineStage::ComputeShader,
                                                    .access = RHI::AccessFlags::UniformRead})
            .set_execute([&renderer, &transient_bind_groups, kernel = *apply_kernel, lighting_constants = description.lighting_constants,
                          fog_constants, scene_color = description.scene_color, depth = description.depth, integrated, destination,
                          extent](RenderGraphComputeContext &context) -> Core::RendererResult {
                const auto lighting = context.buffer(lighting_constants);
                const ComputeBufferBinding buffers[] = {
                    {.name = "lightingData", .buffer = lighting.buffer, .size = lighting.size},
                    {.name = "fog", .buffer = fog_constants, .size = sizeof(FogConstants)},
                };
                const ComputeBinding bindings[] = {
                    {"sceneColor", context.texture(scene_color).default_view},
                    {"sceneDepth", context.texture(depth).default_view},
                    {"integratedFog", context.texture(integrated).default_view},
                    {"sceneColorOut", context.texture(destination).default_view},
                };
                return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, {},
                                                      glm::uvec3{(extent.x + 7u) / 8u, (extent.y + 7u) / 8u, 1u}, transient_bind_groups,
                                                      buffers);
            });

        return destination;
    }

    Core::RendererResult build_volumetric_fog_feature(FrameBuildContext &frame) {
        if (frame.direct_overlay_presentation || !volumetric_fog_active(frame.settings, frame.camera)) {
            return {};
        }
        const RenderGraphBufferHandle lighting_constants = frame.resources.buffer<RenderGraphSemantics::LightingConstants>();
        if (!lighting_constants || frame.transient_buffers == nullptr) {
            // No deferred lighting this frame (nothing to light the fog with).
            return {};
        }
        auto fogged = add_volumetric_fog_passes(
            frame.renderer, frame.device, frame.renderer.history_textures(), frame.graph, frame.transient_bind_groups,
            *frame.transient_buffers,
            VolumetricFogDescription{
                .scene_color = frame.resources.texture<RenderGraphSemantics::SceneHdrColor>(),
                .depth = frame.resources.texture<RenderGraphSemantics::ResolvedSceneDepth>(),
                .lighting_constants = lighting_constants,
                .local_lights = frame.resources.buffer<RenderGraphSemantics::LocalLights>(),
                .cluster_ranges = frame.resources.buffer<RenderGraphSemantics::LightClusterRanges>(),
                .cluster_indices = frame.resources.buffer<RenderGraphSemantics::LightClusterIndices>(),
                .directional_shadow_atlas = frame.resources.texture<RenderGraphSemantics::DirectionalShadowAtlas>(),
                .punctual_shadow_atlas = frame.resources.texture<RenderGraphSemantics::PunctualShadowAtlas>(),
                .extent = frame.module.render_extent,
                .output_extent = frame.module.render_texture_extent(),
                .output_format = frame.deferred_formats.scene_color,
                .camera = frame.camera,
                .exposure = frame.exposure,
                .time_seconds = seconds_since_start(),
                .frame_index = frame.frame_index,
                .history_key = static_cast<u64>(frame.surface.window_id),
            },
            frame.settings);
        if (!fogged) {
            return std::unexpected(fogged.error());
        }
        frame.resources.publish_texture<RenderGraphSemantics::SceneHdrColor>(*fogged);
        return {};
    }

} // namespace SFT::Renderer
