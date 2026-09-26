#pragma once

#include <Foundation/Foundation.hpp>

#include <vector>

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/FramePipeline.hpp>
#include <Renderer/Scene.hpp>

namespace SFT::Renderer {

    class Renderer;

    /// The engine's motion blur: a tile-max reduction of the motion vectors, a neighbour-max dilation, and a
    /// line-integral gather (three compute passes, `Shaders/motion_blur_*.slang`). It is written only against
    /// `Renderer::prepare_compute_kernel` / `record_compute_kernel` and graph-transient textures, so an
    /// application can use it as-is, feed it its own velocity/depth textures, or copy it.
    struct MotionBlurDescription {
        /// Scene-linear colour to blur.
        RenderGraphTextureHandle source{};
        /// Per-pixel motion vectors (RG, UV space) and scene depth, both at `extent`.
        RenderGraphTextureHandle motion{};
        RenderGraphTextureHandle depth{};
        Core::Extent2D extent{};
        RHI::Extent3D output_extent{};
        RHI::Format output_format = RHI::Format::Undefined;
    };

    /// Number of tiles the tile-max pass produces per axis.
    [[nodiscard]] glm::uvec2 motion_blur_tile_extent(Core::Extent2D extent, u32 tile_size_px) noexcept;

    /// Adds the passes and returns the blurred colour texture. Reads `settings.motion_blur`.
    [[nodiscard]] Core::RendererExpected<RenderGraphTextureHandle> add_motion_blur_passes(
        Renderer &renderer, RenderGraph &graph, std::vector<RHI::BindGroupHandle> &transient_bind_groups,
        const MotionBlurDescription &description, const RenderGraphSettings &settings);

} // namespace SFT::Renderer
