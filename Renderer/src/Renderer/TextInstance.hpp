#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <span>
#include <vector>
#pragma endregion

#include <RHI/RHI.hpp>
#include <Core/Core.hpp>
#include <Text/Text.hpp>
#include "TextAtlas.hpp"

using std::span;
using std::vector;

namespace SFT::Renderer {

    // GPU-visible per-glyph instance data for the SDF/MSDF text pipeline (Shaders/text_sdf.slang).
    // One instance = one glyph quad. `position`/`size` are in the render target's pixel space
    // (top-left origin, matching the fullscreen-triangle convention in Shaders/sturdy_common.slang
    // and Shaders/fullscreen_tonemap.slang — no Y-flip); for 3D world-space text the caller
    // projects the glyph's world-space quad corners to screen pixels before filling this in — the
    // shader itself only ever draws flat, screen-aligned pixel-space quads. `rotation` (radians,
    // counter-clockwise) turns the quad's `size` axes around `position` before that projection —
    // needed for text-on-a-spline (Renderer/Spline.cppm's GlyphPathPlacement2D::rotation feeds
    // straight into this) and incidentally makes ordinary rotated 2D labels possible too.
    struct GlyphInstance {
        glm::vec2 position{0.0f};
        glm::vec2 size{0.0f};
        f32 rotation = 0.0f;
        glm::vec2 uv_min{0.0f};
        glm::vec2 uv_max{0.0f};
        glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
        // 0 = SDF (sample R, distance math), 1 = MSDF (median RGB, distance math), 2 = Color
        // (sample RGBA straight through, alpha-modulated only — no distance math; see
        // Text::RasterFormat::Color / Text/ColorGlyph.cppm).
        f32 format_kind = 0.0f;
        // Encoded distance band width, mapped to *screen* pixels: `pixel_range * (size /
        // cell_size_px)` for a glyph drawn at `size` screen pixels from a `cell_size_px`-pixel
        // raster cell (see TextAtlas::Config::pixel_range and GlyphSlot::cell_size_px). Keeps the
        // antialiasing edge one physical pixel wide at any on-screen scale.
        f32 screen_px_range = 2.0f;
    };

    // Maps a raster format onto GlyphInstance::format_kind — the one place this mapping is
    // spelled out, reused everywhere a GlyphInstance gets built from a TextAtlas::GlyphSlot
    // (Renderer/TextCanvasImpl.cpp, Renderer/TextRenderTargetImpl.cpp, ...).
    [[nodiscard]] constexpr f32 format_kind_value(Text::RasterFormat format) noexcept {
        switch (format) {
            case Text::RasterFormat::SDF: return 0.0f;
            case Text::RasterFormat::MSDF: return 1.0f;
            case Text::RasterFormat::Color: return 2.0f;
        }
        return 0.0f;
    }

    // One glyph placed at a specific position, ready to be resolved against a TextAtlas and drawn
    // — the shared "what to draw and where" shape for anything that renders a batch of glyphs
    // without the caller manually calling TextAtlas::ensure_resident() and building GlyphInstances
    // itself (Renderer/TextCanvas.cppm's per-tile glyph store, Renderer/TextRenderTarget.cppm).
    // `outline`/`font` are non-owning — see TextAtlas::GlyphRequest's identical contract.
    struct GlyphPlacement {
        glm::vec2 position{0.0f};
        glm::vec2 size{0.0f};
        glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
        u64 font_id = 0;
        u32 glyph_id = 0;
        u32 units_per_em = 1000;
        f32 pixel_size = 32.0f;
        Text::RasterFormat format = Text::RasterFormat::SDF;
        const Text::GlyphOutline *outline = nullptr;
        const Text::Font *font = nullptr;
    };

    struct TextDrawBatch {
        Text::RasterFormat format = Text::RasterFormat::SDF;
        u32 tile_index = 0;
        u32 first_instance = 0;
        u32 instance_count = 0;
    };

    // The instanced glyph-quad GPU pipeline: one render pipeline (Shaders/text_sdf.slang, alpha
    // blended, no depth test) driving vertex-pulled instanced draws, one per (format, atlas tile)
    // batch — so a whole run of text costs one draw call per atlas tile it touches, almost always
    // one, since a tile holds thousands of glyph cells (see Renderer/TextAtlas.cppm).
    class TextPipeline {
      public:
        TextPipeline() noexcept = default;

        [[nodiscard]] static Core::RendererExpected<TextPipeline> create(RHI::RhiDevice &device, RHI::Format color_format);

        // Groups `instances` by (format, tile) — `slots[i]` must be the TextAtlas::GlyphSlot that
        // produced `instances[i]` (same order, e.g. straight from TextAtlas::ensure_resident) —
        // uploads them into one instance buffer (growing it if needed), and returns the batches to
        // pass to draw().
        [[nodiscard]] Core::RendererResult prepare(RHI::RhiDevice &device, span<const GlyphInstance> instances,
                                                   span<const GlyphSlot> slots, vector<TextDrawBatch> &out_batches);

        // Issues one instanced draw per batch against `pass`, (re)binding each batch's atlas tile
        // in turn. `viewport_size` is the render target's pixel dimensions (the pipeline's only
        // per-draw constant, passed as a push constant). Bind groups created for this call are
        // appended to `transient_bind_groups` — the caller frees them once the frame's fence
        // retires, matching Renderer::record_tonemap's convention.
        [[nodiscard]] Core::RendererResult draw(RHI::RhiDevice &device, RHI::RenderPassEncoder &pass, const TextAtlas &atlas,
                                                span<const TextDrawBatch> batches, glm::vec2 viewport_size,
                                                vector<RHI::BindGroupHandle> &transient_bind_groups);

        void destroy(RHI::RhiDevice &device) noexcept;

      private:
        struct ResourceBinding {
            usize layout_index = 0;
            u32 binding = 0;
            bool found = false;
        };

        RHI::ShaderModuleHandle vertex_module_{};
        RHI::ShaderModuleHandle fragment_module_{};
        RHI::PipelineLayoutHandle pipeline_layout_{};
        RHI::RenderPipelineHandle pipeline_{};
        vector<RHI::BindGroupLayoutHandle> bind_group_layouts_;
        vector<u32> bind_group_layout_sets_;
        RHI::SamplerHandle sampler_{};
        RHI::BufferHandle instance_buffer_{};
        u64 instance_buffer_capacity_ = 0;
        ResourceBinding instances_binding_{};
        ResourceBinding texture_binding_{};
        ResourceBinding sampler_binding_{};
    };

} // namespace SFT::Renderer
