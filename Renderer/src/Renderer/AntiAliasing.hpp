#pragma once

#include <Foundation/Foundation.hpp>

#include <vector>

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/Scene.hpp>

namespace SFT::Renderer {

    class Renderer;

    /// The engine's scene-linear spatial anti-aliasing (`Shaders/fullscreen_anti_aliasing.slang`) as an
    /// ordinary fullscreen effect: the mode, sub-pixel quality and edge threshold from `settings` packed into
    /// the shader's push-constant block. Usable as-is, with modified settings, or as the starting point of an
    /// application's own AA.
    [[nodiscard]] CustomPostProcessEffect post_process_aa_effect(const RenderGraphSettings &settings);

    /// Adds a pass that anti-aliases `source` into `destination` (both `format`, `extent` pixels). Does nothing
    /// when `settings.post_process_aa == 0`.
    [[nodiscard]] Core::RendererResult add_post_process_aa_pass(Renderer &renderer, RenderGraph &graph,
                                                                std::vector<RHI::BindGroupHandle> &transient_bind_groups,
                                                                RenderGraphTextureHandle source, RenderGraphTextureHandle destination,
                                                                Core::Extent2D extent, RHI::Format format,
                                                                const RenderGraphSettings &settings);

} // namespace SFT::Renderer
