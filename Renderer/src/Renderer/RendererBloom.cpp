#include <Foundation/Foundation.hpp>

#include <vector>

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/Bloom.hpp>
#include <Renderer/RendererModule.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Renderer {

    /// Adds render-graph nodes that bloom `source` for `Renderer` using the supplied arguments — see
    /// the declaration's own doc comment (RendererModule.hpp) for the full contract.
    ///
    /// @param graph The current frame's render graph.
    /// @param source The element's own already-rendered color texture (with alpha) to bloom.
    /// @param source_extent `source`'s pixel size.
    /// @param output Destination for the final composite; caller-provided (typically an imported
    ///        texture) rather than allocated here.
    /// @param format Format used for the resource, render target, or conversion.
    /// @param settings Configuration values controlling the operation.
    /// @param out_transient_bind_groups Bind group used or affected by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::add_ui_glow_bloom_passes(
        RenderGraph &graph, RenderGraphTextureHandle source, Core::Extent2D source_extent,
        RenderGraphTextureHandle output, RHI::Format format, const RenderGraphSettings &settings,
        std::vector<RHI::BindGroupHandle> &out_transient_bind_groups) {
        ZoneScopedN("Renderer::add_ui_glow_bloom_passes");
        // A small fixed 3-level, halving pyramid; the composite is additive (`source + bloom * intensity`)
        // because a UI glow keeps its crisp base line and adds a halo on top instead of fading it out.
        auto composite = add_bloom_passes(
            *this, graph, out_transient_bind_groups,
            BloomDescription{
                .source = source,
                .source_extent = source_extent,
                .level_format = format,
                .max_levels = 3,
                .downsample_ratio = 2.0f,
                .minimum_level_axis = 1,
                .output = output,
                .output_format = format,
                .additive_composite = true,
                .label_prefix = "ui glow bloom",
            },
            settings);
        if (!composite.has_value()) {
            return std::unexpected(composite.error());
        }
        return {};
    }

} // namespace SFT::Renderer
