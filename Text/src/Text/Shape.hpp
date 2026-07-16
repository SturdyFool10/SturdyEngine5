#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <hb.h>
#include <vector>
#pragma endregion

#include "Error.hpp"
#include "Features.hpp"
#include "Font.hpp"

using std::vector;

namespace SFT::Text {

    enum class TextDirection {
        Auto,
        LeftToRight,
        RightToLeft,
        TopToBottom,
        BottomToTop,
    };

    // Shaping input beyond the raw text: HarfBuzz needs a script/language/direction to pick the
    // right shaping rules (ligatures, reordering, mark positioning). `script` is an ISO 15924 tag
    // ("Latn", "Arab", ...), `language` a BCP 47 tag ("en", "ar-EG", ...); leave either empty to
    // let HarfBuzz guess from the text itself. `features` overrides specific OpenType features for
    // this shape() call (e.g. Text::disable_ligatures()); empty uses the font's defaults.
    struct ShapeOptions {
        UString script;
        UString language;
        TextDirection direction = TextDirection::Auto;
        OpenTypeFeatureOptions features;
    };

    // One shaped glyph: which glyph in the font to draw, and where to place it relative to the
    // previous glyph's pen position. All values are in font design units (see
    // `Font::units_per_em()`) — a caller scales by the target pixel size before layout.
    struct PositionedGlyph {
        u32 glyph_id = 0;
        f32 x_advance = 0.0f;
        f32 y_advance = 0.0f;
        f32 x_offset = 0.0f;
        f32 y_offset = 0.0f;
        // Byte offset into the source UTF-8 text this glyph originated from — lets a caller map
        // glyphs back to source text ranges (selection, hit-testing, ligature grouping).
        u32 cluster = 0;
        // Which font this glyph was shaped against — 0 (the default) for a plain shape() call
        // against a single font; shape_with_fallback() (Text/FontFallback.cppm) sets this so a
        // downstream atlas lookup knows which font's outline/rasterization to use per glyph.
        u64 font_id = 0;
        // Set by shape_with_fallback() when this glyph came from the emoji font — tells the atlas
        // to rasterize via Text::rasterize_color_glyph (Text/ColorGlyph.cppm) instead of the
        // monochrome SDF/MSDF outline path.
        bool is_color = false;
    };

    namespace Detail {

        [[nodiscard]] hb_direction_t to_hb_direction(TextDirection direction) noexcept;

    } // namespace Detail

    // Shapes UTF-8 text against `font` — HarfBuzz turns character codepoints into positioned glyph
    // indices, applying the font's ligatures, kerning, and script-specific reordering/mark
    // positioning. Font units, not pixels: multiply by (pixel_size / font.units_per_em()) to lay
    // out on screen.
    [[nodiscard]] TextExpected<vector<PositionedGlyph>> shape(const Font &font, const ustr &utf8,
                                                              const ShapeOptions &options = {});

    // Direct typed-feature convenience: shape(font, text,
    // OpenTypeFeatureOptions{.calt = 1, .clig = 1, .liga = 0}).
    [[nodiscard]] TextExpected<vector<PositionedGlyph>> shape(const Font &font, const ustr &utf8,
                                                              const OpenTypeFeatureOptions &features);

    // Convenience overload for comma-separated feature expressions, e.g.
    // shape(font, text, "calt, liga, clig"_ustr). Parsed settings override same-tag typed settings
    // already present in `options`.
    [[nodiscard]] TextExpected<vector<PositionedGlyph>> shape(const Font &font, const ustr &utf8,
                                                              const ustr &comma_separated_features,
                                                              const ShapeOptions &options = {});

} // namespace SFT::Text
