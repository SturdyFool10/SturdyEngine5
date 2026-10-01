#pragma once

#include <Foundation/Foundation.hpp>

#include <Core/Core.hpp>
#include <Renderer/FramePipeline.hpp>
#include <Renderer/Scene.hpp>

namespace SFT::Renderer {

    /// The body-camera look as an ordinary fullscreen effect: `Shaders/camera_emulation.slang` plus the settings
    /// packed into its push-constant block. It works on scene-linear HDR colour, so it belongs between exposure
    /// and tone mapping. Nothing here is private to the renderer.
    ///
    /// @param aspect Width / height of the image the effect runs on (the distortion is aspect-corrected).
    [[nodiscard]] CustomPostProcessEffect camera_emulation_effect(const CameraEmulationSettings &settings, u32 frame_index,
                                                                  f32 aspect);

    /// Adds a pass that applies `camera_emulation_effect` to `source`, writing `destination`. Uses only the
    /// renderer's public fullscreen-effect services (`prepare_fullscreen_effect` / `record_fullscreen_effect`).
    [[nodiscard]] Core::RendererResult add_camera_emulation_pass(FrameBuildContext &frame, RenderGraphTextureHandle source,
                                                                 RenderGraphTextureHandle destination, Core::Extent2D extent,
                                                                 RHI::Format format, const RenderGraphSettings &settings);

    /// The engine's `camera_emulation` frame feature: reads `SceneHdrColor` and republishes the processed colour.
    /// A no-op unless `settings.camera_emulation.enabled`. Register it (or your own) with `FramePipeline::add`.
    [[nodiscard]] Core::RendererResult build_camera_emulation_feature(FrameBuildContext &frame);

} // namespace SFT::Renderer
