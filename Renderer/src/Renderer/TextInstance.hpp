#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <cmath>
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
        // Encoded distance band width in atlas texels. The fragment shader combines this with
        // fwidth(UV) and the actual atlas dimensions to recover screen-pixel distance, remaining
        // exact under non-uniform scale and rotation.
        f32 distance_pixel_range = 2.0f;
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
        // Desired on-screen em-box size, not the padded atlas-raster size. make_glyph_instance()
        // expands the actual quad by the resident raster's guard pixels while preserving this
        // font size, so padding never makes the visible glyph smaller or changes its advance.
        glm::vec2 size{0.0f};
        // Radians counter-clockwise around the pen origin. make_glyph_instance() rotates both the
        // pen-to-raster bearing and the quad axes, so curved/rotated text does not drift.
        f32 rotation = 0.0f;
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
    // on-screen sizes, ramping to zero by `max_ppem`. Scalable SDF/MSDF outlines are unhinted
    // (Text::glyph_outline pulls straight from glyf/CFF, without grid-fitting), which preserves
    // shape at arbitrary transforms but can make small text look thin. Fixed-size hinted callers
    // disable this compensation so it is never applied twice.
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
    // `distance_pixel_range`, and `stem_darkening_px` get computed, reused by every draw path
    // (Renderer/RendererTextOverlay.cpp, Renderer/TextCanvasImpl.cpp,
    // Renderer/TextRenderTargetImpl.cpp) instead of each hand-rolling the same struct literal.
    // `position` is taken separately (not `placement.position`) since a tiled caller
    // (Renderer::TextCanvas) needs it relative to its tile's origin, not the placement's absolute
    // canvas position.
    [[nodiscard]] inline GlyphInstance make_glyph_instance(glm::vec2 position, const GlyphPlacement &placement,
                                                            const GlyphSlot &slot, f32 atlas_pixel_range) noexcept {
        const glm::vec2 instance_scale = slot.reference_ppem > 0.0f
                                             ? placement.size / glm::vec2{slot.reference_ppem}
                                             : glm::vec2{1.0f};
        // `position` in is the pen (baseline origin), not the quad's top-left corner — every glyph's
        // resident raster is cropped to *its own* ink bounding box (see RasterizedGlyph's doc
        // comment), so the offset from pen to that raster's top-left varies per glyph (a
        // parenthesis descends well below the baseline and rises well above x-height, unlike a
        // plain lowercase letter) and must be added back here, rescaled by the same
        // reference-ppem-relative factor used for the raster size above.
        const glm::vec2 bearing_offset = glm::vec2{slot.bearing_x, -slot.bearing_top} * instance_scale;
        const f32 cosine = std::cos(placement.rotation);
        const f32 sine = std::sin(placement.rotation);
        const glm::vec2 rotated_bearing{
            bearing_offset.x * cosine - bearing_offset.y * sine,
            bearing_offset.x * sine + bearing_offset.y * cosine,
        };
        const glm::vec2 raster_size = slot.raster_size_px * instance_scale;
        return GlyphInstance{
            .position = position + rotated_bearing,
            .size = raster_size,
            .uv_min = slot.uv_min,
            .uv_max = slot.uv_max,
            .color = placement.color,
            .rotation = placement.rotation,
            .format_kind = format_kind_value(slot.format),
            .distance_pixel_range = atlas_pixel_range,
            .stem_darkening_px = placement.stem_darkening ? resolved_stem_darkening_px(placement.pixel_size) : 0.0f,
        };
    }

    struct TextDrawBatch {
        Text::RasterFormat format = Text::RasterFormat::SDF;
        u32 tile_index = 0;
        // Owned by the caller's independently fenced resource slot. It remains allocated across
        // frames and grows only when this slot's instance capacity is exceeded.
        RHI::BufferHandle instance_buffer{};
        u32 first_instance = 0;
        u32 instance_count = 0;
        struct BoundGroup {
            u32 set = 0;
            RHI::BindGroupHandle handle{};
        };
        vector<BoundGroup> bind_groups;
    };

    // Persistent GPU state for one independently fenced text workload. The main renderer owns one
    // per frame-in-flight slot; offscreen text targets/canvas tiles own one beside their texture.
    // Buffers grow but never shrink, and bind groups are rebuilt only when that buffer grows or the
    // atlas image view changes.
    struct TextFrameResources {
        struct BindingCacheEntry {
            Text::RasterFormat format = Text::RasterFormat::SDF;
            u32 tile_index = 0;
            RHI::TextureViewHandle atlas_view{};
            vector<TextDrawBatch::BoundGroup> bind_groups;
        };

        RHI::BufferHandle instance_buffer{};
        u64 instance_capacity_bytes = 0;
        // Exact CPU mirror of the last successful upload. Static text can revisit this frame slot
        // without issuing another host-to-GPU write; comparison is field-wise so struct padding
        // never participates.
        vector<GlyphInstance> uploaded_instances;
        vector<BindingCacheEntry> binding_cache;
    };

    void destroy_text_frame_resources(RHI::RhiDevice &device, TextFrameResources &resources) noexcept;

    // The instanced glyph-quad GPU pipeline: one render pipeline (Shaders/text_sdf.slang, alpha
    // blended, no depth test) driving vertex-pulled instanced draws, one per (format, atlas tile)
    // batch — so a whole run of text costs one draw call per atlas tile it touches, almost always
    // one, once the adaptively sized atlas image for that format has reached steady state (see
    // Renderer/TextAtlas.cpp).
    class TextPipeline {
      public:
        TextPipeline() noexcept = default;

        [[nodiscard]] static Core::RendererExpected<TextPipeline> create(RHI::RhiDevice &device, RHI::Format color_format);

        // Forms consecutive (format, tile) batches without reordering painter order. `resources`
        // belongs to a fence-retired frame slot (or a synchronous offscreen owner), so its buffer
        // and bind groups can be updated/reused without racing another in-flight frame.
        [[nodiscard]] Core::RendererResult prepare(RHI::RhiDevice &device, const TextAtlas &atlas,
                                                   span<const GlyphInstance> instances, span<const GlyphSlot> slots,
                                                   TextFrameResources &resources, vector<TextDrawBatch> &out_batches);

        // Issues one instanced draw per batch against `pass`, binding the persistent groups prepared
        // above. `viewport_size` is the render target's pixel dimensions.
        [[nodiscard]] Core::RendererResult draw(RHI::RenderPassEncoder &pass,
                                                span<const TextDrawBatch> batches, glm::vec2 viewport_size);

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
        ResourceBinding instances_binding_{};
        ResourceBinding texture_binding_{};
        ResourceBinding sampler_binding_{};
    };

} // namespace SFT::Renderer
