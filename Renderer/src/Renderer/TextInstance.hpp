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
    // Field order is deliberate, not cosmetic: it must byte-match Shaders/text_sdf.slang's
    // GlyphInstance exactly, and Slang lays out StructuredBuffer elements with std430 rules
    // (vec2 aligned to 8, vec4 aligned to 16, the whole struct padded to a multiple of its
    // largest member's alignment) — rules plain C++ struct layout doesn't follow on its own. The
    // two vec2 pairs (8-byte aligned, 16 bytes total) then the vec4 (16-byte aligned, already at
    // a 32-byte/16-aligned offset) then four floats (16 bytes, no trailing pad needed) happen to
    // lay out identically under both C++'s natural alignment and std430 with zero gaps either
    // way — reordering the fields (or `stem_darkening_px`) is a hard requirement, not tidiness.
    struct GlyphInstance {
        glm::vec2 position{0.0f};
        glm::vec2 size{0.0f};
        glm::vec2 uv_min{0.0f};
        glm::vec2 uv_max{0.0f};
        glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
        // `rotation` (radians, counter-clockwise) turns the quad's `size` axes around `position`
        // before pixel-space projection — needed for text-on-a-spline
        // (Renderer/Spline.cppm's GlyphPathPlacement2D::rotation feeds straight into this) and
        // incidentally makes ordinary rotated 2D labels possible too.
        f32 rotation = 0.0f;
        // 0 = SDF (sample R, distance math), 1 = MSDF (median RGB, distance math), 2 = Color
        // (sample RGBA straight through, alpha-modulated only — no distance math; see
        // Text::RasterFormat::Color / Text/ColorGlyph.cppm).
        f32 format_kind = 0.0f;
        // Encoded distance band width, mapped to *screen* pixels: `pixel_range * (size /
        // cell_size_px)` for a glyph drawn at `size` screen pixels from a `cell_size_px`-pixel
        // raster cell (see TextAtlas::Config::pixel_range and GlyphSlot::cell_size_px). Keeps the
        // antialiasing edge one physical pixel wide at any on-screen scale.
        f32 screen_px_range = 2.0f;
        // Stem-darkening bias, in screen pixels, added to the SDF/MSDF coverage threshold before
        // the alpha cutoff — see resolved_stem_darkening_px() below for what this counteracts and
        // why it ramps out at larger sizes.
        f32 stem_darkening_px = 0.0f;
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
        // On by default — see resolved_stem_darkening_px(). A caller that wants perfectly literal
        // vector-accurate edges (e.g. an exported/print-preview path where optical compensation
        // would be wrong) sets this false to get the raw distance field untouched.
        bool stem_darkening = true;
    };

    // A small embolden bias (in screen pixels) applied to the SDF/MSDF coverage threshold at small
    // on-screen sizes, ramping to zero by `max_ppem`. Every outline this engine rasterizes is
    // unhinted (Text::glyph_outline pulls straight from glyf/CFF, no grid-fitting), which keeps
    // shapes proportionally exact at any scale but leaves small text looking thin/washed-out —
    // the same tradeoff Quartz/CoreText and DirectWrite make, and the same fix they apply: a
    // little extra weight at small sizes instead of pixel-snapping the outline. Constants (0.22px
    // max strength, 14-28ppem ramp) match what a from-scratch text stack (Vertex's `textui` crate)
    // settled on for this same unhinted-SDF setup.
    [[nodiscard]] constexpr f32 resolved_stem_darkening_px(f32 pixel_size, f32 min_ppem = 14.0f, f32 max_ppem = 28.0f,
                                                            f32 max_strength = 0.22f) noexcept {
        if (pixel_size >= max_ppem) {
            return 0.0f;
        }
        if (pixel_size <= min_ppem) {
            return max_strength;
        }
        return max_strength * (1.0f - (pixel_size - min_ppem) / (max_ppem - min_ppem));
    }

    // Builds a GlyphInstance from a resolved atlas slot — the one place `format_kind`,
    // `screen_px_range`, and `stem_darkening_px` get computed, reused by every draw path
    // (Renderer/RendererTextOverlay.cpp, Renderer/TextCanvasImpl.cpp,
    // Renderer/TextRenderTargetImpl.cpp) instead of each hand-rolling the same struct literal.
    // `position` is taken separately (not `placement.position`) since a tiled caller
    // (Renderer::TextCanvas) needs it relative to its tile's origin, not the placement's absolute
    // canvas position.
    [[nodiscard]] inline GlyphInstance make_glyph_instance(glm::vec2 position, const GlyphPlacement &placement,
                                                            const GlyphSlot &slot, f32 atlas_pixel_range) noexcept {
        const f32 instance_scale = slot.cell_size_px > 0.0f ? placement.size.x / slot.cell_size_px : 1.0f;
        // `position` in is the pen (baseline origin), not the quad's top-left corner — every glyph's
        // resident raster is cropped to *its own* ink bounding box (see RasterizedGlyph's doc
        // comment), so the offset from pen to that raster's top-left varies per glyph (a
        // parenthesis descends well below the baseline and rises well above x-height, unlike a
        // plain lowercase letter) and must be added back here, rescaled by the same
        // cell-size-relative factor already used for screen_px_range above.
        const glm::vec2 bearing_offset = glm::vec2{slot.bearing_x, -slot.bearing_top} * instance_scale;
        return GlyphInstance{
            .position = position + bearing_offset,
            .size = placement.size,
            .uv_min = slot.uv_min,
            .uv_max = slot.uv_max,
            .color = placement.color,
            .format_kind = format_kind_value(slot.format),
            .screen_px_range = atlas_pixel_range * instance_scale,
            .stem_darkening_px = placement.stem_darkening ? resolved_stem_darkening_px(placement.pixel_size) : 0.0f,
        };
    }

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
        // pass to draw(). The instance buffer write itself is a plain host-visible memcpy (no GPU
        // work, nothing to wait on); on growth, the outgrown buffer may still be referenced by a
        // prior frame's in-flight bind group, so it's appended to `out_transient_buffers` instead
        // of being destroyed on the spot — the caller frees it once the frame's fence retires.
        [[nodiscard]] Core::RendererResult prepare(RHI::RhiDevice &device, span<const GlyphInstance> instances,
                                                   span<const GlyphSlot> slots, vector<TextDrawBatch> &out_batches,
                                                   vector<RHI::BufferHandle> &out_transient_buffers);

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
