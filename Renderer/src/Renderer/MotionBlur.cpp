#include <Renderer/MotionBlur.hpp>

#include <algorithm>
#include <span>

#include <Renderer/RendererModule.hpp>

namespace SFT::Renderer {

    namespace {

        // Must match the push-constant blocks in Shaders/motion_blur_{tile_max,neighbor_max,gather}.slang.
        struct TileMaxConstants {
            glm::uvec2 render_extent;
            u32 tile_size;
        };
        static_assert(sizeof(TileMaxConstants) == 12);

        struct NeighborMaxConstants {
            glm::uvec2 tile_extent;
        };
        static_assert(sizeof(NeighborMaxConstants) == 8);

        struct GatherConstants {
            glm::vec2 render_extent;
            f32 intensity;
            f32 shutter_fraction;
            f32 max_blur_radius_px;
            f32 fg_bg_weight_bias;
            u32 sample_count;
            u32 tile_size;
            u32 camera_motion_only;
        };
        static_assert(sizeof(GatherConstants) == 36);

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

    } // namespace

    glm::uvec2 motion_blur_tile_extent(Core::Extent2D extent, u32 tile_size_px) noexcept {
        const u32 tile_size = std::max(tile_size_px, 1u);
        return glm::uvec2{(extent.x + tile_size - 1) / tile_size, (extent.y + tile_size - 1) / tile_size};
    }

    Core::RendererExpected<RenderGraphTextureHandle> add_motion_blur_passes(
        Renderer &renderer, RenderGraph &graph, std::vector<RHI::BindGroupHandle> &transient_bind_groups,
        const MotionBlurDescription &description, const RenderGraphSettings &settings) {
        if (!description.source || !description.motion || !description.depth) {
            return std::unexpected(Core::GraphicsBackendError{
                Core::GraphicsBackendErrorCode::OperationFailed,
                "Motion blur requires a colour, a motion-vector and a depth render-graph texture."});
        }
        if (description.output_format == RHI::Format::Undefined) {
            return std::unexpected(Core::GraphicsBackendError{
                Core::GraphicsBackendErrorCode::OperationFailed, "Motion blur needs the output format."});
        }
        const auto tile_max_kernel = renderer.prepare_compute_kernel(kernel_description("motion_blur_tile_max", "tileMaxMain"));
        if (!tile_max_kernel) return std::unexpected(tile_max_kernel.error());
        const auto neighbor_kernel = renderer.prepare_compute_kernel(kernel_description("motion_blur_neighbor_max", "neighborMaxMain"));
        if (!neighbor_kernel) return std::unexpected(neighbor_kernel.error());
        const auto gather_kernel = renderer.prepare_compute_kernel(kernel_description("motion_blur_gather", "gatherMain"));
        if (!gather_kernel) return std::unexpected(gather_kernel.error());

        const MotionBlurSettings &blur = settings.motion_blur;
        const glm::uvec2 render_extent{description.extent.x, description.extent.y};
        const u32 tile_size = std::max(blur.tile_size_px, 1u);
        const glm::uvec2 tile_extent = motion_blur_tile_extent(description.extent, tile_size);

        constexpr RHI::TextureUsage velocity_usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::Storage;
        const RenderGraphTextureHandle tile_max_velocity = graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::RG16Float,
            .extent = RHI::Extent3D{.width = tile_extent.x, .height = tile_extent.y, .depth_or_layers = 1},
            .usage = velocity_usage,
            .label = "motion blur tile-max velocity",
        });
        const RenderGraphTextureHandle dilated_velocity = graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::RG16Float,
            .extent = RHI::Extent3D{.width = tile_extent.x, .height = tile_extent.y, .depth_or_layers = 1},
            .usage = velocity_usage,
            .label = "motion blur dilated velocity",
        });
        const RenderGraphTextureHandle destination = graph.create_texture(RenderGraphTextureDesc{
            .format = description.output_format,
            .extent = description.output_extent,
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled | RHI::TextureUsage::Storage |
                     RHI::TextureUsage::TransferSrc | RHI::TextureUsage::TransferDst,
            .label = "motion blur gather target",
        });

        graph.add_compute_pass("motion blur tile max"_ustr)
            .add_sampled_texture(description.motion)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = tile_max_velocity, .read = false, .write = true})
            .set_execute([&renderer, &transient_bind_groups, kernel = *tile_max_kernel, motion = description.motion, tile_max_velocity,
                          render_extent, tile_size, tile_extent](RenderGraphComputeContext &context) -> Core::RendererResult {
                const ComputeBinding bindings[] = {
                    {"gbufferMotion", context.texture(motion).default_view},
                    {"tileMaxVelocityOut", context.texture(tile_max_velocity).default_view},
                };
                const TileMaxConstants constants{.render_extent = render_extent, .tile_size = tile_size};
                return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, bytes_of(constants),
                                                      glm::uvec3{tile_extent, 1u}, transient_bind_groups);
            });

        graph.add_compute_pass("motion blur neighbor max"_ustr)
            .add_sampled_texture(tile_max_velocity)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = dilated_velocity, .read = false, .write = true})
            .set_execute([&renderer, &transient_bind_groups, kernel = *neighbor_kernel, tile_max_velocity, dilated_velocity,
                          tile_extent](RenderGraphComputeContext &context) -> Core::RendererResult {
                const ComputeBinding bindings[] = {
                    {"tileMaxVelocity", context.texture(tile_max_velocity).default_view},
                    {"dilatedVelocityOut", context.texture(dilated_velocity).default_view},
                };
                const NeighborMaxConstants constants{.tile_extent = tile_extent};
                return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, bytes_of(constants),
                                                      glm::uvec3{(tile_extent.x + 7u) / 8u, (tile_extent.y + 7u) / 8u, 1u},
                                                      transient_bind_groups);
            });

        graph.add_compute_pass("motion blur gather"_ustr)
            .add_sampled_texture(description.source)
            .add_sampled_texture(description.motion)
            .add_sampled_texture(description.depth)
            .add_sampled_texture(dilated_velocity)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = destination, .read = false, .write = true})
            .set_execute([&renderer, &transient_bind_groups, kernel = *gather_kernel, source = description.source,
                          motion = description.motion, depth = description.depth, dilated_velocity, destination, render_extent,
                          tile_size, blur](RenderGraphComputeContext &context) -> Core::RendererResult {
                const ComputeBinding bindings[] = {
                    {"sceneColor", context.texture(source).default_view},
                    {"gbufferMotion", context.texture(motion).default_view},
                    {"gbufferDepth", context.texture(depth).default_view},
                    {"dilatedVelocity", context.texture(dilated_velocity).default_view},
                    {"sceneColorOut", context.texture(destination).default_view},
                };
                const GatherConstants constants{
                    .render_extent = glm::vec2{static_cast<f32>(render_extent.x), static_cast<f32>(render_extent.y)},
                    .intensity = blur.intensity,
                    .shutter_fraction = blur.shutter_angle_degrees / 360.0f,
                    .max_blur_radius_px = blur.max_blur_radius_px,
                    .fg_bg_weight_bias = blur.background_foreground_weight_bias,
                    .sample_count = std::max(blur.sample_count, 1u),
                    .tile_size = tile_size,
                    .camera_motion_only = blur.camera_motion_only ? 1u : 0u,
                };
                return renderer.record_compute_kernel(context.compute_pass(), kernel, bindings, bytes_of(constants),
                                                      glm::uvec3{(render_extent.x + 7u) / 8u, (render_extent.y + 7u) / 8u, 1u},
                                                      transient_bind_groups);
            });

        return destination;
    }

} // namespace SFT::Renderer
