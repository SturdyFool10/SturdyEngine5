#pragma once

#include <Foundation/Foundation.hpp>

#include <vector>

#include <glm/vec4.hpp>

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/FramePipeline.hpp>
#include <Renderer/HistoryTextures.hpp>
#include <Renderer/Scene.hpp>

namespace SFT::Renderer {

    class Renderer;

    /// Froxel volumetric fog (`Shaders/volumetric_fog_{inject,integrate,apply}.slang`): exponential height fog with
    /// optional wind-driven noise, lit by the sun (cascaded shadows), the ambient term and every clustered local light
    /// (spot/point shadows), temporally reprojected and composited onto the scene colour.
    ///
    /// Written only against public services -- `Renderer::prepare_compute_kernel`/`record_compute_kernel`, the graph
    /// blackboard and a `HistoryTextureCache` -- so it can be fed other inputs, replaced or copied. It lights with the
    /// same data as the deferred lighting pass: `RenderGraphSemantics::LightingConstants` and, when clustered lighting
    /// is on, `LocalLights` / `LightClusterRanges` / `LightClusterIndices`.
    struct VolumetricFogDescription {
        RenderGraphTextureHandle scene_color{};  // rgba16f scene-linear, exposure applied
        RenderGraphTextureHandle depth{};        // device depth, `extent`
        /// `ShadowLightingGpuData` (required) and the clustered light list (optional: without it local lights do not
        /// light the fog).
        RenderGraphBufferHandle lighting_constants{};
        RenderGraphBufferHandle local_lights{};
        RenderGraphBufferHandle cluster_ranges{};
        RenderGraphBufferHandle cluster_indices{};
        /// Shadow atlases, when the frame rendered them (otherwise the matching shadows are skipped).
        RenderGraphTextureHandle directional_shadow_atlas{};
        RenderGraphTextureHandle punctual_shadow_atlas{};
        Core::Extent2D extent{};
        /// Extent of the texture the result is written to (may exceed `extent`; see `render_texture_extent`).
        RHI::Extent3D output_extent{};
        RHI::Format output_format = RHI::Format::RGBA16Float;
        CameraView camera{};
        f32 exposure = 1.0f;
        f32 time_seconds = 0.0f;
        u64 frame_index = 0;
        /// Which persistent histories to use (the engine passes the window id).
        u64 history_key = 0;
    };

    /// Whether the fog runs for this frame's settings and camera: enabled, a raster scene and a perspective camera.
    [[nodiscard]] bool volumetric_fog_active(const RenderGraphSettings &settings, const CameraView &camera) noexcept;

    /// The froxel grid for a render extent: tiles x, tiles y, slices, atlas columns (slices per atlas row).
    [[nodiscard]] glm::uvec4 volumetric_fog_grid(Core::Extent2D extent, const RenderSettings::VolumetricFogSettings &fog) noexcept;

    /// Adds the inject, integrate and apply passes and returns the fogged scene colour. Reads `settings.frame.volumetric_fog`.
    [[nodiscard]] Core::RendererExpected<RenderGraphTextureHandle> add_volumetric_fog_passes(
        Renderer &renderer, RHI::RhiDevice &device, HistoryTextureCache &histories, RenderGraph &graph,
        std::vector<RHI::BindGroupHandle> &transient_bind_groups, std::vector<RHI::BufferHandle> &transient_buffers,
        const VolumetricFogDescription &description, const RenderGraphSettings &settings);

    /// The engine's `volumetric_fog` feature (Post stage, after `scene_background`): consumes and republishes
    /// SceneHdrColor.
    [[nodiscard]] Core::RendererResult build_volumetric_fog_feature(FrameBuildContext &frame);

} // namespace SFT::Renderer
