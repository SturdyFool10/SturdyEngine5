#pragma once

#include <Foundation/Foundation.hpp>

#include <vector>

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/FramePipeline.hpp>
#include <Renderer/Scene.hpp>

namespace SFT::Renderer {

    class Renderer;

    /// The engine's bloom, expressed as ordinary fullscreen effects: a Karis-weighted 13-tap prefilter, a
    /// progressive downsample pyramid, a tent-filter upsample chain accumulated with constant-colour
    /// blending, and a composite back onto the source. Shaders are `Shaders/fullscreen_bloom.slang` and
    /// `Shaders/fullscreen_bloom_composite.slang`. Nothing here is private to the renderer, so an
    /// application can use it as-is, call it on any texture (the UI's glow does), or copy it.
    struct BloomDescription {
        /// Scene-linear texture to bloom.
        RenderGraphTextureHandle source{};
        Core::Extent2D source_extent{};
        /// Format of the pyramid levels (packed HDR by default).
        RHI::Format level_format = RHI::Format::RG11B10Float;
        u32 max_levels = 6;
        /// Size ratio between successive levels, clamped to [1.25, 2].
        f32 downsample_ratio = 1.61803398875f;
        /// Levels stop being added once an axis would fall below this many pixels (the first level is
        /// always added).
        u32 minimum_level_axis = 4;
        /// Destination of the composite. When invalid, a transient texture of `output_extent` /
        /// `output_format` is created.
        RenderGraphTextureHandle output{};
        RHI::Extent3D output_extent{};
        RHI::Format output_format = RHI::Format::Undefined;
        /// Add the bloom on top of the source (`source + bloom * intensity`) rather than interpolating
        /// towards it. Thresholded bloom is always additive; the no-threshold mode conserves a constant
        /// image by interpolating.
        bool additive_composite = true;
        std::string_view label_prefix = "bloom";
    };

    /// Extents of the pyramid levels `add_bloom_passes` builds for a source of `extent`.
    [[nodiscard]] std::vector<Core::Extent2D> plan_bloom_levels(Core::Extent2D extent, u32 max_levels, f32 downsample_ratio,
                                                                u32 minimum_level_axis = 4);

    /// Adds the bloom passes to `graph` and returns the texture holding the composite. Uses only the
    /// renderer's public fullscreen-effect services, which is what a replacement bloom would use.
    ///
    /// @param transient_bind_groups Frame-transient bind groups (`FrameBuildContext::transient_bind_groups`).
    [[nodiscard]] Core::RendererExpected<RenderGraphTextureHandle> add_bloom_passes(
        Renderer &renderer, RenderGraph &graph, std::vector<RHI::BindGroupHandle> &transient_bind_groups,
        const BloomDescription &description, const RenderGraphSettings &settings);

} // namespace SFT::Renderer
