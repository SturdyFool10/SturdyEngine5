#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <span>
#include <vector>
#pragma endregion

#include "Error.hpp"
#include "Font.hpp"
#include "Shape.hpp"

using std::vector;
using std::span;

namespace SFT::Text {

    struct FallbackFont {
        const Font *font = nullptr;
        u64 font_id = 0;
        bool is_color = false;
    };

    // A primary font, a dedicated emoji face, and an ordered coverage-driven fallback chain.
    // IDs are caller-assigned opaque identities stamped onto positioned glyphs for atlas lookup.
    struct FontStack {
        const Font *primary = nullptr;
        const Font *emoji = nullptr;
        u64 primary_font_id = 0;
        u64 emoji_font_id = 0;
        // Ordered, coverage-driven fallbacks after the primary face. These are used for missing
        // glyphs in any script; the dedicated emoji face still wins for emoji-presentation runs.
        span<const FallbackFont> fallbacks{};
        bool emoji_is_color = true;
    };

    // One homogeneous font + bidi run in visual left-to-right order. Glyph positions retain
    // HarfBuzz's native signed advances; `pen_origin_em` shifts a zero-based run origin so those
    // advances fit inside [0, advance_em], regardless of whether the run flows LTR or RTL.
    struct ShapedRun {
        vector<PositionedGlyph> glyphs;
        TextDirection direction = TextDirection::LeftToRight;
        u64 font_id = 0;
        u32 units_per_em = 1000;
        bool is_color = false;
        f32 pen_origin_em = 0.0f;
        f32 advance_em = 0.0f;
    };

    struct ShapedLine {
        vector<ShapedRun> runs;
        TextDirection base_direction = TextDirection::LeftToRight;
        f32 advance_em = 0.0f;
    };

    // Unicode 17 Emoji_Presentation lookup plus the joiner/selector/tag components used to build
    // emoji sequences. Text-default symbols move to the emoji face only when followed by VS16;
    // VS15 keeps even emoji-default symbols in text presentation when coverage permits.
    [[nodiscard]] bool is_emoji_codepoint(char32_t codepoint) noexcept;

    namespace Detail {

        struct DecodedCodepoint {
            char32_t value = 0;
            usize length = 1;
        };

        [[nodiscard]] DecodedCodepoint decode_utf8(const ustr &text, usize offset) noexcept;

    } // namespace Detail

    // Shapes coverage-selected font runs and merges them in logical source order. Use
    // shape_line_with_fallback() when the caller also needs UAX #9 visual bidi run ordering.
    [[nodiscard]] TextExpected<vector<PositionedGlyph>> shape_with_fallback(const FontStack &fonts, const ustr &utf8,
                                                                            const ShapeOptions &options = {});

    [[nodiscard]] TextExpected<vector<PositionedGlyph>> shape_with_fallback(
        const FontStack &fonts, const ustr &utf8, const OpenTypeFeatureOptions &features);

    [[nodiscard]] TextExpected<vector<PositionedGlyph>> shape_with_fallback(
        const FontStack &fonts, const ustr &utf8, const ustr &comma_separated_features,
        const ShapeOptions &options = {});

    // Full UAX #9 line itemization. SheenBidi resolves paragraph levels, isolates, brackets, and
    // visual run order; each resulting directional/font run is then shaped by HarfBuzz. The flat
    // shape_with_fallback() API remains for callers that already own run layout.
    [[nodiscard]] TextExpected<ShapedLine> shape_line_with_fallback(const FontStack &fonts, const ustr &utf8,
                                                                    const ShapeOptions &options = {});

    [[nodiscard]] TextExpected<ShapedLine> shape_line_with_fallback(
        const FontStack &fonts, const ustr &utf8, const OpenTypeFeatureOptions &features);

    [[nodiscard]] TextExpected<ShapedLine> shape_line_with_fallback(
        const FontStack &fonts, const ustr &utf8, const ustr &comma_separated_features,
        const ShapeOptions &options = {});

} // namespace SFT::Text
