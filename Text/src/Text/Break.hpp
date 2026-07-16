#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <vector>
#pragma endregion

using std::vector;

namespace SFT::Text {

    enum class LineBreakKind {
        Allowed,
        Mandatory,
    };

    struct LineBreakOpportunity {
        // UTF-8 byte boundary immediately after the character where the break may occur.
        usize byte_index = 0;
        LineBreakKind kind = LineBreakKind::Allowed;
    };

    // Unicode UAX #14 line-break opportunities. `language` is an optional BCP-47-ish language
    // code used by libunibreak's small tailoring table (for example "en" or "zh").
    [[nodiscard]] vector<LineBreakOpportunity> line_break_opportunities(const ustr &text,
                                                                        const ustr &language = {});

    // Unicode UAX #29 extended grapheme and word boundaries as UTF-8 byte indices. Both include
    // 0 and text.byte_size(), making them directly usable as safe slice/caret boundaries.
    [[nodiscard]] vector<usize> grapheme_boundaries(const ustr &text, const ustr &language = {});

    [[nodiscard]] vector<usize> word_boundaries(const ustr &text, const ustr &language = {});

} // namespace SFT::Text
