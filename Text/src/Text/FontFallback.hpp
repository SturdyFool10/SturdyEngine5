#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <string_view>
#include <vector>
#pragma endregion

#include "Error.hpp"
#include "Font.hpp"
#include "Shape.hpp"

using std::string_view;
using std::vector;

namespace SFT::Text {

    // A primary font plus one dedicated font for emoji — not a general N-font fallback chain,
    // just the two roles asked for. `primary_font_id`/`emoji_font_id` are caller-assigned opaque
    // IDs (e.g. a hash of the font's file path) stamped onto each shaped glyph's `font_id` so a
    // downstream atlas lookup (Renderer/TextAtlas.cppm) knows which font's outline/rasterization
    // to use — this package has no notion of "font identity" of its own beyond the raw `Font&`.
    struct FontStack {
        const Font *primary = nullptr;
        const Font *emoji = nullptr;
        u64 primary_font_id = 0;
        u64 emoji_font_id = 0;
    };

    // Cheap range-table check for "is this codepoint the kind that should be looked up in an
    // emoji font" — covers the Unicode blocks that carry most pictographic/emoji content plus the
    // joiner/selector codepoints used to build sequences (flags, ZWJ family/skin-tone emoji).
    // Not a full Unicode emoji-property table (that needs the generated `emoji-data.txt` ranges,
    // hundreds of entries) — this is the practical 80/20 that correctly itemizes the vast
    // majority of real-world emoji text without vendoring Unicode Character Database data.
    [[nodiscard]] bool is_emoji_codepoint(char32_t codepoint) noexcept;

    namespace Detail {

        struct DecodedCodepoint {
            char32_t value = 0;
            usize length = 1;
        };

        [[nodiscard]] DecodedCodepoint decode_utf8(string_view text, usize offset) noexcept;

    } // namespace Detail

    // Shapes UTF-8 text against `fonts.primary`, itemizing runs of emoji-range codepoints
    // (is_emoji_codepoint()) out to `fonts.emoji` when one is set, then re-merges the shaped runs
    // in source order with `PositionedGlyph::cluster` rebased back onto the original string and
    // `font_id`/`is_color` stamped per glyph. This is font-run itemization only, not full Unicode
    // bidi segmentation — `options.direction` is passed through to each run's shape() call exactly
    // as plain shape() already does, no new directional handling is added here.
    [[nodiscard]] TextExpected<vector<PositionedGlyph>> shape_with_fallback(const FontStack &fonts, string_view utf8,
                                                                                   const ShapeOptions &options = {});

} // namespace SFT::Text
