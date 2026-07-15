#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <span>
#include <unordered_map>
#include <vector>
#pragma endregion

#include <RHI/RHI.hpp>
#include <Core/Core.hpp>
#include <Text/Text.hpp>
#include "TileGrid.hpp"

using std::span;
using std::unordered_map;
using std::vector;

namespace SFT::Renderer {

    // Identifies one cached glyph rasterization: which font, which glyph, at what reference pixel
    // size, in which distance-field format. A glyph used at multiple sizes/formats concurrently
    // (the same font small in a UI label and large on a 3D sign) gets one cache entry per distinct
    // key — see Text::select_raster_format's hysteresis for why the format is part of the key
    // rather than something re-derived live every lookup.
    struct GlyphKey {
        u64 font_id = 0;
        u32 glyph_id = 0;
        u32 reference_ppem = 0;
        Text::RasterFormat format = Text::RasterFormat::SDF;
        [[nodiscard]] friend constexpr bool operator==(const GlyphKey &, const GlyphKey &) noexcept = default;
    };

    struct GlyphKeyHash {
        [[nodiscard]] usize operator()(const GlyphKey &key) const noexcept;
    };

    // What the atlas needs to rasterize a glyph on a cache miss. `outline`/`font` are non-owning
    // and only dereferenced if this exact (font, glyph, size, format) isn't already resident — the
    // caller (a font/shaping layer) owns their lifetime for the duration of the call. `outline` is
    // used for `RasterFormat::SDF`/`MSDF` (Text::rasterize_glyph); `font` is used for
    // `RasterFormat::Color` (Text::rasterize_color_glyph, which extracts each COLR layer's outline
    // itself, or decodes an embedded PNG strike — either way it needs the font, not a pre-extracted
    // outline).
    struct GlyphRequest {
        u64 font_id = 0;
        u32 glyph_id = 0;
        u32 units_per_em = 1000;
        f32 pixel_size = 32.0f;
        Text::RasterFormat format = Text::RasterFormat::SDF;
        const Text::GlyphOutline *outline = nullptr;
        const Text::Font *font = nullptr;
    };

    // Where a resident glyph lives in the atlas: which tile, and its normalized UV rect within
    // that tile's texture. `cell_size_px` is the resident raster box's edge length in pixels
    // (cells are square) — an instance builder (Renderer/TextInstance.cppm) uses it together with
    // the glyph's font-unit metrics to size the glyph's screen/world quad.
    struct GlyphSlot {
        u32 tile_index = 0;
        glm::vec2 uv_min{0.0f};
        glm::vec2 uv_max{0.0f};
        f32 cell_size_px = 0.0f;
        Text::RasterFormat format = Text::RasterFormat::SDF;
    };

    // An LRU, tile-based cache of rasterized glyphs backed by RHI textures. Three independent
    // sub-atlases are maintained — R8Unorm (SDF), RGBA8Unorm (MSDF), and RGBA8Unorm (Color, for
    // emoji — see Text/ColorGlyph.cppm) — each its own set of same-size tiles clamped to the
    // device's actual max 2D image dimension
    // (clamp_tile_size, TileGrid.cppm), so requesting more glyph storage than fits in one texture grows
    // the tile count instead of failing outright. Each tile is subdivided into a fixed-size cell
    // grid; eviction reclaims individual cells (via LruIndex), not whole tiles, so a
    // handful of rarely-used glyphs never holds an entire tile hostage.
    class TextAtlas {
      public:
        struct Config {
            u32 desired_tile_size = 2048;
            u32 cell_size = 64;
            u32 max_tiles_per_format = 64;
            f32 pixel_range = 4.0f;
            f32 padding_px = 4.0f;
        };

        TextAtlas() noexcept = default;

        [[nodiscard]] static Core::RendererExpected<TextAtlas> create(RHI::RhiDevice &device, const Config &config);

        // Ensures every glyph in `requests` is resident (rasterizing any misses in parallel via
        // Async::par_iter, then recording their upload — one batched staging-buffer copy plus the
        // layout-transition barriers around it — into the caller's `encoder`), and marks each as
        // most-recently-used. `out_slots` is resized to `requests.size()` and filled in request
        // order. Deliberately does NOT submit or wait: `encoder` is the caller's own per-frame
        // command encoder (already recording the rest of the frame), so this upload becomes just
        // more commands in that one queue submission — no separate fence, no CPU stall. The staging
        // buffer this call creates on a miss is appended to `out_transient_buffers`; the caller must
        // keep it alive (and eventually destroy it) until the frame's fence retires, since the GPU
        // copy hasn't necessarily run yet when this function returns.
        [[nodiscard]] Core::RendererResult ensure_resident(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                                           span<const GlyphRequest> requests, vector<GlyphSlot> &out_slots,
                                                           vector<RHI::BufferHandle> &out_transient_buffers);

        [[nodiscard]] RHI::TextureViewHandle tile_view(Text::RasterFormat format, u32 tile_index) const noexcept;
        [[nodiscard]] u32 tile_count(Text::RasterFormat format) const noexcept;
        [[nodiscard]] u32 tile_size() const noexcept;
        [[nodiscard]] f32 pixel_range() const noexcept;

        void destroy(RHI::RhiDevice &device) noexcept;

      private:
        struct Tile {
            RHI::TextureHandle texture{};
            RHI::TextureViewHandle view{};
            RHI::TextureLayout current_layout = RHI::TextureLayout::Undefined;
        };

        struct FormatAtlas {
            vector<Tile> tiles;
            u32 cells_per_row = 0;
            u32 next_free_cell = 0; // linear cell-allocation cursor before the first eviction pass
        };

        struct CellLocation {
            u32 tile_index = 0;
            u32 cell_x = 0;
            u32 cell_y = 0;
            // The sub-rect of the cell actually rasterized into and sampled — see the comment on
            // TextAtlas::upload_misses' `raster_size` computation. Not necessarily the full
            // `config_.cell_size`: small glyphs raster smaller so their on-screen quad isn't stuck
            // minifying a whole 64px cell down to a handful of pixels.
            u32 raster_size = 0;
        };

        struct PendingUpload {
            usize request_index = 0;
            CellLocation cell{};
        };

        [[nodiscard]] Core::RendererExpected<CellLocation> allocate_cell(RHI::RhiDevice &device, Text::RasterFormat format);
        [[nodiscard]] Core::RendererResult upload_misses(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                                         span<const GlyphRequest> requests, const vector<PendingUpload> &misses,
                                                         vector<GlyphSlot> &out_slots, vector<RHI::BufferHandle> &out_transient_buffers);

        [[nodiscard]] FormatAtlas &format_atlas(Text::RasterFormat format) noexcept;
        [[nodiscard]] const FormatAtlas &format_atlas(Text::RasterFormat format) const noexcept;
        [[nodiscard]] LruIndex<GlyphKey, GlyphKeyHash> &format_lru(Text::RasterFormat format) noexcept;
        [[nodiscard]] RHI::Format texture_format(Text::RasterFormat format) const noexcept;
        [[nodiscard]] GlyphSlot slot_from_cell(Text::RasterFormat format, CellLocation cell) const noexcept;

        Config config_{};
        u32 tile_size_ = 0;
        FormatAtlas sdf_;
        FormatAtlas msdf_;
        FormatAtlas color_;
        unordered_map<GlyphKey, CellLocation, GlyphKeyHash> resident_;
        LruIndex<GlyphKey, GlyphKeyHash> sdf_lru_;
        LruIndex<GlyphKey, GlyphKeyHash> msdf_lru_;
        LruIndex<GlyphKey, GlyphKeyHash> color_lru_;
    };

} // namespace SFT::Renderer
