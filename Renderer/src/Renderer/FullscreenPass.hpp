#pragma once

#include <Foundation/Foundation.hpp>

#include <string>
#include <vector>

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/Scene.hpp>

namespace SFT::Renderer {

    class Renderer;

    /// One full-screen draw of a `CustomPostProcessEffect` into a graph texture.
    struct FullscreenPassDescription {
        std::string label;
        RenderGraphTextureHandle destination;
        RHI::Format destination_format = RHI::Format::Undefined;
        Core::Extent2D extent{};
        RHI::LoadOp load_op = RHI::LoadOp::DontCare;
        /// Bound as `sourceTexture`.
        RenderGraphTextureHandle source;
        /// Bound as `extraTexture0` when valid (the effect must declare `extra_input_count = 1`).
        RenderGraphTextureHandle extra_source{};
        CustomPostProcessEffect effect;
    };

    /// Compiles the effect for `destination_format` if needed and adds a render pass drawing it. This is the
    /// building block the engine's own bloom, anti-aliasing and tone mapping are made of.
    [[nodiscard]] Core::RendererResult add_fullscreen_effect_pass(Renderer &renderer, RenderGraph &graph,
                                                                  std::vector<RHI::BindGroupHandle> &transient_bind_groups,
                                                                  FullscreenPassDescription description);

} // namespace SFT::Renderer
