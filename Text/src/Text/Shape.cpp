#include "Shape.hpp"

#include <span>

using std::span;

namespace SFT::Text::Detail {

hb_direction_t to_hb_direction(TextDirection direction) noexcept {
    switch (direction) {
    case TextDirection::LeftToRight:
        return HB_DIRECTION_LTR;
    case TextDirection::RightToLeft:
        return HB_DIRECTION_RTL;
    case TextDirection::TopToBottom:
        return HB_DIRECTION_TTB;
    case TextDirection::BottomToTop:
        return HB_DIRECTION_BTT;
    }
    return HB_DIRECTION_LTR;
}

} // namespace SFT::Text::Detail

namespace SFT::Text {

namespace {

TextExpected<vector<PositionedGlyph>> shape_resolved(const Font &font, const ustr &utf8,
                                                     const ShapeOptions &options,
                                                     span<const OpenTypeFeatureSetting> settings) {
    if (!font) {
        return text_error(TextErrorCode::InvalidArgument, "Cannot shape text with an invalid font.");
    }

    hb_buffer_t *buffer = hb_buffer_create();
    if (buffer == nullptr || !hb_buffer_allocation_successful(buffer)) {
        if (buffer != nullptr) {
            hb_buffer_destroy(buffer);
        }
        return text_error(TextErrorCode::ShapingFailed, "Failed to allocate a HarfBuzz shaping buffer.");
    }

    hb_buffer_add_utf8(buffer, utf8.data(), static_cast<int>(utf8.byte_size()), 0,
                       static_cast<int>(utf8.byte_size()));

    hb_buffer_set_direction(buffer, Detail::to_hb_direction(options.direction));
    if (!options.script.empty()) {
        hb_buffer_set_script(buffer, hb_script_from_string(options.script.data(),
                                                            static_cast<int>(options.script.byte_size())));
    }
    if (!options.language.empty()) {
        hb_buffer_set_language(buffer, hb_language_from_string(options.language.data(),
                                                                static_cast<int>(options.language.byte_size())));
    }
    hb_buffer_guess_segment_properties(buffer);

    vector<hb_feature_t> hb_features;
    hb_features.reserve(settings.size());
    for (const OpenTypeFeatureSetting &setting : settings) {
        hb_features.push_back(hb_feature_t{
            .tag = setting.tag,
            .value = setting.value,
            .start = setting.start,
            .end = setting.end,
        });
    }
    hb_shape(font.handle(), buffer, hb_features.empty() ? nullptr : hb_features.data(),
             static_cast<unsigned int>(hb_features.size()));

    const unsigned int glyph_count = hb_buffer_get_length(buffer);
    const hb_glyph_info_t *infos = hb_buffer_get_glyph_infos(buffer, nullptr);
    const hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(buffer, nullptr);
    if (glyph_count > 0 && (infos == nullptr || positions == nullptr)) {
        hb_buffer_destroy(buffer);
        return text_error(TextErrorCode::ShapingFailed,
                          "HarfBuzz produced no glyph info/position data.");
    }

    vector<PositionedGlyph> glyphs;
    glyphs.reserve(glyph_count);
    for (unsigned int i = 0; i < glyph_count; ++i) {
        glyphs.push_back(PositionedGlyph{
            .glyph_id = infos[i].codepoint,
            .x_advance = static_cast<f32>(positions[i].x_advance),
            .y_advance = static_cast<f32>(positions[i].y_advance),
            .x_offset = static_cast<f32>(positions[i].x_offset),
            .y_offset = static_cast<f32>(positions[i].y_offset),
            .cluster = infos[i].cluster,
        });
    }

    hb_buffer_destroy(buffer);
    return glyphs;
}

} // namespace

TextExpected<vector<PositionedGlyph>> shape(const Font &font, const ustr &utf8,
                                            const ShapeOptions &options) {
    const vector<OpenTypeFeatureSetting> settings = feature_settings(options.features);
    return shape_resolved(font, utf8, options, settings);
}

TextExpected<vector<PositionedGlyph>> shape(const Font &font, const ustr &utf8,
                                            const OpenTypeFeatureOptions &features) {
    ShapeOptions options;
    options.features = features;
    return shape(font, utf8, options);
}

TextExpected<vector<PositionedGlyph>> shape(const Font &font, const ustr &utf8,
                                            const ustr &comma_separated_features,
                                            const ShapeOptions &options) {
    auto parsed = parse_feature_settings(comma_separated_features);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    ShapeOptions resolved = options;
    resolved.features.custom.insert(resolved.features.custom.end(), parsed->begin(), parsed->end());
    return shape(font, utf8, resolved);
}

} // namespace SFT::Text
