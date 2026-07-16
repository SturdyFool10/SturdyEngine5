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

    // Images/views superseded by grow-only atlas replacement. They remain alive until the command
    // buffer that copies from them (and every earlier queued draw that sampled them) retires.
    struct TextAtlasRetiredResources {
        vector<RHI::TextureHandle> textures;
        vector<RHI::TextureViewHandle> texture_views;
    };

    // Where a resident glyph lives in the atlas: which tile, and its normalized UV rect within
    // that tile's texture. `raster_size_px` is the tightly packed resident raster rectangle in
    // pixels — an instance builder uses it together
    // with `reference_ppem` to size the glyph's screen/world quad without shrinking for padding.
    struct GlyphSlot {
        u32 tile_index = 0;
        glm::vec2 uv_min{0.0f};
        glm::vec2 uv_max{0.0f};
        glm::vec2 raster_size_px{0.0f};
        // Actual em size at which this atlas entry was generated. It may be lower than the
        // requested display size when an unusually large glyph must be down-rasterized to fit a tile;
        // distance fields then scale it back up without clipping.
        f32 reference_ppem = 0.0f;
        Text::RasterFormat format = Text::RasterFormat::SDF;
        // Where the resident raster sits relative to the pen, in its own pixel space (see
        // Text::RasterizedGlyph's doc comment for the convention) — an instance builder rescales
        // these by the same reference-ppem-relative factor it applies to the resident raster size
        // before placing the glyph's quad, since a cache hit may be drawn at a different actual
        // pixel size than the one this slot's raster was generated at.
        f32 bearing_x = 0.0f;
        f32 bearing_top = 0.0f;
    };

    // An LRU, tile-based cache of rasterized glyphs backed by RHI textures. Three independent
    // sub-atlases are maintained — R8Unorm (SDF), RGBA8Unorm (MSDF), and RGBA8Unorm (Color, for
    // emoji — see Text/ColorGlyph.cpp) — each with its own lazily allocated image. A format starts
    // with no VRAM allocation; its first image is small, then that image is replaced by a doubled
    // image up to a configured/device-clamped ceiling. Existing texels are copied to the same
    // coordinates and the smaller image is fence-retired. Images never shrink, including after
    // eviction. This keeps ordinary UI text cheap while allowing multilingual workloads to grow.
    // Every glyph reserves only its actual padded ink rectangle. Eviction returns that rectangle
    // to a coalescing free-rectangle allocator instead of throwing away a whole tile.
    class TextAtlas {
      public:
        struct Config {
            u32 initial_image_size = 64;
            u32 maximum_image_size = 4096;
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
                                                           vector<RHI::BufferHandle> &out_transient_buffers,
                                                           TextAtlasRetiredResources &out_retired_resources);

        [[nodiscard]] RHI::TextureViewHandle tile_view(Text::RasterFormat format, u32 tile_index) const noexcept;
        [[nodiscard]] u32 tile_count(Text::RasterFormat format) const noexcept;
        [[nodiscard]] u32 tile_size() const noexcept;
        [[nodiscard]] f32 pixel_range() const noexcept;

        void destroy(RHI::RhiDevice &device) noexcept;

      private:
        struct AtlasRect {
            u32 x = 0;
            u32 y = 0;
            u32 width = 0;
            u32 height = 0;
        };

        struct Tile {
            RHI::TextureHandle texture{};
            RHI::TextureViewHandle view{};
            RHI::TextureLayout current_layout = RHI::TextureLayout::Undefined;
            u32 size = 0;
            // Disjoint rectangles covering every currently unused texel in this tile. Allocation
            // guillotine-splits one rectangle; release merges compatible neighbors back together.
            vector<AtlasRect> free_rects;
        };

        struct FormatAtlas {
            vector<Tile> tiles;
        };

        struct RectLocation {
            u32 tile_index = 0;
            u32 x = 0;
            u32 y = 0;
            u32 raster_width = 0;
            u32 raster_height = 0;
            f32 reference_ppem = 0.0f;
            // Only known once Text::rasterize_glyph actually runs (upload_misses), so a fresh
            // cache-miss RectLocation is inserted into resident_ with these left at 0 and
            // overwritten right after rasterizing — see GlyphSlot's doc comment for what they mean.
            f32 bearing_x = 0.0f;
            f32 bearing_top = 0.0f;
        };

        struct PendingUpload {
            usize request_index = 0;
            GlyphKey key{};
            RectLocation rect{};
            bool allocated = false;
        };

        [[nodiscard]] Core::RendererExpected<RectLocation>
        allocate_rect(RHI::RhiDevice &device, RHI::CommandEncoder &encoder, Text::RasterFormat format,
                      u32 width, u32 height, span<const GlyphKey> protected_keys,
                      TextAtlasRetiredResources &out_retired_resources);
        [[nodiscard]] Core::RendererExpected<Tile> create_tile(RHI::RhiDevice &device,
                                                               Text::RasterFormat format, u32 size);
        [[nodiscard]] Core::RendererResult append_tile(RHI::RhiDevice &device, Text::RasterFormat format, u32 size);
        [[nodiscard]] Core::RendererResult grow_tile(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                                     Text::RasterFormat format, u32 new_size,
                                                     TextAtlasRetiredResources &out_retired_resources);
        void release_rect(Text::RasterFormat format, RectLocation rect) noexcept;
        [[nodiscard]] Core::RendererResult upload_misses(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                                         span<const GlyphRequest> requests, const vector<PendingUpload> &misses,
                                                         vector<GlyphSlot> &out_slots, vector<RHI::BufferHandle> &out_transient_buffers);

        [[nodiscard]] FormatAtlas &format_atlas(Text::RasterFormat format) noexcept;
        [[nodiscard]] const FormatAtlas &format_atlas(Text::RasterFormat format) const noexcept;
        [[nodiscard]] LruIndex<GlyphKey, GlyphKeyHash> &format_lru(Text::RasterFormat format) noexcept;
        [[nodiscard]] RHI::Format texture_format(Text::RasterFormat format) const noexcept;
        [[nodiscard]] GlyphSlot slot_from_rect(Text::RasterFormat format, RectLocation rect) const noexcept;

        Config config_{};
        u32 initial_tile_size_ = 0;
        u32 max_tile_size_ = 0;
        FormatAtlas sdf_;
        FormatAtlas msdf_;
        FormatAtlas color_;
        unordered_map<GlyphKey, RectLocation, GlyphKeyHash> resident_;
        LruIndex<GlyphKey, GlyphKeyHash> sdf_lru_;
        LruIndex<GlyphKey, GlyphKeyHash> msdf_lru_;
        LruIndex<GlyphKey, GlyphKeyHash> color_lru_;
    };

} // namespace SFT::Renderer
