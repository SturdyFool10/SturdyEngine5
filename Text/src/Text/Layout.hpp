#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <optional>
#include <vector>
#pragma endregion

#include "Break.hpp"
#include "Error.hpp"
#include "FontFallback.hpp"

using std::optional;
using std::vector;

namespace SFT::Text {

    enum class TextAlignment {
        Start,
        Center,
        End,
    };

    struct TextLayoutOptions {
        // Empty means no soft wrapping. Width is normalized em units: 1.0 is the requested font
        // size, so layout remains reusable at any pixel size.
        optional<f32> max_width_em;
        TextAlignment alignment = TextAlignment::Start;
        ShapeOptions shape;
        // Optional UAX #14 tailoring language. Empty inherits ShapeOptions::language.
        UString break_language;
    };

    struct LaidOutLine {
        // Source bytes consumed by this line. `visible_byte_end` excludes trailing break-space;
        // `byte_end` includes it, so adjacent lines still partition the original source exactly.
        usize byte_start = 0;
        usize visible_byte_end = 0;
        usize byte_end = 0;
        bool mandatory_break_after = false;
        ShapedLine shaped;
        f32 offset_em = 0.0f;
    };

    struct TextLayout {
        vector<LaidOutLine> lines;
        f32 width_em = 0.0f;
        // Maximum metrics across the primary, emoji, and configured fallback faces. The baseline
        // of the first line is `ascender_em` below the layout's top edge; subsequent baselines are
        // separated by `line_height_em`.
        f32 ascender_em = 0.0f;
        f32 descender_em = 0.0f;
        f32 line_gap_em = 0.0f;
        f32 line_height_em = 1.0f;
        f32 height_em = 0.0f;
    };

    // Shapes and wraps UTF-8 without splitting an extended grapheme cluster. UAX #14 supplies
    // normal opportunities; if one token is wider than the constraint, UAX #29 grapheme
    // boundaries provide the emergency breaks. Every glyph cluster is rebased to the original
    // source byte range for selection, caret movement, and hit testing.
    [[nodiscard]] TextExpected<TextLayout> layout_text(const FontStack &fonts, const ustr &text,
                                                       const TextLayoutOptions &options = {});

} // namespace SFT::Text
