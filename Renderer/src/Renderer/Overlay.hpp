#pragma once

#include <Foundation/Foundation.hpp>

#include <span>

#include <Core/Core.hpp>
#include <Renderer/FramePipeline.hpp>
#include <Renderer/Scene.hpp>

namespace SFT::Renderer {

    /// Overlays are how an application puts its own drawing on top of the finished frame: a HUD, on-screen UI,
    /// gizmos, debug text. The engine ships none of them; an `OverlayPass` (see `Scene.hpp`) placed in
    /// `RenderGraphSettings::overlay_passes` (`Engine::RenderFrameParameters::overlay_passes`) is the whole
    /// mechanism. UI libraries build their on-screen presentation out of it too (`UI::UiSurface`).

    /// Runs each overlay's `prepare`, then adds one render pass per overlay onto the presentation target,
    /// handling the HDR / transparent-window composition for overlay-only frames. This is what the built-in
    /// `overlay_passes` frame feature calls; a replacement feature can call it too, at a different point.
    [[nodiscard]] Core::RendererResult add_overlay_passes(FrameBuildContext &frame, std::span<const OverlayPass> overlays);

} // namespace SFT::Renderer
