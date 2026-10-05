#pragma once

#include <Foundation/Foundation.hpp>

#include <vector>

#include <glm/vec2.hpp>

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/FramePipeline.hpp>
#include <Renderer/HistoryTextures.hpp>
#include <Renderer/Scene.hpp>

namespace SFT::Renderer {

    class Renderer;

    /// Indirect specular light, in two layers (`Shaders/sturdy_specular.slang` documents the maths):
    ///
    /// * the **environment**: the atmosphere's sky-view LUT prefiltered into an atlas with one octahedral map per
    ///   roughness level, refreshed a level per frame (the sky moves slowly), so every surface reflects something and
    ///   metals are never black;
    /// * **screen-space reflections**: one deterministic ray per pixel along the specular direction, walked through a min-depth
    ///   pyramid built in a single dispatch. Roughness is *filtered*, not sampled: last frame's lit colour is kept as a chain of
    ///   five blurred half-resolution levels (dual-filter downsample) and the trace reads the level the specular cone has
    ///   widened to at the hit (cone tracing). With nothing random there is nothing to denoise or accumulate: no noise, no
    ///   temporal smear, no ghosting. Misses and off-screen reflections fall back to the environment through the
    ///   confidence in the alpha channel.
    ///
    /// Total cost: a full-resolution trace (one ray, <= `max_steps` pyramid steps), the pyramid dispatch, and four small blur
    /// passes over half-resolution levels; plus a 192x128 prefilter slice. No ray tracing hardware, runs on every backend.
    /// Written only against public services -- `Renderer::prepare_compute_kernel`/`record_compute_kernel`, the graph
    /// blackboard and a `HistoryTextureCache` -- so it can be fed other inputs, replaced or copied.
    struct ReflectionDescription {
        RenderGraphTextureHandle depth{};       // device depth, `extent`
        RenderGraphTextureHandle normal{};      // octahedral world normal
        RenderGraphTextureHandle material{};    // roughness in x
        RenderGraphTextureHandle motion{};      // uv motion (previous = uv - motion)
        RenderGraphTextureHandle sky_view_lut{};
        RenderGraphBufferHandle atmosphere_constants{};
        RenderGraphBufferHandle lighting_constants{};
        /// Where the colour chain levels are published (`RenderGraphSemantics::ReflectionColorLevel*`) for the history pass.
        RenderGraphBlackboard &resources;
        Core::Extent2D extent{};
        CameraView camera{};
        u64 frame_index = 0;
        /// Which persistent histories to use (the engine passes the window id).
        u64 history_key = 0;
    };

    struct ReflectionResult {
        /// The prefiltered environment atlas (always produced).
        RenderGraphTextureHandle environment{};
        /// Screen-space reflections (rgb radiance before exposure, a confidence); empty when SSR is off.
        RenderGraphTextureHandle screen_space{};
    };

    /// Whether the lighting pass will use the prefiltered environment this frame.
    [[nodiscard]] bool reflections_environment_active(const RenderGraphSettings &settings) noexcept;
    /// Whether the lighting pass will use screen-space reflections: enabled, a perspective camera, a raster scene.
    [[nodiscard]] bool reflections_screen_space_active(const RenderGraphSettings &settings, const CameraView &camera) noexcept;

    /// Which environment-atlas levels to refresh this frame: the mirror level every frame plus one rough level in turn,
    /// or every level when the atlas holds nothing yet.
    [[nodiscard]] u32 reflection_environment_level_mask(u64 frame_index, bool atlas_has_contents) noexcept;

    /// Size of the min-depth pyramid atlas for a half-resolution extent (level 0 at the origin, the coarser levels stacked
    /// in a column to its right; see `Shaders/ssr_common.slang`).
    [[nodiscard]] glm::uvec2 reflection_pyramid_extent(glm::uvec2 half_extent) noexcept;

    /// Adds the environment prefilter and, when active, the pyramid / trace / resolve / temporal passes.
    [[nodiscard]] Core::RendererExpected<ReflectionResult> add_reflection_passes(
        Renderer &renderer, RHI::RhiDevice &device, HistoryTextureCache &histories, RenderGraph &graph,
        std::vector<RHI::BindGroupHandle> &transient_bind_groups, std::vector<RHI::BufferHandle> &transient_buffers,
        const ReflectionDescription &description, const RenderGraphSettings &settings);

    /// Size of one level of the blurred colour chain (level 0 is the half-resolution extent).
    [[nodiscard]] glm::uvec2 reflection_color_level_extent(glm::uvec2 half_extent, u32 level) noexcept;

    /// Adds the passes that store this frame's lit colour (scene-linear, multiplied by `exposure`) as the blurred colour chain
    /// for the next frame's trace. Writes through the handles `add_reflection_passes` published on `resources`.
    [[nodiscard]] Core::RendererResult add_reflection_history_pass(
        Renderer &renderer, RHI::RhiDevice &device, HistoryTextureCache &histories, RenderGraph &graph,
        std::vector<RHI::BindGroupHandle> &transient_bind_groups, RenderGraphBlackboard &resources,
        RenderGraphTextureHandle scene_color, Core::Extent2D extent, f32 exposure, u64 history_key, u64 frame_index);

    /// The engine's `reflections` feature (Scene stage, before `lighting`): publishes ReflectionEnvironment,
    /// ScreenSpaceReflections and the ReflectionColorLevel* chain.
    [[nodiscard]] Core::RendererResult build_reflections_feature(FrameBuildContext &frame);
    /// The engine's `reflections_history` feature (Scene stage, once the lit scene colour is final).
    [[nodiscard]] Core::RendererResult build_reflections_history_feature(FrameBuildContext &frame);

} // namespace SFT::Renderer
