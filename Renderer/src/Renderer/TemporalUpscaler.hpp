#pragma once

#include <Foundation/Foundation.hpp>

#include <vector>

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/FramePipeline.hpp>
#include <Renderer/HistoryTextures.hpp>
#include <Renderer/Scene.hpp>

namespace SFT::Renderer {

    class Renderer;

    /// The engine's temporal anti-aliasing / upscaler (`Shaders/temporal_upscale.slang`): reconstructs an output-
    /// resolution image from the jittered render-resolution frame and the previous output. It is the renderer's single
    /// "temporal upscaler" slot: a vendor upscaler (FSR, DLSS, XeSS) is integrated by replacing the `temporal_upscale`
    /// frame feature with one that honours the same contract -- consume SceneHdrColor, ResolvedSceneDepth and
    /// GBufferMotion at render resolution plus `TemporalUpscalerSettings::jitter_uv`, publish an output-resolution
    /// SceneHdrColor.
    struct TemporalUpscaleDescription {
        RenderGraphTextureHandle color{};   // scene-linear HDR, `input_extent`
        RenderGraphTextureHandle depth{};   // device depth, `input_extent`
        RenderGraphTextureHandle motion{};  // G-buffer motion, `input_extent`
        Core::Extent2D input_extent{};
        Core::Extent2D output_extent{};
        RHI::Format output_format = RHI::Format::RGBA16Float;
        u64 frame_index = 0;
        u64 history_key = 0;
    };

    /// Adds the pass and returns the output-resolution colour. Reads `settings.frame.temporal_upscaler`.
    [[nodiscard]] Core::RendererExpected<RenderGraphTextureHandle> add_temporal_upscale_pass(
        Renderer &renderer, RHI::RhiDevice &device, HistoryTextureCache &histories, RenderGraph &graph,
        std::vector<RHI::BindGroupHandle> &transient_bind_groups, const TemporalUpscaleDescription &description,
        const RenderGraphSettings &settings);

    /// The engine's `temporal_upscale` feature (Post stage, before exposure and tone mapping).
    [[nodiscard]] Core::RendererResult build_temporal_upscale_feature(FrameBuildContext &frame);

    /// The size of the current SceneHdrColor: the render extent, or the output extent once a temporal upscaler ran.
    [[nodiscard]] Core::Extent2D scene_color_extent(const FrameBuildContext &frame) noexcept;

} // namespace SFT::Renderer
