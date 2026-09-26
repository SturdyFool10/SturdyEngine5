#include <Renderer/Bloom.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <string>

#include <Renderer/FullscreenPass.hpp>
#include <Renderer/RendererModule.hpp>

namespace SFT::Renderer {

    namespace {

        // Must match `BloomConstants` in Shaders/fullscreen_bloom.slang.
        struct BloomConstants {
            glm::vec2 source_texel_size{1.0f};
            f32 threshold = 1.0f;
            f32 soft_knee = 0.5f;
            glm::vec2 filter_scale{1.0f};
        };
        static_assert(sizeof(BloomConstants) == 24);

        // Must match `BloomCompositeConstants` in Shaders/fullscreen_bloom_composite.slang.
        struct BloomCompositeConstants {
            f32 bloom_intensity = 0.0f;
            u32 threshold_enabled = 0;
        };
        static_assert(sizeof(BloomCompositeConstants) == 8);

        template <class Constants>
        void set_constants(CustomPostProcessEffect &effect, const Constants &constants) {
            const std::span<const std::byte> bytes = std::as_bytes(std::span<const Constants>{&constants, 1});
            effect.push_constants.assign(bytes.begin(), bytes.end());
        }

        [[nodiscard]] CustomPostProcessEffect bloom_effect(std::string entry_point, const BloomConstants &constants, std::string_view label) {
            CustomPostProcessEffect effect;
            effect.shader_path = "Shaders/fullscreen_bloom.slang";
            effect.module_name = "fullscreen_bloom";
            effect.fragment_entry_point = std::move(entry_point);
            effect.label = UString{label};
            set_constants(effect, constants);
            return effect;
        }

        [[nodiscard]] Core::GraphicsBackendError bloom_error(std::string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

    } // namespace

    std::vector<Core::Extent2D> plan_bloom_levels(Core::Extent2D extent, u32 max_levels, f32 downsample_ratio, u32 minimum_level_axis) {
        max_levels = std::clamp(max_levels, 1u, 12u);
        downsample_ratio = std::isfinite(downsample_ratio) ? std::clamp(downsample_ratio, 1.25f, 2.0f) : 1.61803398875f;
        std::vector<Core::Extent2D> extents;
        Core::Extent2D source_extent = extent;
        for (u32 level = 0; level < max_levels; ++level) {
            const Core::Extent2D level_extent = glm::max(
                Core::Extent2D{glm::floor(glm::dvec2{source_extent} / static_cast<f64>(downsample_ratio))}, Core::Extent2D{1u, 1u});
            if (!extents.empty() && (level_extent.x < minimum_level_axis || level_extent.y < minimum_level_axis)) {
                break;
            }
            extents.push_back(level_extent);
            if (level_extent == source_extent) {
                break;
            }
            source_extent = level_extent;
        }
        return extents;
    }

    Core::RendererExpected<RenderGraphTextureHandle> add_bloom_passes(Renderer &renderer, RenderGraph &graph,
                                                                       std::vector<RHI::BindGroupHandle> &transient_bind_groups,
                                                                       const BloomDescription &description,
                                                                       const RenderGraphSettings &settings) {
        if (!description.source) {
            return std::unexpected(bloom_error("Bloom needs a source texture."));
        }
        if (!description.output && description.output_format == RHI::Format::Undefined) {
            return std::unexpected(bloom_error("Bloom needs an output texture or an output format."));
        }
        const std::vector<Core::Extent2D> extents =
            plan_bloom_levels(description.source_extent, description.max_levels, description.downsample_ratio, description.minimum_level_axis);
        const std::string prefix{description.label_prefix};

        std::vector<RenderGraphTextureHandle> levels;
        levels.reserve(extents.size());
        for (const Core::Extent2D extent : extents) {
            levels.push_back(graph.create_texture(RenderGraphTextureDesc{
                .format = description.level_format,
                .extent = RHI::Extent3D{.width = extent.x, .height = extent.y, .depth_or_layers = 1},
                .mip_levels = 1,
                .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled,
                .label = "bloom level",
            }));
        }

        // Downsample: level 0 <- source (threshold + Karis prefilter), level n <- level n-1.
        for (usize level = 0; level < extents.size(); ++level) {
            const Core::Extent2D source_extent = level == 0 ? description.source_extent : extents[level - 1];
            const Core::Extent2D destination_extent = extents[level];
            const BloomConstants constants{
                .source_texel_size = glm::vec2{1.0f / static_cast<f32>(source_extent.x), 1.0f / static_cast<f32>(source_extent.y)},
                .threshold = settings.bloom_threshold,
                .soft_knee = settings.bloom_soft_knee,
                .filter_scale = glm::vec2{0.5f * static_cast<f32>(source_extent.x) / static_cast<f32>(destination_extent.x),
                                          0.5f * static_cast<f32>(source_extent.y) / static_cast<f32>(destination_extent.y)},
            };
            if (Core::RendererResult added = add_fullscreen_effect_pass(
                    renderer, graph, transient_bind_groups,
                    FullscreenPassDescription{
                        .label = prefix + " downsample",
                        .destination = levels[level],
                        .destination_format = description.level_format,
                        .extent = destination_extent,
                        .load_op = RHI::LoadOp::DontCare,
                        .source = level == 0 ? description.source : levels[level - 1],
                        .effect = bloom_effect(level == 0 ? "prefilterMain" : "downsampleMain", constants, prefix + " downsample"),
                    });
                !added.has_value()) {
                return std::unexpected(added.error());
            }
        }

        // Upsample: accumulate level n into level n-1 as coarse * scatter + fine * (1 - scatter).
        const f32 scatter = std::clamp(settings.bloom_scatter, 0.0f, 1.0f);
        for (usize level = extents.size(); level-- > 1;) {
            const Core::Extent2D source_extent = extents[level];
            const BloomConstants constants{
                .source_texel_size = glm::vec2{1.0f / static_cast<f32>(source_extent.x), 1.0f / static_cast<f32>(source_extent.y)},
                .threshold = settings.bloom_threshold,
                .soft_knee = settings.bloom_soft_knee,
                .filter_scale = glm::vec2{1.0f},
            };
            CustomPostProcessEffect effect = bloom_effect("upsampleMain", constants, prefix + " upsample");
            effect.blend = FullscreenBlend::ConstantMix;
            effect.blend_constant = scatter;
            if (Core::RendererResult added = add_fullscreen_effect_pass(
                    renderer, graph, transient_bind_groups,
                    FullscreenPassDescription{
                        .label = prefix + " upsample",
                        .destination = levels[level - 1],
                        .destination_format = description.level_format,
                        .extent = extents[level - 1],
                        .load_op = RHI::LoadOp::Load,
                        .source = levels[level],
                        .effect = std::move(effect),
                    });
                !added.has_value()) {
                return std::unexpected(added.error());
            }
        }

        RenderGraphTextureHandle output = description.output;
        if (!output) {
            output = graph.create_texture(RenderGraphTextureDesc{
                .format = description.output_format,
                .extent = description.output_extent,
                .mip_levels = 1,
                .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferSrc,
                .label = "bloom composite",
            });
        }
        CustomPostProcessEffect composite;
        composite.shader_path = "Shaders/fullscreen_bloom_composite.slang";
        composite.module_name = "fullscreen_bloom_composite";
        composite.fragment_entry_point = "fragmentMain";
        composite.extra_input_count = 1;
        composite.label = UString{prefix + " composite"};
        set_constants(composite, BloomCompositeConstants{
                                     .bloom_intensity = settings.bloom_intensity,
                                     .threshold_enabled = description.additive_composite ? 1u : 0u,
                                 });
        RHI::Format composite_format = description.output_format;
        if (composite_format == RHI::Format::Undefined) {
            return std::unexpected(bloom_error("Bloom needs the output format to build the composite pipeline."));
        }
        if (Core::RendererResult added = add_fullscreen_effect_pass(
                renderer, graph, transient_bind_groups,
                FullscreenPassDescription{
                    .label = prefix + " composite",
                    .destination = output,
                    .destination_format = composite_format,
                    .extent = description.source_extent,
                    .load_op = RHI::LoadOp::DontCare,
                    .source = description.source,
                    .extra_source = levels.front(),
                    .effect = std::move(composite),
                });
            !added.has_value()) {
            return std::unexpected(added.error());
        }
        return output;
    }

} // namespace SFT::Renderer
