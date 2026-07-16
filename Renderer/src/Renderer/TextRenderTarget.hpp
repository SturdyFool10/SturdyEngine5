#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <vector>
#pragma endregion

#include <RHI/RHI.hpp>
#include <Core/Core.hpp>
#include <Text/Text.hpp>
#include "TextAtlas.hpp"
#include "TextInstance.hpp"

using std::span;
using std::vector;

namespace SFT::Renderer {

    // A single off-screen 2D text render target — e.g. white text rendered once and sampled as a
    // mask by another shader. Unlike Renderer::TextCanvas (a scrolling/tiled document surface),
    // this is exactly one texture: its size is checked against the device's max 2D image
    // dimension at creation and *rejected* (not silently clamped or tiled) if it doesn't fit,
    // since truncating a fixed-purpose mask/target would silently corrupt whatever it's used for —
    // a caller that legitimately needs a surface bigger than one GPU image should reach for
    // Renderer::TextCanvas instead, which is built for exactly that.
    class TextRenderTarget {
      public:
        struct Config {
            u32 width = 0;
            u32 height = 0;
            // RGBA8Unorm for tinted/colored text (e.g. compositing rendered text into a scene);
            // R8Unorm for a pure alpha-coverage mask (e.g. text used to mask another shader) —
            // either way the same TextPipeline draws into it, only the target format differs.
            RHI::Format format = RHI::Format::RGBA8Unorm;
        };

        TextRenderTarget() noexcept = default;

        [[nodiscard]] static Core::RendererExpected<TextRenderTarget> create(RHI::RhiDevice &device, const Config &config);

        // Re-renders the target's full contents from `glyphs` (clears first) — reuses exactly the
        // atlas-lookup + instanced-draw machinery Renderer::TextCanvas's per-tile render does
        // (TextAtlas::ensure_resident -> TextPipeline::prepare/draw), just against one fixed
        // texture instead of a tile grid.
        [[nodiscard]] Core::RendererResult render(RHI::RhiDevice &device, TextAtlas &atlas, TextPipeline &pipeline,
                                                  span<const GlyphPlacement> glyphs);

        // Raw RHI handles, for wiring into a render graph directly.
        [[nodiscard]] RHI::TextureHandle texture() const noexcept;
        [[nodiscard]] RHI::TextureViewHandle view() const noexcept;
        [[nodiscard]] RHI::SamplerHandle sampler() const noexcept;
        [[nodiscard]] u32 width() const noexcept;
        [[nodiscard]] u32 height() const noexcept;

        void destroy(RHI::RhiDevice &device) noexcept;

      private:
        Config config_{};
        RHI::TextureHandle texture_{};
        RHI::TextureViewHandle view_{};
        RHI::SamplerHandle sampler_{};
        RHI::TextureLayout current_layout_ = RHI::TextureLayout::Undefined;
        TextFrameResources text_resources_{};
        const TextPipeline *resources_pipeline_ = nullptr;
    };

} // namespace SFT::Renderer
