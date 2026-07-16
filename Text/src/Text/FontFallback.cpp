#include "FontFallback.hpp"

#include <SheenBidi/SheenBidi.h>
#include <algorithm>
#include <array>
#include <string_view>

using std::string_view;

namespace SFT::Text {

bool is_emoji_codepoint(char32_t codepoint) noexcept {
        struct Range {
            char32_t first;
            char32_t last;
        };
        // Generated from Unicode 17's Emoji_Presentation binary property. Keeping the compact
        // ranges locally avoids a heavyweight ICU runtime dependency for one stable property.
        static constexpr std::array presentation_ranges{
            Range{0x231A, 0x231B}, Range{0x23E9, 0x23EC}, Range{0x23F0, 0x23F0},
            Range{0x23F3, 0x23F3}, Range{0x25FD, 0x25FE}, Range{0x2614, 0x2615},
            Range{0x2648, 0x2653}, Range{0x267F, 0x267F}, Range{0x2693, 0x2693},
            Range{0x26A1, 0x26A1}, Range{0x26AA, 0x26AB}, Range{0x26BD, 0x26BE},
            Range{0x26C4, 0x26C5}, Range{0x26CE, 0x26CE}, Range{0x26D4, 0x26D4},
            Range{0x26EA, 0x26EA}, Range{0x26F2, 0x26F3}, Range{0x26F5, 0x26F5},
            Range{0x26FA, 0x26FA}, Range{0x26FD, 0x26FD}, Range{0x2705, 0x2705},
            Range{0x270A, 0x270B}, Range{0x2728, 0x2728}, Range{0x274C, 0x274C},
            Range{0x274E, 0x274E}, Range{0x2753, 0x2755}, Range{0x2757, 0x2757},
            Range{0x2795, 0x2797}, Range{0x27B0, 0x27B0}, Range{0x27BF, 0x27BF},
            Range{0x2B1B, 0x2B1C}, Range{0x2B50, 0x2B50}, Range{0x2B55, 0x2B55},
            Range{0x1F004, 0x1F004}, Range{0x1F0CF, 0x1F0CF}, Range{0x1F18E, 0x1F18E},
            Range{0x1F191, 0x1F19A}, Range{0x1F1E6, 0x1F1FF}, Range{0x1F201, 0x1F201},
            Range{0x1F21A, 0x1F21A}, Range{0x1F22F, 0x1F22F}, Range{0x1F232, 0x1F236},
            Range{0x1F238, 0x1F23A}, Range{0x1F250, 0x1F251}, Range{0x1F300, 0x1F320},
            Range{0x1F32D, 0x1F335}, Range{0x1F337, 0x1F37C}, Range{0x1F37E, 0x1F393},
            Range{0x1F3A0, 0x1F3CA}, Range{0x1F3CF, 0x1F3D3}, Range{0x1F3E0, 0x1F3F0},
            Range{0x1F3F4, 0x1F3F4}, Range{0x1F3F8, 0x1F43E}, Range{0x1F440, 0x1F440},
            Range{0x1F442, 0x1F4FC}, Range{0x1F4FF, 0x1F53D}, Range{0x1F54B, 0x1F54E},
            Range{0x1F550, 0x1F567}, Range{0x1F57A, 0x1F57A}, Range{0x1F595, 0x1F596},
            Range{0x1F5A4, 0x1F5A4}, Range{0x1F5FB, 0x1F64F}, Range{0x1F680, 0x1F6C5},
            Range{0x1F6CC, 0x1F6CC}, Range{0x1F6D0, 0x1F6D2}, Range{0x1F6D5, 0x1F6D8},
            Range{0x1F6DC, 0x1F6DF}, Range{0x1F6EB, 0x1F6EC}, Range{0x1F6F4, 0x1F6FC},
            Range{0x1F7E0, 0x1F7EB}, Range{0x1F7F0, 0x1F7F0}, Range{0x1F90C, 0x1F93A},
            Range{0x1F93C, 0x1F945}, Range{0x1F947, 0x1F9FF}, Range{0x1FA70, 0x1FA7C},
            Range{0x1FA80, 0x1FA8A}, Range{0x1FA8E, 0x1FAC6}, Range{0x1FAC8, 0x1FAC8},
            Range{0x1FACD, 0x1FADC}, Range{0x1FADF, 0x1FAEA}, Range{0x1FAEF, 0x1FAF8},
        };
        for (const Range range : presentation_ranges) {
            if (codepoint < range.first) {
                break;
            }
            if (codepoint <= range.last) {
                return true;
            }
        }
        return codepoint == 0xFE0F || codepoint == 0x200D || codepoint == 0x20E3 ||
               (codepoint >= 0xE0020 && codepoint <= 0xE007F);
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

namespace {

struct FontChoice {
    const Font *font = nullptr;
    u64 font_id = 0;
    bool is_color = false;

    [[nodiscard]] friend bool operator==(const FontChoice &, const FontChoice &) noexcept = default;
};

struct SourceCodepoint {
    char32_t value = 0;
    usize start = 0;
    usize length = 0;
};

struct FontRun {
    usize start = 0;
    usize length = 0;
    FontChoice choice{};
};

[[nodiscard]] bool font_has_codepoint(const FontChoice &choice, char32_t codepoint) noexcept {
    if (choice.font == nullptr || !*choice.font) {
        return false;
    }
    hb_codepoint_t glyph = 0;
    return hb_font_get_nominal_glyph(choice.font->handle(), static_cast<hb_codepoint_t>(codepoint), &glyph);
}

[[nodiscard]] bool is_cluster_extender(char32_t codepoint) noexcept {
    if (codepoint == 0x200D || codepoint == 0xFE0E || codepoint == 0xFE0F || codepoint == 0x20E3 ||
        (codepoint >= 0x1F3FB && codepoint <= 0x1F3FF) ||
        (codepoint >= 0xE0020 && codepoint <= 0xE007F)) {
        return true;
    }
    return hb_unicode_combining_class(hb_unicode_funcs_get_default(), static_cast<hb_codepoint_t>(codepoint)) != 0;
}

[[nodiscard]] vector<SourceCodepoint> decode_source(const ustr &text, usize source_start, usize source_length) {
    vector<SourceCodepoint> decoded;
    usize offset = source_start;
    const usize end = source_start + source_length;
    while (offset < end) {
        const Detail::DecodedCodepoint codepoint = Detail::decode_utf8(text, offset);
        decoded.push_back(SourceCodepoint{.value = codepoint.value, .start = offset, .length = codepoint.length});
        offset += codepoint.length;
    }
    return decoded;
}

[[nodiscard]] bool wants_emoji_presentation(span<const SourceCodepoint> decoded, usize index) noexcept {
    const char32_t codepoint = decoded[index].value;
    // Explicit presentation selectors override the base codepoint's default property.
    if (index + 1 < decoded.size() && decoded[index + 1].value == 0xFE0E) {
        return false;
    }
    // A text-default symbol followed by VS16, and the ASCII base of a keycap sequence, must move
    // to the emoji font together with its extenders rather than being split into separate runs.
    if (index + 1 < decoded.size() && decoded[index + 1].value == 0xFE0F) {
        return true;
    }
    const bool keycap_base = codepoint == U'#' || codepoint == U'*' || (codepoint >= U'0' && codepoint <= U'9');
    if (keycap_base && index + 1 < decoded.size()) {
        if (decoded[index + 1].value == 0x20E3) {
            return true;
        }
        if (index + 2 < decoded.size() && decoded[index + 1].value == 0xFE0F &&
            decoded[index + 2].value == 0x20E3) {
            return true;
        }
    }
    return is_emoji_codepoint(codepoint);
}

[[nodiscard]] FontChoice select_font(const FontStack &fonts, char32_t codepoint, bool emoji_presentation) noexcept {
    const FontChoice primary{fonts.primary, fonts.primary_font_id, false};
    const FontChoice emoji{fonts.emoji, fonts.emoji_font_id, fonts.emoji_is_color};

    if (emoji_presentation && emoji.font != nullptr && *emoji.font &&
        (font_has_codepoint(emoji, codepoint) || is_cluster_extender(codepoint))) {
        return emoji;
    }
    if (font_has_codepoint(primary, codepoint)) {
        return primary;
    }
    for (const FallbackFont &fallback : fonts.fallbacks) {
        const FontChoice candidate{fallback.font, fallback.font_id, fallback.is_color};
        if (font_has_codepoint(candidate, codepoint)) {
            return candidate;
        }
    }
    if (font_has_codepoint(emoji, codepoint)) {
        return emoji;
    }
    // Preserve HarfBuzz's .notdef behavior in the primary font when no face covers the scalar.
    return primary;
}

[[nodiscard]] bool coverage_ignorable(char32_t codepoint) noexcept {
    return codepoint == 0x200C || codepoint == 0x200D || codepoint == 0xFE0E || codepoint == 0xFE0F ||
           (codepoint >= 0xE0020 && codepoint <= 0xE007F);
}

[[nodiscard]] bool font_covers_range(const FontChoice &choice, span<const SourceCodepoint> decoded,
                                     usize first, usize last) noexcept {
    if (choice.font == nullptr || !*choice.font) {
        return false;
    }
    for (usize i = first; i < last; ++i) {
        if (!coverage_ignorable(decoded[i].value) && !font_has_codepoint(choice, decoded[i].value)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_neutral_script(hb_script_t script) noexcept {
    return script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED || script == HB_SCRIPT_UNKNOWN ||
           script == HB_SCRIPT_INVALID;
}

void append_font_run(vector<FontRun> &runs, const vector<SourceCodepoint> &decoded,
                     usize first, usize last, FontChoice choice) {
    if (first >= last) {
        return;
    }
    const usize start = decoded[first].start;
    const usize end = decoded[last - 1].start + decoded[last - 1].length;
    if (!runs.empty() && runs.back().choice == choice && runs.back().start + runs.back().length == start) {
        runs.back().length = end - runs.back().start;
    } else {
        runs.push_back(FontRun{.start = start, .length = end - start, .choice = choice});
    }
}

[[nodiscard]] vector<FontRun> itemize_fonts(const FontStack &fonts, const ustr &text,
                                            usize source_start, usize source_length) {
    const vector<SourceCodepoint> decoded = decode_source(text, source_start, source_length);
    vector<FontRun> runs;
    if (decoded.empty()) {
        return runs;
    }

    vector<bool> emoji(decoded.size());
    vector<hb_script_t> scripts(decoded.size(), HB_SCRIPT_COMMON);
    for (usize i = 0; i < decoded.size(); ++i) {
        emoji[i] = wants_emoji_presentation(decoded, i);
        if (i > 0 && is_cluster_extender(decoded[i].value)) {
            // Selectors, joiners, modifiers, tags, and combining marks belong to their base's
            // presentation run; their standalone property must never split the cluster.
            emoji[i] = emoji[i - 1];
        }
        scripts[i] = hb_unicode_script(hb_unicode_funcs_get_default(),
                                       static_cast<hb_codepoint_t>(decoded[i].value));
    }

    // Common/Inherited characters (spaces, punctuation, combining marks) adopt a neighboring
    // concrete script so they do not split a joining run. The reverse pass resolves leading
    // neutrals that had no concrete script to their left.
    hb_script_t previous_script = HB_SCRIPT_INVALID;
    for (usize i = 0; i < scripts.size(); ++i) {
        if (is_neutral_script(scripts[i])) {
            if (!is_neutral_script(previous_script)) {
                scripts[i] = previous_script;
            }
        } else {
            previous_script = scripts[i];
        }
    }
    hb_script_t next_script = HB_SCRIPT_INVALID;
    for (usize i = scripts.size(); i-- > 0;) {
        if (is_neutral_script(scripts[i])) {
            if (!is_neutral_script(next_script)) {
                scripts[i] = next_script;
            }
        } else {
            next_script = scripts[i];
        }
    }

    usize segment_start = 0;
    while (segment_start < decoded.size()) {
        usize segment_end = segment_start + 1;
        while (segment_end < decoded.size() && emoji[segment_end] == emoji[segment_start] &&
               (emoji[segment_start] || scripts[segment_end] == scripts[segment_start])) {
            ++segment_end;
        }

        const FontChoice primary{fonts.primary, fonts.primary_font_id, false};
        const FontChoice emoji_face{fonts.emoji, fonts.emoji_font_id, fonts.emoji_is_color};
        optional<FontChoice> whole_run_font;
        if (emoji[segment_start] && font_covers_range(emoji_face, decoded, segment_start, segment_end)) {
            whole_run_font = emoji_face;
        } else if (!emoji[segment_start] && font_covers_range(primary, decoded, segment_start, segment_end)) {
            whole_run_font = primary;
        } else {
            for (const FallbackFont &fallback : fonts.fallbacks) {
                const FontChoice candidate{fallback.font, fallback.font_id, fallback.is_color};
                if (font_covers_range(candidate, decoded, segment_start, segment_end)) {
                    whole_run_font = candidate;
                    break;
                }
            }
        }

        if (whole_run_font) {
            append_font_run(runs, decoded, segment_start, segment_end, *whole_run_font);
            segment_start = segment_end;
            continue;
        }

        // No candidate covers the whole script run. Fall back at scalar/cluster granularity while
        // keeping every extender with its base, accepting .notdef rather than severing attachment.
        for (usize i = segment_start; i < segment_end; ++i) {
            FontChoice choice = select_font(fonts, decoded[i].value, emoji[i]);
            if (!runs.empty() && is_cluster_extender(decoded[i].value)) {
                choice = runs.back().choice;
            }
            append_font_run(runs, decoded, i, i + 1, choice);
        }
        segment_start = segment_end;
    }
    return runs;
}

[[nodiscard]] ShapeOptions options_for_source_range(const ShapeOptions &options, usize start, usize length,
                                                     TextDirection direction) {
    ShapeOptions resolved = options;
    resolved.direction = direction;
    resolved.features.custom.clear();
    const u64 range_start = start;
    const u64 range_end = start + length;
    for (const OpenTypeFeatureSetting &setting : options.features.custom) {
        if (setting.start == HB_FEATURE_GLOBAL_START && setting.end == HB_FEATURE_GLOBAL_END) {
            resolved.features.custom.push_back(setting);
            continue;
        }
        const u64 intersection_start = std::max<u64>(setting.start, range_start);
        const u64 intersection_end = std::min<u64>(setting.end, range_end);
        if (intersection_start < intersection_end) {
            resolved.features.custom.push_back(OpenTypeFeatureSetting{
                .tag = setting.tag,
                .value = setting.value,
                .start = static_cast<u32>(intersection_start - range_start),
                .end = static_cast<u32>(intersection_end - range_start),
            });
        }
    }
    return resolved;
}

[[nodiscard]] TextExpected<ShapedRun> shape_font_run(const ustr &whole_text, const FontRun &run,
                                                     TextDirection direction, const ShapeOptions &options) {
    const string_view bytes = whole_text.cpp_string_view();
    const ustr run_text{bytes.substr(run.start, run.length)};
    const ShapeOptions resolved = options_for_source_range(options, run.start, run.length, direction);
    auto glyphs = shape(*run.choice.font, run_text, resolved);
    if (!glyphs) {
        return std::unexpected(glyphs.error());
    }

    f32 pen = 0.0f;
    f32 minimum_pen = 0.0f;
    f32 maximum_pen = 0.0f;
    for (PositionedGlyph &glyph : *glyphs) {
        glyph.cluster += static_cast<u32>(run.start);
        glyph.font_id = run.choice.font_id;
        glyph.is_color = run.choice.is_color;
        minimum_pen = std::min(minimum_pen, pen);
        maximum_pen = std::max(maximum_pen, pen);
        pen += glyph.x_advance;
        minimum_pen = std::min(minimum_pen, pen);
        maximum_pen = std::max(maximum_pen, pen);
    }

    const u32 units_per_em = std::max(run.choice.font->units_per_em(), 1u);
    const f32 inverse_em = 1.0f / static_cast<f32>(units_per_em);
    return ShapedRun{
        .glyphs = std::move(*glyphs),
        .direction = direction,
        .font_id = run.choice.font_id,
        .units_per_em = units_per_em,
        .is_color = run.choice.is_color,
        .pen_origin_em = -minimum_pen * inverse_em,
        .advance_em = (maximum_pen - minimum_pen) * inverse_em,
    };
}

} // namespace

TextExpected<vector<PositionedGlyph>> shape_with_fallback(const FontStack &fonts, const ustr &utf8,
                                                                                   const ShapeOptions &options) {
        if (fonts.primary == nullptr || !*fonts.primary) {
            return text_error(TextErrorCode::InvalidArgument, "shape_with_fallback requires a valid primary font.");
        }

        const string_view utf8_bytes = utf8.cpp_string_view();
        vector<PositionedGlyph> merged;
        for (const FontRun &run : itemize_fonts(fonts, utf8, 0, utf8_bytes.size())) {
            const ShapeOptions resolved = options_for_source_range(options, run.start, run.length, options.direction);
            const ustr run_text{utf8_bytes.substr(run.start, run.length)};
            auto glyphs = shape(*run.choice.font, run_text, resolved);
            if (!glyphs) {
                return glyphs;
            }
            for (PositionedGlyph &glyph : *glyphs) {
                glyph.cluster += static_cast<u32>(run.start);
                glyph.font_id = run.choice.font_id;
                glyph.is_color = run.choice.is_color;
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

TextExpected<ShapedLine> shape_line_with_fallback(const FontStack &fonts, const ustr &utf8,
                                                   const ShapeOptions &options) {
        if (fonts.primary == nullptr || !*fonts.primary) {
            return text_error(TextErrorCode::InvalidArgument,
                              "shape_line_with_fallback requires a valid primary font.");
        }
        if (options.direction == TextDirection::TopToBottom || options.direction == TextDirection::BottomToTop) {
            return text_error(TextErrorCode::InvalidArgument,
                              "shape_line_with_fallback currently accepts horizontal text directions only.");
        }
        if (utf8.empty()) {
            return ShapedLine{
                .runs = {},
                .base_direction = options.direction == TextDirection::RightToLeft
                                      ? TextDirection::RightToLeft
                                      : TextDirection::LeftToRight,
                .advance_em = 0.0f,
            };
        }

        const string_view bytes = utf8.cpp_string_view();
        const SBCodepointSequence sequence{
            .stringEncoding = SBStringEncodingUTF8,
            .stringBuffer = bytes.data(),
            .stringLength = bytes.size(),
        };
        SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
        if (algorithm == nullptr) {
            return text_error(TextErrorCode::ShapingFailed,
                              "Failed to create the Unicode bidirectional algorithm state.");
        }

        SBUInteger paragraph_length = 0;
        SBUInteger separator_length = 0;
        SBAlgorithmGetParagraphBoundary(algorithm, 0, bytes.size(), &paragraph_length, &separator_length);
        if (paragraph_length != bytes.size() || separator_length != 0) {
            SBAlgorithmRelease(algorithm);
            return text_error(TextErrorCode::InvalidArgument,
                              "shape_line_with_fallback expects one line without paragraph separators.");
        }

        SBLevel base_level = SBLevelDefaultLTR;
        if (options.direction == TextDirection::LeftToRight) {
            base_level = 0;
        } else if (options.direction == TextDirection::RightToLeft) {
            base_level = 1;
        }
        SBParagraphRef paragraph = SBAlgorithmCreateParagraph(algorithm, 0, bytes.size(), base_level);
        if (paragraph == nullptr) {
            SBAlgorithmRelease(algorithm);
            return text_error(TextErrorCode::ShapingFailed,
                              "Failed to resolve Unicode bidirectional paragraph levels.");
        }
        SBLineRef line = SBParagraphCreateLine(paragraph, 0, bytes.size());
        if (line == nullptr) {
            SBParagraphRelease(paragraph);
            SBAlgorithmRelease(algorithm);
            return text_error(TextErrorCode::ShapingFailed,
                              "Failed to resolve Unicode bidirectional line runs.");
        }

        ShapedLine result{
            .runs = {},
            .base_direction = (SBParagraphGetBaseLevel(paragraph) & 1u) != 0
                                  ? TextDirection::RightToLeft
                                  : TextDirection::LeftToRight,
            .advance_em = 0.0f,
        };
        const SBUInteger bidi_run_count = SBLineGetRunCount(line);
        const SBRun *bidi_runs = SBLineGetRunsPtr(line);
        for (SBUInteger i = 0; i < bidi_run_count; ++i) {
            const SBRun &bidi_run = bidi_runs[i];
            const bool right_to_left = (bidi_run.level & 1u) != 0;
            const TextDirection direction = right_to_left ? TextDirection::RightToLeft : TextDirection::LeftToRight;
            vector<FontRun> font_runs = itemize_fonts(fonts, utf8, bidi_run.offset, bidi_run.length);

            auto append_run = [&](const FontRun &font_run) -> TextResult {
                auto shaped = shape_font_run(utf8, font_run, direction, options);
                if (!shaped) {
                    return std::unexpected(shaped.error());
                }
                result.advance_em += shaped->advance_em;
                result.runs.push_back(std::move(*shaped));
                return {};
            };

            if (right_to_left) {
                for (auto run = font_runs.rbegin(); run != font_runs.rend(); ++run) {
                    if (TextResult appended = append_run(*run); !appended) {
                        SBLineRelease(line);
                        SBParagraphRelease(paragraph);
                        SBAlgorithmRelease(algorithm);
                        return std::unexpected(appended.error());
                    }
                }
            } else {
                for (const FontRun &font_run : font_runs) {
                    if (TextResult appended = append_run(font_run); !appended) {
                        SBLineRelease(line);
                        SBParagraphRelease(paragraph);
                        SBAlgorithmRelease(algorithm);
                        return std::unexpected(appended.error());
                    }
                }
            }
        }

        SBLineRelease(line);
        SBParagraphRelease(paragraph);
        SBAlgorithmRelease(algorithm);
        return result;
    }

TextExpected<ShapedLine> shape_line_with_fallback(const FontStack &fonts, const ustr &utf8,
                                                   const OpenTypeFeatureOptions &features) {
        ShapeOptions options;
        options.features = features;
        return shape_line_with_fallback(fonts, utf8, options);
    }

TextExpected<ShapedLine> shape_line_with_fallback(const FontStack &fonts, const ustr &utf8,
                                                   const ustr &comma_separated_features,
                                                   const ShapeOptions &options) {
        auto parsed = parse_feature_settings(comma_separated_features);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        ShapeOptions resolved = options;
        resolved.features.custom.insert(resolved.features.custom.end(), parsed->begin(), parsed->end());
        return shape_line_with_fallback(fonts, utf8, resolved);
    }

} // namespace SFT::Text
