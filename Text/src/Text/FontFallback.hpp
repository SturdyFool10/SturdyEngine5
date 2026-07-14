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
    [[nodiscard]] inline bool is_emoji_codepoint(char32_t codepoint) noexcept {
        if (codepoint >= 0x1F300 && codepoint <= 0x1FAFF) {
            return true; // misc symbols & pictographs, emoticons, transport, supplemental symbols/pictographs, symbols & pictographs extended-A
        }
        if (codepoint >= 0x1F000 && codepoint <= 0x1F0FF) {
            return true; // mahjong tiles / dominoes / playing cards (also emoji-rendered on most platforms)
        }
        if (codepoint >= 0x2600 && codepoint <= 0x27BF) {
            return true; // misc symbols, dingbats
        }
        if (codepoint >= 0x1F1E6 && codepoint <= 0x1F1FF) {
            return true; // regional indicators (flag emoji are pairs of these)
        }
        if (codepoint == 0xFE0F) {
            return true; // variation selector-16 (forces emoji presentation)
        }
        if (codepoint == 0x200D) {
            return true; // zero-width joiner (ZWJ sequences: family/skin-tone/profession emoji)
        }
        return false;
    }

    namespace Detail {

        struct DecodedCodepoint {
            char32_t value = 0;
            usize length = 1;
        };

        [[nodiscard]] inline DecodedCodepoint decode_utf8(string_view text, usize offset) noexcept {
            const auto lead = static_cast<unsigned char>(text[offset]);
            if (lead < 0x80) {
                return DecodedCodepoint{lead, 1};
            }
            usize length = 1;
            char32_t value = 0;
            if ((lead & 0xE0) == 0xC0) {
                length = 2;
                value = lead & 0x1F;
            } else if ((lead & 0xF0) == 0xE0) {
                length = 3;
                value = lead & 0x0F;
            } else if ((lead & 0xF8) == 0xF0) {
                length = 4;
                value = lead & 0x07;
            } else {
                return DecodedCodepoint{0xFFFD, 1}; // invalid lead byte
            }
            if (offset + length > text.size()) {
                return DecodedCodepoint{0xFFFD, 1};
            }
            for (usize i = 1; i < length; ++i) {
                const auto continuation = static_cast<unsigned char>(text[offset + i]);
                if ((continuation & 0xC0) != 0x80) {
                    return DecodedCodepoint{0xFFFD, 1};
                }
                value = (value << 6) | (continuation & 0x3F);
            }
            return DecodedCodepoint{value, length};
        }

    } // namespace Detail

    // Shapes UTF-8 text against `fonts.primary`, itemizing runs of emoji-range codepoints
    // (is_emoji_codepoint()) out to `fonts.emoji` when one is set, then re-merges the shaped runs
    // in source order with `PositionedGlyph::cluster` rebased back onto the original string and
    // `font_id`/`is_color` stamped per glyph. This is font-run itemization only, not full Unicode
    // bidi segmentation — `options.direction` is passed through to each run's shape() call exactly
    // as plain shape() already does, no new directional handling is added here.
    [[nodiscard]] inline TextExpected<vector<PositionedGlyph>> shape_with_fallback(const FontStack &fonts, string_view utf8,
                                                                                   const ShapeOptions &options = {}) {
        if (fonts.primary == nullptr || !*fonts.primary) {
            return text_error(TextErrorCode::InvalidArgument, "shape_with_fallback requires a valid primary font.");
        }

        struct Run {
            usize start = 0;
            usize length = 0;
            bool is_emoji = false;
        };
        vector<Run> runs;
        const bool have_emoji_font = fonts.emoji != nullptr && *fonts.emoji;
        usize offset = 0;
        while (offset < utf8.size()) {
            const Detail::DecodedCodepoint decoded = Detail::decode_utf8(utf8, offset);
            const bool emoji = have_emoji_font && is_emoji_codepoint(decoded.value);
            if (!runs.empty() && runs.back().is_emoji == emoji) {
                runs.back().length += decoded.length;
            } else {
                runs.push_back(Run{offset, decoded.length, emoji});
            }
            offset += decoded.length;
        }

        vector<PositionedGlyph> merged;
        for (const Run &run : runs) {
            const Font &font = run.is_emoji ? *fonts.emoji : *fonts.primary;
            const u64 font_id = run.is_emoji ? fonts.emoji_font_id : fonts.primary_font_id;
            auto glyphs = shape(font, utf8.substr(run.start, run.length), options);
            if (!glyphs) {
                return glyphs;
            }
            for (PositionedGlyph &glyph : *glyphs) {
                glyph.cluster += static_cast<u32>(run.start);
                glyph.font_id = font_id;
                glyph.is_color = run.is_emoji;
            }
            merged.insert(merged.end(), glyphs->begin(), glyphs->end());
        }
        return merged;
    }

} // namespace SFT::Text
