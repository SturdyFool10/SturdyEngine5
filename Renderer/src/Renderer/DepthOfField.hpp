#pragma once

#include <Foundation/Foundation.hpp>

#include <vector>

#include <glm/vec2.hpp>

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/FramePipeline.hpp>
#include <Renderer/Scene.hpp>

namespace SFT::Renderer {

    class Renderer;

    /// Depth of field (`Shaders/dof_*.slang`): the thin-lens blur of everything away from the focal plane, with the aperture's
    /// own polygon as the bokeh. Four compute passes -- a half-resolution colour + circle-of-confusion prefilter, a tile-max of
    /// the blur, a "scatter as gather" bokeh gather (far and near fields separately), and a full-resolution composite --
    /// written only against public services (`prepare_compute_kernel`/`record_compute_kernel`, the blackboard), so it can be
    /// fed other inputs, replaced or copied. Nothing is random across frames: no temporal state, no ghosting.
    struct DepthOfFieldDescription {
        RenderGraphTextureHandle scene_color{};  // rgba16f scene-linear, `extent`
        RenderGraphTextureHandle depth{};        // device depth (0 near, 1 far), `extent`
        Core::Extent2D extent{};
        /// Extent of the texture the result is written to (may exceed `extent`; see `render_texture_extent`).
        RHI::Extent3D output_extent{};
        RHI::Format output_format = RHI::Format::RGBA16Float;
        CameraView camera{};
    };

    /// Whether depth of field runs for this frame: enabled, a raster scene, a perspective camera.
    [[nodiscard]] bool depth_of_field_active(const RenderGraphSettings &settings, const CameraView &camera) noexcept;

    /// The diameter, in pixels of an image `image_height_px` tall, of the blur disc a point at `view_depth` makes (the thin-lens
    /// circle of confusion, before the `max_blur_px` cap): positive behind the focus plane, negative in front of it. This is
    /// the function the shaders evaluate (through one precomputed scale), so a test can check the optics on the CPU.
    [[nodiscard]] f32 circle_of_confusion_px(const RenderSettings::DepthOfFieldSettings &lens, f32 view_depth, f32 image_height_px) noexcept;

    /// Samples per pixel the gather uses for a `quality` setting (0, 1, 2).
    [[nodiscard]] u32 depth_of_field_sample_count(u32 quality) noexcept;

    /// Adds the four passes and returns the focused-and-blurred scene colour. Reads `settings.frame.depth_of_field`.
    [[nodiscard]] Core::RendererExpected<RenderGraphTextureHandle> add_depth_of_field_passes(
        Renderer &renderer, RenderGraph &graph, std::vector<RHI::BindGroupHandle> &transient_bind_groups,
        std::vector<RHI::BufferHandle> &transient_buffers, RHI::RhiDevice &device, const DepthOfFieldDescription &description,
        const RenderGraphSettings &settings);

    /// The engine's `depth_of_field` feature (Post stage, after `volumetric_fog`): consumes and republishes SceneHdrColor.
    [[nodiscard]] Core::RendererResult build_depth_of_field_feature(FrameBuildContext &frame);

} // namespace SFT::Renderer
