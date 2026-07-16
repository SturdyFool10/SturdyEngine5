#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <msdfgen.h>
#include <optional>
#include <vector>
#pragma endregion

#include "Error.hpp"
#include "Outline.hpp"

using std::vector;
using std::optional;

namespace SFT::Text {

    // Which encoding a rasterized glyph uses. SDF/MSDF are distance fields (see below); `Color` is
    // a flat RGBA8 raster with no distance math at all — color emoji glyphs (Text/ColorGlyph.cppm)
    // don't benefit from distance-field rescaling the way monochrome ink does, and real color-emoji
    // fonts ship fixed-size strikes/layers anyway, so they're just sampled straight through by the
    // text shader instead of going through the SDF/MSDF coverage-antialiasing path.
    enum class RasterFormat {
        SDF,
        MSDF,
        Color,
    };

    // Chooses SDF vs MSDF from a glyph's final on-screen long-side size, in pixels: above 32px,
    // corner sharpness becomes visible, so switch to MSDF; at or below, SDF is indistinguishable
    // and cheaper. Applies **hysteresis** (down-shift only below 28px) so a glyph whose projected
    // size oscillates near the threshold — a 3D label as the camera dollies, an animated UI scale
    // — doesn't thrash between formats and constantly re-rasterize into the atlas. Pass the
    // glyph's current cached format as `previous` (default SDF for a glyph seen for the first
    // time); 2D/UI callers that compute size once at layout time can simply omit `previous`.
    [[nodiscard]] RasterFormat select_raster_format(f32 long_side_px, RasterFormat previous = RasterFormat::SDF) noexcept;

    // A rasterized glyph: `width * height * channel_count` bytes, row-major, top-to-bottom,
    // channel values normalized to [0, 255] the way generateSDF/generateMSDF's distance mapping
    // already produces — 0.5 (encoded ~128) sits on the true glyph edge. `channel_count` is 1 for
    // RasterFormat::SDF, 3 for RasterFormat::MSDF (matching RHI::Format::R8Unorm/RGBA8Unorm* atlas
    // storage — see Renderer/TextAtlas.cppm).
    //
    // `bearing_x`/`bearing_top` place this raster relative to the pen (the glyph's origin on the
    // baseline) — the same convention FreeType's `bitmap_left`/`bitmap_top` use, since each raster
    // is tightly cropped to *this glyph's own* ink bounding box (plus `RasterParams::padding_px`),
    // not to a shared baseline-relative box: a parenthesis descends well below the baseline and
    // rises well above x-height, while a plain lowercase letter sits almost entirely within
    // [0, x-height], so two rasters of the same output `width`/`height` place their own baseline
    // at two different rows unless the caller repositions each one by its bearing. In pixel space:
    // `raster_left_edge = pen.x + bearing_x`, `raster_top_edge = pen.y - bearing_top`.
    struct RasterizedGlyph {
        u32 width = 0;
        u32 height = 0;
        u32 channel_count = 0;
        f32 bearing_x = 0.0f;
        f32 bearing_top = 0.0f;
        vector<u8> pixels;
    };

    // Axis-aligned outline bounds in font design units. Keeping this public lets the atlas choose
    // a reference ppem that fits unusually wide glyphs (ligatures, swashes, combining sequences)
    // instead of silently clipping them to its fixed-size storage cell.
    struct GlyphBounds {
        f32 left = 0.0f;
        f32 bottom = 0.0f;
        f32 right = 0.0f;
        f32 top = 0.0f;
        bool empty = true;

        [[nodiscard]] constexpr f32 width() const noexcept { return right - left; }
        [[nodiscard]] constexpr f32 height() const noexcept { return top - bottom; }
    };

    [[nodiscard]] GlyphBounds glyph_bounds(const GlyphOutline &outline);

    // Optional shared projection for several outlines that must retain their relative positions,
    // most notably the layers of a COLR glyph. Values are added in font units before scaling.
    struct RasterTranslation {
        f32 x = 0.0f;
        f32 y = 0.0f;
    };

    // Placement of the glyph within the output raster. `scale` converts font design units (see
    // Font::units_per_em()) to output pixels; `pixel_range` is the width of the encoded distance
    // band either side of the true edge, in output pixels (a larger range gives smoother
    // antialiasing falloff at the cost of atlas precision — 4px is a reasonable default);
    // `padding_px` reserves space between the glyph's ink bounding box and the raster edge so the
    // distance band isn't clipped. The caller (the atlas — Renderer/TextAtlas.cpp) picks
    // `width`/`height`/`scale` to fit its tightly packed atlas rectangle.
    struct RasterParams {
        u32 width = 0;
        u32 height = 0;
        f32 scale = 1.0f;
        f32 pixel_range = 4.0f;
        f32 padding_px = 4.0f;
        optional<RasterTranslation> translation;
    };

    namespace Detail {

        [[nodiscard]] msdfgen::Shape to_msdfgen_shape(const GlyphOutline &outline);

        [[nodiscard]] u8 to_unorm_byte(float value) noexcept;

    } // namespace Detail

    // Rasterizes `outline` into a signed distance field, single-channel (SDF) or multi-channel
    // (MSDF) per `format`. Built on msdfgen (Chlumsky) — `outline`'s contours map directly onto
    // msdfgen's own edge-segment representation (line/quadratic/cubic), so no flattening or
    // reparsing is needed between HarfBuzz's hb-draw output and msdfgen's shape input.
    [[nodiscard]] TextExpected<RasterizedGlyph> rasterize_glyph(const GlyphOutline &outline, RasterFormat format,
                                                                       const RasterParams &params);

} // namespace SFT::Text
