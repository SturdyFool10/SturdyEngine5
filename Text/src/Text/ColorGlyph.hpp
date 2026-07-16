#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <hb.h>
#include <hb-ot.h>
// stb_image is a single-header library — STB_IMAGE_IMPLEMENTATION must be defined in exactly one
// translation unit (ColorGlyph.cpp, before this header is included there) to get the actual
// decoder definitions; this header only pulls in the declarations.
#define STBI_ONLY_PNG
#include <stb_image.h>
#include <vector>
#pragma endregion

#include "Error.hpp"
#include "Font.hpp"
#include "Outline.hpp"
#include "Raster.hpp"

using std::vector;

namespace SFT::Text {

    // Which color-glyph table (if any) a glyph is stored in. A font can carry both — `Bitmap`
    // (CBDT or Apple `sbix`, fixed-size PNG strikes) and `Layered` (COLR/CPAL, vector layers with
    // per-layer palette colors) are checked independently; a glyph with neither is an ordinary
    // monochrome outline glyph (Text::glyph_outline / the SDF/MSDF path).
    enum class ColorGlyphFormat {
        None,
        Bitmap,
        Layered,
    };

    [[nodiscard]] ColorGlyphFormat detect_color_format(const Font &font, u32 glyph_id);

    struct ColorRasterParams {
        u32 width = 0;
        u32 height = 0;
        // Desired em size in pixels — selects the nearest embedded PNG strike (CBDT/sbix ship
        // several fixed sizes; HarfBuzz picks the closest to this) and scales COLR layer outlines.
        f32 pixel_size = 32.0f;
        // Transparent guard pixels around the color image. Unlike an SDF this is not a distance
        // band; it prevents bilinear sampling from clipping or bleeding at the atlas boundary.
        f32 padding_px = 2.0f;
    };

    namespace Detail {

        [[nodiscard]] TextExpected<RasterizedGlyph> rasterize_bitmap_glyph(const Font &font, u32 glyph_id,
                                                                                   const ColorRasterParams &params);

        [[nodiscard]] TextExpected<RasterizedGlyph> rasterize_layered_glyph(const Font &font, u32 glyph_id,
                                                                                    const ColorRasterParams &params);

    } // namespace Detail

    // Rasterizes a color glyph (bitmap or layered — see detect_color_format()) into a flat RGBA8
    // cell, ready for Renderer::TextAtlas's Color sub-atlas. Fails if the glyph has neither color
    // table — callers should check detect_color_format() first (or fall back to
    // Text::rasterize_glyph for an ordinary monochrome outline glyph).
    [[nodiscard]] TextExpected<RasterizedGlyph> rasterize_color_glyph(const Font &font, u32 glyph_id,
                                                                             const ColorRasterParams &params);

} // namespace SFT::Text
