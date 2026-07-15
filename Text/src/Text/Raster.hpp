#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <msdfgen.h>
#include <vector>
#pragma endregion

#include "Error.hpp"
#include "Outline.hpp"

using std::vector;

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
    struct RasterizedGlyph {
        u32 width = 0;
        u32 height = 0;
        u32 channel_count = 0;
        vector<u8> pixels;
    };

    // Placement of the glyph within the output raster. `scale` converts font design units (see
    // Font::units_per_em()) to output pixels; `pixel_range` is the width of the encoded distance
    // band either side of the true edge, in output pixels (a larger range gives smoother
    // antialiasing falloff at the cost of atlas precision — 4px is a reasonable default);
    // `padding_px` reserves space between the glyph's ink bounding box and the raster edge so the
    // distance band isn't clipped. The caller (the atlas — Renderer/TextAtlas.cppm) picks
    // `width`/`height`/`scale` to fit its cell grid.
    struct RasterParams {
        u32 width = 0;
        u32 height = 0;
        f32 scale = 1.0f;
        f32 pixel_range = 4.0f;
        f32 padding_px = 4.0f;
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
