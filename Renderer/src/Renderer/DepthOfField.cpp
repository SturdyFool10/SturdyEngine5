#include <Renderer/DepthOfField.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <string>

#include <glm/trigonometric.hpp>

#include <Renderer/FrameUpload.hpp>
#include <Renderer/RendererModule.hpp>

namespace SFT::Renderer {

    namespace {

        // Must match `DofConstants` in Shaders/dof_common.slang.
        struct DofConstants {
            glm::uvec4 extents{};
            glm::vec4 depth{};
            glm::vec4 lens{};
            glm::uvec4 gather{};
        };
        static_assert(sizeof(DofConstants) == 64);

        // Half-resolution texels per tile edge (DOF_TILE_SIZE in the shaders).
        constexpr u32 kTileSize = 8;

        [[nodiscard]] ComputeKernelDescription kernel_description(std::string_view module, std::string_view entry) {
            return ComputeKernelDescription{
                .shader_path = "Shaders/" + std::string{module} + ".slang",
                .module_name = std::string{module},
                .entry_point = std::string{entry},
                .label = std::string{module},
            };
        }

        [[nodiscard]] f32 finite_or(f32 value, f32 fallback) noexcept { return std::isfinite(value) ? value : fallback; }

        /// Blur diameter, in pixels, of a point whose |z - d| / z is 1: the lens-and-sensor part of the circle of confusion.
        [[nodiscard]] f32 blur_scale_px(const RenderSettings::DepthOfFieldSettings &lens, f32 image_height_px) noexcept {
            const f32 focal = std::max(finite_or(lens.focal_length_mm, 35.0f), 1.0f);
            const f32 f_stop = std::max(finite_or(lens.aperture_f_stop, 2.8f), 0.1f);
            const f32 focus_mm = std::max(finite_or(lens.focus_distance, 10.0f), 0.01f) * 1000.0f;
            const f32 sensor = std::max(finite_or(lens.sensor_height_mm, 24.0f), 1.0f);
            // f^2 / (N (d - f)): the disc diameter on the sensor, in mm. A focus closer than the focal length cannot be
            // reached by the lens; clamp so the formula stays finite and the blur maximal rather than inverted.
            const f32 diameter_mm = focal * focal / (f_stop * std::max(focus_mm - focal, 1.0f));
            return diameter_mm * (image_height_px / sensor) * std::max(finite_or(lens.strength, 1.0f), 0.0f);
        }

    } // namespace

    bool depth_of_field_active(const RenderGraphSettings &settings, const CameraView &camera) noexcept {
        const bool orthographic = std::abs(camera.projection[3][3]) > 0.5f;
        return settings.frame.depth_of_field.enabled && settings.render_scene &&
               settings.spectral_path_tracing.mode != SpectralRenderMode::FullPathTracing && !orthographic;
    }

    f32 circle_of_confusion_px(const RenderSettings::DepthOfFieldSettings &lens, f32 view_depth, f32 image_height_px) noexcept {
        const f32 focus = std::max(finite_or(lens.focus_distance, 10.0f), 0.01f);
        const f32 depth = std::max(view_depth, 1.0e-4f);
        return blur_scale_px(lens, image_height_px) * (depth - focus) / depth;
    }

    u32 depth_of_field_sample_count(u32 quality) noexcept {
        constexpr u32 counts[] = {24, 40, 64};
        return counts[std::min(quality, 2u)];
    }

    Core::RendererExpected<RenderGraphTextureHandle> add_depth_of_field_passes(
        Renderer &renderer, RenderGraph &graph, std::vector<RHI::BindGroupHandle> &transient_bind_groups,
        std::vector<RHI::BufferHandle> &transient_buffers, RHI::RhiDevice &device, const DepthOfFieldDescription &description,
        const RenderGraphSettings &settings) {
        if (!description.scene_color || !description.depth || description.output_format == RHI::Format::Undefined) {
            return std::unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                              "Depth of field needs the scene colour, the scene depth and the output format."});
        }
        const auto prefilter_kernel = renderer.prepare_compute_kernel(kernel_description("dof_prefilter", "prefilterMain"));
        if (!prefilter_kernel) return std::unexpected(prefilter_kernel.error());
        const auto tile_kernel = renderer.prepare_compute_kernel(kernel_description("dof_tile_max", "tileMaxMain"));
        if (!tile_kernel) return std::unexpected(tile_kernel.error());
        const auto gather_kernel = renderer.prepare_compute_kernel(kernel_description("dof_gather", "gatherMain"));
        if (!gather_kernel) return std::unexpected(gather_kernel.error());
        const auto composite_kernel = renderer.prepare_compute_kernel(kernel_description("dof_composite", "compositeMain"));
        if (!composite_kernel) return std::unexpected(composite_kernel.error());

        const RenderSettings::DepthOfFieldSettings &lens = settings.frame.depth_of_field;
        const Core::Extent2D full = description.extent;
        const glm::uvec2 half{std::max((full.x + 1u) / 2u, 1u), std::max((full.y + 1u) / 2u, 1u)};
        const glm::uvec2 tiles{(half.x + kTileSize - 1u) / kTileSize, (half.y + kTileSize - 1u) / kTileSize};

        const f32 near_plane = std::max(finite_or(description.camera.near_plane, 0.01f), 1.0e-4f);
        const f32 far_plane = std::max(finite_or(description.camera.far_plane, 1000.0f), near_plane * 1.001f);
        const f32 max_blur = std::clamp(finite_or(lens.max_blur_px, 24.0f), 1.0f, 128.0f);
        const f32 blades = static_cast<f32>(std::clamp(lens.blades, 3u, 32u));
        const DofConstants constants{
            .extents = glm::uvec4{full.x, full.y, half.x, half.y},
            .depth = glm::vec4{near_plane * far_plane / (far_plane - near_plane), far_plane / (far_plane - near_plane),
                               std::max(finite_or(lens.focus_distance, 10.0f), 0.01f), blur_scale_px(lens, static_cast<f32>(full.y))},
            .lens = glm::vec4{max_blur, blades, glm::radians(finite_or(lens.blade_rotation_degrees, 0.0f)), lens.near_field ? 1.0f : 0.0f},
            .gather = glm::uvec4{depth_of_field_sample_count(lens.quality), tiles.x, tiles.y, 0u},
        };
        auto constants_buffer = upload_transient_buffer(device, transient_buffers, RHI::BufferUsage::Uniform,
                                                        std::as_bytes(std::span<const DofConstants>{&constants, 1}), "depth of field constants");
        if (!constants_buffer) return std::unexpected(constants_buffer.error());
        const RHI::BufferHandle dof_buffer = *constants_buffer;

        constexpr RHI::TextureUsage storage_sampled = RHI::TextureUsage::Storage | RHI::TextureUsage::Sampled;
        const auto texture = [&](RHI::Format format, glm::uvec2 size, const char *label) {
            return graph.create_texture(RenderGraphTextureDesc{
                .format = format,
                .extent = RHI::Extent3D{.width = size.x, .height = size.y, .depth_or_layers = 1},
                .usage = storage_sampled,
                .label = label});
        };
        const RenderGraphTextureHandle color_half = texture(RHI::Format::RGBA16Float, half, "dof half colour");
        const RenderGraphTextureHandle coc_half = texture(RHI::Format::RGBA16Float, half, "dof half circle of confusion");
        const RenderGraphTextureHandle tile_max = texture(RHI::Format::RGBA16Float, tiles, "dof tile max");
        const RenderGraphTextureHandle far_field = texture(RHI::Format::RGBA16Float, half, "dof far field");
        const RenderGraphTextureHandle near_field = texture(RHI::Format::RGBA16Float, half, "dof near field");
        const RenderGraphTextureHandle destination = graph.create_texture(RenderGraphTextureDesc{
            .format = description.output_format,
            .extent = description.output_extent,
            .usage = RHI::TextureUsage::ColorAttachment | storage_sampled | RHI::TextureUsage::TransferSrc | RHI::TextureUsage::TransferDst,
            .label = "depth of field scene colour"});

        const ComputeBufferBinding constants_binding{.name = "dof", .buffer = dof_buffer, .size = sizeof(DofConstants)};

        graph.add_compute_pass("dof prefilter"_ustr)
            .add_sampled_texture(description.scene_color)
            .add_sampled_texture(description.depth)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = color_half, .read = false, .write = true})
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = coc_half, .read = false, .write = true})
            .set_execute([&renderer, &transient_bind_groups, kernel = *prefilter_kernel, constants_binding, scene = description.scene_color,
                          depth = description.depth, color_half, coc_half, half](RenderGraphComputeContext &context) -> Core::RendererResult {
                const ComputeBinding bindings[] = {{"sceneColor", context.texture(scene).default_view},
                                                   {"sceneDepth", context.texture(depth).default_view},
                                                   {"colorOut", context.texture(color_half).default_view},
                                                   {"cocOut", context.texture(coc_half).default_view}};
                return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, {},
                                                      glm::uvec3{(half.x + 7u) / 8u, (half.y + 7u) / 8u, 1u}, transient_bind_groups,
                                                      std::span<const ComputeBufferBinding>{&constants_binding, 1});
            });

        graph.add_compute_pass("dof tile max"_ustr)
            .add_sampled_texture(coc_half)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = tile_max, .read = false, .write = true})
            .set_execute([&renderer, &transient_bind_groups, kernel = *tile_kernel, constants_binding, coc_half, tile_max,
                          tiles](RenderGraphComputeContext &context) -> Core::RendererResult {
                const ComputeBinding bindings[] = {{"cocHalf", context.texture(coc_half).default_view},
                                                   {"tileMaxOut", context.texture(tile_max).default_view}};
                return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, {},
                                                      glm::uvec3{(tiles.x + 7u) / 8u, (tiles.y + 7u) / 8u, 1u}, transient_bind_groups,
                                                      std::span<const ComputeBufferBinding>{&constants_binding, 1});
            });

        graph.add_compute_pass("dof gather"_ustr)
            .add_sampled_texture(color_half)
            .add_sampled_texture(coc_half)
            .add_sampled_texture(tile_max)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = far_field, .read = false, .write = true})
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = near_field, .read = false, .write = true})
            .set_execute([&renderer, &transient_bind_groups, kernel = *gather_kernel, constants_binding, color_half, coc_half, tile_max,
                          far_field, near_field, half](RenderGraphComputeContext &context) -> Core::RendererResult {
                const ComputeBinding bindings[] = {{"colorHalf", context.texture(color_half).default_view},
                                                   {"cocHalf", context.texture(coc_half).default_view},
                                                   {"tileMax", context.texture(tile_max).default_view},
                                                   {"farOut", context.texture(far_field).default_view},
                                                   {"nearOut", context.texture(near_field).default_view}};
                return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, {},
                                                      glm::uvec3{(half.x + 7u) / 8u, (half.y + 7u) / 8u, 1u}, transient_bind_groups,
                                                      std::span<const ComputeBufferBinding>{&constants_binding, 1});
            });

        graph.add_compute_pass("dof composite"_ustr)
            .add_sampled_texture(description.scene_color)
            .add_sampled_texture(description.depth)
            .add_sampled_texture(far_field)
            .add_sampled_texture(near_field)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = destination, .read = false, .write = true})
            .set_execute([&renderer, &transient_bind_groups, kernel = *composite_kernel, constants_binding, scene = description.scene_color,
                          depth = description.depth, far_field, near_field, destination,
                          full](RenderGraphComputeContext &context) -> Core::RendererResult {
                const ComputeBinding bindings[] = {{"sceneColor", context.texture(scene).default_view},
                                                   {"sceneDepth", context.texture(depth).default_view},
                                                   {"farField", context.texture(far_field).default_view},
                                                   {"nearField", context.texture(near_field).default_view},
                                                   {"colorOut", context.texture(destination).default_view}};
                return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, {},
                                                      glm::uvec3{(full.x + 7u) / 8u, (full.y + 7u) / 8u, 1u}, transient_bind_groups,
                                                      std::span<const ComputeBufferBinding>{&constants_binding, 1});
            });
        return destination;
    }

    Core::RendererResult build_depth_of_field_feature(FrameBuildContext &frame) {
        if (frame.direct_overlay_presentation || !depth_of_field_active(frame.settings, frame.camera) || frame.transient_buffers == nullptr) {
            return {};
        }
        auto focused = add_depth_of_field_passes(
            frame.renderer, frame.graph, frame.transient_bind_groups, *frame.transient_buffers, frame.device,
            DepthOfFieldDescription{
                .scene_color = frame.resources.texture<RenderGraphSemantics::SceneHdrColor>(),
                .depth = frame.resources.texture<RenderGraphSemantics::ResolvedSceneDepth>(),
                .extent = frame.module.render_extent,
                .output_extent = frame.module.render_texture_extent(),
                .output_format = frame.deferred_formats.scene_color,
                .camera = frame.camera,
            },
            frame.settings);
        if (!focused) {
            return std::unexpected(focused.error());
        }
        frame.resources.publish_texture<RenderGraphSemantics::SceneHdrColor>(*focused);
        return {};
    }

} // namespace SFT::Renderer
