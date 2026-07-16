#include "Layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace SFT::Text {

namespace {

[[nodiscard]] bool is_trimmable_line_end(char32_t codepoint) noexcept {
    if (codepoint == U'\t' || codepoint == U' ') {
        return true;
    }
    const hb_unicode_general_category_t category = hb_unicode_general_category(
        hb_unicode_funcs_get_default(), static_cast<hb_codepoint_t>(codepoint));
    return category == HB_UNICODE_GENERAL_CATEGORY_SPACE_SEPARATOR;
}

[[nodiscard]] usize visible_end_before_space(const ustr &text, usize start, usize end) noexcept {
    usize offset = start;
    usize last_visible_end = start;
    while (offset < end) {
        const Detail::DecodedCodepoint decoded = Detail::decode_utf8(text, offset);
        offset += decoded.length;
        if (!is_trimmable_line_end(decoded.value)) {
            last_visible_end = offset;
        }
    }
    return last_visible_end;
}

[[nodiscard]] usize paragraph_content_end(string_view bytes, usize boundary) noexcept {
    if (boundary >= 2 && bytes[boundary - 2] == '\r' && bytes[boundary - 1] == '\n') {
        return boundary - 2;
    }
    if (boundary >= 1 && (bytes[boundary - 1] == '\r' || bytes[boundary - 1] == '\n')) {
        return boundary - 1;
    }
    if (boundary >= 2 && static_cast<unsigned char>(bytes[boundary - 2]) == 0xC2 &&
        static_cast<unsigned char>(bytes[boundary - 1]) == 0x85) {
        return boundary - 2; // NEL
    }
    if (boundary >= 3 && static_cast<unsigned char>(bytes[boundary - 3]) == 0xE2 &&
        static_cast<unsigned char>(bytes[boundary - 2]) == 0x80 &&
        (static_cast<unsigned char>(bytes[boundary - 1]) == 0xA8 ||
         static_cast<unsigned char>(bytes[boundary - 1]) == 0xA9)) {
        return boundary - 3; // LINE/PARAGRAPH SEPARATOR
    }
    return boundary;
}

void rebase_clusters(ShapedLine &line, usize byte_offset) noexcept {
    for (ShapedRun &run : line.runs) {
        for (PositionedGlyph &glyph : run.glyphs) {
            glyph.cluster += static_cast<u32>(byte_offset);
        }
    }
}

[[nodiscard]] TextExpected<ShapedLine> shape_source_range(const FontStack &fonts, const ustr &text,
                                                          usize start, usize end,
                                                          const ShapeOptions &options) {
    if (start == end) {
        return ShapedLine{
            .runs = {},
            .base_direction = options.direction == TextDirection::RightToLeft
                                  ? TextDirection::RightToLeft
                                  : TextDirection::LeftToRight,
            .advance_em = 0.0f,
        };
    }
    const string_view bytes = text.cpp_string_view();
    const ustr line_text{bytes.substr(start, end - start)};
    ShapeOptions sliced_options = options;
    sliced_options.features.custom.clear();
    const u64 range_start = start;
    const u64 range_end = end;
    for (const OpenTypeFeatureSetting &setting : options.features.custom) {
        if (setting.start == HB_FEATURE_GLOBAL_START && setting.end == HB_FEATURE_GLOBAL_END) {
            sliced_options.features.custom.push_back(setting);
            continue;
        }
        const u64 intersection_start = std::max<u64>(setting.start, range_start);
        const u64 intersection_end = std::min<u64>(setting.end, range_end);
        if (intersection_start < intersection_end) {
            sliced_options.features.custom.push_back(OpenTypeFeatureSetting{
                .tag = setting.tag,
                .value = setting.value,
                .start = static_cast<u32>(intersection_start - range_start),
                .end = static_cast<u32>(intersection_end - range_start),
            });
        }
    }
    auto shaped = shape_line_with_fallback(fonts, line_text, sliced_options);
    if (shaped) {
        rebase_clusters(*shaped, start);
    }
    return shaped;
}

void include_font_metrics(const Font *font, f32 &ascender, f32 &descender, f32 &line_gap) noexcept {
    if (font == nullptr || !*font) {
        return;
    }
    const f32 inverse_em = 1.0f / static_cast<f32>(std::max(font->units_per_em(), 1u));
    ascender = std::max(ascender, static_cast<f32>(font->ascender()) * inverse_em);
    descender = std::min(descender, static_cast<f32>(font->descender()) * inverse_em);
    line_gap = std::max(line_gap, static_cast<f32>(font->line_gap()) * inverse_em);
}

} // namespace

TextExpected<TextLayout> layout_text(const FontStack &fonts, const ustr &text,
                                     const TextLayoutOptions &options) {
    if (fonts.primary == nullptr || !*fonts.primary) {
        return text_error(TextErrorCode::InvalidArgument, "layout_text requires a valid primary font.");
    }
    if (options.max_width_em && (!std::isfinite(*options.max_width_em) || *options.max_width_em <= 0.0f)) {
        return text_error(TextErrorCode::InvalidArgument,
                          "Text layout width must be finite and positive when provided.");
    }

    f32 ascender_em = 0.0f;
    f32 descender_em = 0.0f;
    f32 line_gap_em = 0.0f;
    include_font_metrics(fonts.primary, ascender_em, descender_em, line_gap_em);
    include_font_metrics(fonts.emoji, ascender_em, descender_em, line_gap_em);
    for (const FallbackFont &fallback : fonts.fallbacks) {
        include_font_metrics(fallback.font, ascender_em, descender_em, line_gap_em);
    }
    TextLayout layout{
        .lines = {},
        .width_em = 0.0f,
        .ascender_em = ascender_em,
        .descender_em = descender_em,
        .line_gap_em = line_gap_em,
        .line_height_em = std::max(ascender_em - descender_em + line_gap_em, 1.0f),
        .height_em = 0.0f,
    };

    const ustr break_language = options.break_language.empty() ? options.shape.language.as_ustr()
                                                                : options.break_language.as_ustr();
    const vector<LineBreakOpportunity> opportunities = line_break_opportunities(text, break_language);
    const vector<usize> graphemes = grapheme_boundaries(text, break_language);
    const string_view bytes = text.cpp_string_view();

    auto append_segment = [&](usize segment_start, usize segment_end, bool mandatory_break_after,
                              span<const usize> soft_breaks) -> TextResult {
        if (segment_start == segment_end) {
            auto shaped = shape_source_range(fonts, text, segment_start, segment_end, options.shape);
            if (!shaped) {
                return std::unexpected(shaped.error());
            }
            layout.lines.push_back(LaidOutLine{
                .byte_start = segment_start,
                .visible_byte_end = segment_end,
                .byte_end = segment_end,
                .mandatory_break_after = mandatory_break_after,
                .shaped = std::move(*shaped),
            });
            return {};
        }

        usize line_start = segment_start;
        while (line_start < segment_end) {
            vector<usize> candidates;
            for (usize boundary : soft_breaks) {
                if (boundary > line_start && boundary <= segment_end) {
                    candidates.push_back(boundary);
                }
            }
            if (candidates.empty() || candidates.back() != segment_end) {
                candidates.push_back(segment_end);
            }
            if (!options.max_width_em) {
                candidates.assign(1, segment_end);
            }

            usize chosen_end = 0;
            usize chosen_visible_end = 0;
            optional<ShapedLine> chosen_shape;
            for (usize candidate : candidates) {
                const usize visible_end = visible_end_before_space(text, line_start, candidate);
                auto shaped = shape_source_range(fonts, text, line_start, visible_end, options.shape);
                if (!shaped) {
                    return std::unexpected(shaped.error());
                }
                if (!options.max_width_em || shaped->advance_em <= *options.max_width_em + 0.0001f) {
                    chosen_end = candidate;
                    chosen_visible_end = visible_end;
                    chosen_shape = std::move(*shaped);
                } else {
                    break;
                }
            }

            if (!chosen_shape) {
                // The first normal opportunity is too wide. Walk extended grapheme boundaries so
                // even an unspaced token can wrap without bisecting a combining/ZWJ sequence.
                const auto first_grapheme_it = std::ranges::upper_bound(graphemes, line_start);
                if (first_grapheme_it == graphemes.end()) {
                    return text_error(TextErrorCode::ShapingFailed,
                                      "Text wrapping found no grapheme boundary after the line start.");
                }
                const usize first_grapheme = *first_grapheme_it;
                for (usize boundary : graphemes) {
                    if (boundary <= line_start || boundary > segment_end) {
                        continue;
                    }
                    auto shaped = shape_source_range(fonts, text, line_start, boundary, options.shape);
                    if (!shaped) {
                        return std::unexpected(shaped.error());
                    }
                    if (!options.max_width_em || shaped->advance_em <= *options.max_width_em + 0.0001f ||
                        boundary == first_grapheme) {
                        chosen_end = boundary;
                        chosen_visible_end = boundary;
                        chosen_shape = std::move(*shaped);
                        if (options.max_width_em && chosen_shape->advance_em > *options.max_width_em + 0.0001f) {
                            break; // one grapheme is intrinsically wider than the constraint
                        }
                    } else {
                        break;
                    }
                }
            }

            if (!chosen_shape || chosen_end <= line_start) {
                return text_error(TextErrorCode::ShapingFailed,
                                  "Text wrapping could not make forward progress at a grapheme boundary.");
            }
            const bool is_segment_end = chosen_end == segment_end;
            layout.lines.push_back(LaidOutLine{
                .byte_start = line_start,
                .visible_byte_end = chosen_visible_end,
                .byte_end = chosen_end,
                .mandatory_break_after = is_segment_end && mandatory_break_after,
                .shaped = std::move(*chosen_shape),
            });
            line_start = chosen_end;
        }
        return {};
    };

    usize paragraph_start = 0;
    vector<usize> paragraph_soft_breaks;
    bool ended_with_mandatory_break = false;
    for (const LineBreakOpportunity &opportunity : opportunities) {
        if (opportunity.kind == LineBreakKind::Allowed) {
            paragraph_soft_breaks.push_back(opportunity.byte_index);
            continue;
        }
        const usize content_end = paragraph_content_end(bytes, opportunity.byte_index);
        if (TextResult appended = append_segment(paragraph_start, content_end, true, paragraph_soft_breaks); !appended) {
            return std::unexpected(appended.error());
        }
        // The visible content excludes the paragraph separator, but the final logical line owns
        // those source bytes so consecutive LaidOutLine ranges still partition the full input.
        layout.lines.back().byte_end = opportunity.byte_index;
        paragraph_start = opportunity.byte_index;
        paragraph_soft_breaks.clear();
        ended_with_mandatory_break = paragraph_start == bytes.size();
    }

    if (paragraph_start < bytes.size() || layout.lines.empty()) {
        if (TextResult appended = append_segment(paragraph_start, bytes.size(), false, paragraph_soft_breaks); !appended) {
            return std::unexpected(appended.error());
        }
    } else if (ended_with_mandatory_break) {
        if (TextResult appended = append_segment(bytes.size(), bytes.size(), false, {}); !appended) {
            return std::unexpected(appended.error());
        }
    }

    for (const LaidOutLine &line : layout.lines) {
        layout.width_em = std::max(layout.width_em, line.shaped.advance_em);
    }
    const f32 alignment_width = options.max_width_em.value_or(layout.width_em);
    for (LaidOutLine &line : layout.lines) {
        const f32 remaining = std::max(0.0f, alignment_width - line.shaped.advance_em);
        switch (options.alignment) {
            case TextAlignment::Center:
                line.offset_em = remaining * 0.5f;
                break;
            case TextAlignment::Start:
                line.offset_em = line.shaped.base_direction == TextDirection::RightToLeft ? remaining : 0.0f;
                break;
            case TextAlignment::End:
                line.offset_em = line.shaped.base_direction == TextDirection::RightToLeft ? 0.0f : remaining;
                break;
        }
    }
    layout.height_em = static_cast<f32>(layout.lines.size()) * layout.line_height_em;
    return layout;
}

} // namespace SFT::Text
