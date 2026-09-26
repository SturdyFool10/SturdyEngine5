#pragma once

#include <Foundation/Foundation.hpp>

#include <string_view>

#include <Core/Core.hpp>
#include <Renderer/FramePipeline.hpp>
#include <Renderer/Scene.hpp>

namespace SFT::Renderer {

    /// The engine's tone mapping / display encode, expressed as an ordinary fullscreen effect: the
    /// `Shaders/fullscreen_tonemap.slang` shader plus the operator, exposure, HDR and colour-space settings
    /// packed into its push-constant block. Nothing here is private to the renderer, so an application can
    /// use it as-is, call it with modified settings, or copy it as the starting point of its own tone mapper.
    ///
    /// @param settings Supplies the operator and its parameters; `tone_mapping == false` selects a plain
    ///        display encode (no curve).
    /// @param preserve_alpha Treat the source as premultiplied and encode the unassociated colour (used for
    ///        composited UI).
    [[nodiscard]] CustomPostProcessEffect tone_mapping_effect(const RenderGraphSettings &settings, bool preserve_alpha = false);

    /// Adds a pass that tone maps `source` into `destination` using `tone_mapping_effect`.
    ///
    /// With `composite_over` (needs `preserve_alpha`) the result is blended premultiplied-over what `destination` already
    /// holds instead of replacing it; that is how a UI layer is put over a finished frame.
    ///
    /// `destination` is written in `frame.output_format` unless `target_format` says otherwise. The pass
    /// only uses `FrameBuildContext`'s public services (`prepare_fullscreen_effect` /
    /// `record_fullscreen_effect`), which is exactly what a replacement `tone_mapping` feature would use.
    [[nodiscard]] Core::RendererResult add_tone_mapping_pass(FrameBuildContext &frame, RenderGraphTextureHandle source,
                                                             RenderGraphTextureHandle destination, const RenderGraphSettings &settings,
                                                             bool preserve_alpha = false, std::string_view label = "tone mapping",
                                                             RHI::Format target_format = RHI::Format::Undefined,
                                                             bool composite_over = false);

} // namespace SFT::Renderer
