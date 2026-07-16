#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <span>
#include <unordered_map>
#include <vector>
#pragma endregion

#include <RHI/RHI.hpp>
#include <Core/Core.hpp>
#include <Text/Text.hpp>
#include "TileGrid.hpp"
#include "TextAtlas.hpp"
#include "TextInstance.hpp"

using std::span;
using std::unordered_map;
using std::vector;

namespace SFT::Renderer {

    // A currently-resident canvas tile, ready to composite/blit into a frame.
    struct ResidentCanvasTile {
        TileCoord coord{};
        RHI::TextureViewHandle view{};
        RHI::Rect2D logical_rect{};
    };

    // A large 2D text surface — e.g. a whole scrollable document/editor buffer — pre-rendered to
    // texture(s) for reuse across frames. Logically unbounded in size, but backed by a grid of
    // same-size RHI render-target tiles clamped to the device's actual max 2D image dimension
    // (TileGrid::clamp_tile_size, same helper the glyph atlas uses), so a canvas can never attempt
    // to allocate a single GPU image bigger than the hardware supports — the crash this class
    // exists to make structurally impossible. Only tiles recently touched or requested by
    // ensure_viewport_resident() actually own a GPU texture; the rest exist only as the logical
    // glyph placements in `tile_glyphs_`, which is what a tile is re-rendered from if it becomes
    // resident again after eviction.
    class TextCanvas {
      public:
        struct Config {
            u32 desired_tile_size = 2048;
            u32 max_resident_tiles = 64;
        };

        TextCanvas() noexcept = default;

        // `atlas`/`pipeline` must outlive the canvas — every tile render is a glyph-atlas lookup
        // (TextAtlas::ensure_resident) followed by an instanced draw (TextPipeline), reusing
        // exactly the machinery a direct (non-canvas) text draw uses.
        [[nodiscard]] static Core::RendererExpected<TextCanvas> create(RHI::RhiDevice &device, const Config &config,
                                                                       TextAtlas &atlas, TextPipeline &pipeline);

        // Records `glyphs` into the canvas's logical glyph store, splitting each glyph's quad
        // across whichever tile(s) it overlaps, and marks every touched tile dirty (re-rendered
        // the next time it's requested via ensure_viewport_resident()) rather than rendering it
        // immediately — a caller adding many runs in a row (loading a document) only pays for one
        // render per touched tile, not one per draw_run() call.
        void draw_run(span<const GlyphPlacement> glyphs);

        // Ensures every tile overlapping `viewport` (canvas logical pixels) is resident: creates
        // and (re-)renders any that are missing or dirty, evicting the least-recently-used
        // resident tile outside `viewport` if the resident budget is exceeded. Returns each
        // resident tile's view + logical rect in no particular order.
        [[nodiscard]] Core::RendererExpected<vector<ResidentCanvasTile>> ensure_viewport_resident(RHI::RhiDevice &device,
                                                                                                   RHI::Rect2D viewport);

        [[nodiscard]] u32 tile_size() const noexcept;

        void destroy(RHI::RhiDevice &device) noexcept;

      private:
        struct TileRecord {
            RHI::TextureHandle texture{};
            RHI::TextureViewHandle view{};
            RHI::TextureLayout current_layout = RHI::TextureLayout::Undefined;
            TextFrameResources text_resources{};
            bool dirty = true;
        };

        [[nodiscard]] Core::RendererResult render_tile(RHI::RhiDevice &device, TileCoord coord, TileRecord &tile);
        [[nodiscard]] Core::RendererResult evict_if_over_budget(RHI::RhiDevice &device, span<const TileCoord> keep);

        Config config_{};
        u32 tile_size_ = 0;
        TextAtlas *atlas_ = nullptr;
        TextPipeline *pipeline_ = nullptr;
        unordered_map<TileCoord, TileRecord, TileCoordHash> resident_tiles_;
        unordered_map<TileCoord, vector<GlyphPlacement>, TileCoordHash> tile_glyphs_;
        LruIndex<TileCoord, TileCoordHash> lru_;
    };

} // namespace SFT::Renderer
