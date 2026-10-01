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

    /// The engine's screen-space indirect lighting (SSILVB, `Shaders/ssgi_*.slang`): a half-resolution visibility-
    /// bitmask trace against the depth buffer that gathers radiance from last frame's lit image, a depth-aware
    /// upsample with temporal accumulation, and a pass that keeps the half-resolution radiance for the next frame.
    /// Written only against `Renderer::prepare_compute_kernel` / `record_compute_kernel`, graph textures and a
    /// `HistoryTextureCache`, so it can be used as-is, fed other inputs, or copied.
    ///
    /// The result is irradiance / pi (demodulated), the same quantity the ray-traced GI produces: the lighting pass
    /// multiplies it by the diffuse albedo.
    struct ScreenSpaceGiDescription {
        RenderGraphTextureHandle depth{};   // device depth (Depth32Float), `extent`
        RenderGraphTextureHandle normal{};  // octahedral world normal
        RenderGraphTextureHandle motion{};  // uv motion (previous = uv - motion)
        Core::Extent2D extent{};
        CameraView camera{};
        /// Camera lens strength (vertex-warp fisheye), 0 for none.
        f32 lens_strength = 0.0f;
        u64 frame_index = 0;
        /// Which persistent histories to use (the engine passes the window id).
        u64 history_key = 0;
    };

    struct ScreenSpaceGiResult {
        /// Full-resolution irradiance / pi (rgb) and linear view depth (a).
        RenderGraphTextureHandle irradiance{};
        /// The imported half-resolution radiance history the trace read; `add_screen_space_gi_history_pass` writes the
        /// next frame's into it.
        RenderGraphTextureHandle radiance_history{};
    };

    /// Whether the engine's lighting pass will consume screen-space GI this frame: enabled, ReSTIR GI not in use, a
    /// perspective camera, and a raster (not fully path-traced) frame.
    [[nodiscard]] bool screen_space_gi_active(const RenderGraphSettings &settings, const CameraView &camera) noexcept;

    /// Adds the trace and resolve passes. Reads `settings.screen_space_gi`.
    [[nodiscard]] Core::RendererExpected<ScreenSpaceGiResult> add_screen_space_gi_passes(
        Renderer &renderer, RHI::RhiDevice &device, HistoryTextureCache &histories, RenderGraph &graph,
        std::vector<RHI::BindGroupHandle> &transient_bind_groups, const ScreenSpaceGiDescription &description,
        const RenderGraphSettings &settings);

    /// Adds the pass that stores this frame's lit colour (`scene_color`, scene-linear, multiplied by `exposure`) into
    /// `radiance_history` for the next frame.
    [[nodiscard]] Core::RendererResult add_screen_space_gi_history_pass(
        Renderer &renderer, RHI::RhiDevice &device, HistoryTextureCache &histories, RenderGraph &graph,
        std::vector<RHI::BindGroupHandle> &transient_bind_groups, RenderGraphTextureHandle scene_color,
        RenderGraphTextureHandle radiance_history, Core::Extent2D extent, f32 exposure, u64 history_key, u64 frame_index,
        const RenderGraphSettings &settings);

    /// The engine's `screen_space_gi` feature (Scene stage, after `global_illumination`): publishes SurfelIrradiance
    /// and ScreenSpaceGiRadianceHistory when `screen_space_gi_active`.
    [[nodiscard]] Core::RendererResult build_screen_space_gi_feature(FrameBuildContext &frame);
    /// The engine's `screen_space_gi_history` feature (Scene stage, after the scene colour is final).
    [[nodiscard]] Core::RendererResult build_screen_space_gi_history_feature(FrameBuildContext &frame);

} // namespace SFT::Renderer
