#include "FontFallback.hpp"

#include <string_view>

using std::string_view;

namespace SFT::Text {

bool is_emoji_codepoint(char32_t codepoint) noexcept {
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

} // namespace SFT::Text

namespace SFT::Text::Detail {

DecodedCodepoint decode_utf8(const ustr &text, usize offset) noexcept {
            const string_view bytes = text.cpp_string_view();
            const auto lead = static_cast<unsigned char>(bytes[offset]);
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
            if (offset + length > bytes.size()) {
                return DecodedCodepoint{0xFFFD, 1};
            }
            for (usize i = 1; i < length; ++i) {
                const auto continuation = static_cast<unsigned char>(bytes[offset + i]);
                if ((continuation & 0xC0) != 0x80) {
                    return DecodedCodepoint{0xFFFD, 1};
                }
                value = (value << 6) | (continuation & 0x3F);
            }
            return DecodedCodepoint{value, length};
        }

} // namespace SFT::Text::Detail

namespace SFT::Text {

TextExpected<vector<PositionedGlyph>> shape_with_fallback(const FontStack &fonts, const ustr &utf8,
                                                                                   const ShapeOptions &options) {
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
        const string_view utf8_bytes = utf8.cpp_string_view();
        while (offset < utf8_bytes.size()) {
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
            const ustr run_text{utf8_bytes.substr(run.start, run.length)};
            auto glyphs = shape(font, run_text, options);
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

TextExpected<vector<PositionedGlyph>> shape_with_fallback(const FontStack &fonts, const ustr &utf8,
                                                          const OpenTypeFeatureOptions &features) {
        ShapeOptions options;
        options.features = features;
        return shape_with_fallback(fonts, utf8, options);
    }

TextExpected<vector<PositionedGlyph>> shape_with_fallback(const FontStack &fonts, const ustr &utf8,
                                                          const ustr &comma_separated_features,
                                                          const ShapeOptions &options) {
        auto parsed = parse_feature_settings(comma_separated_features);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        ShapeOptions resolved = options;
        resolved.features.custom.insert(resolved.features.custom.end(), parsed->begin(), parsed->end());
        return shape_with_fallback(fonts, utf8, resolved);
    }

} // namespace SFT::Text
