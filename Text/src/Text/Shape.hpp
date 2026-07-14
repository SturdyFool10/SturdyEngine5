#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <hb.h>
#include <span>
#include <string_view>
#include <vector>
#pragma endregion

#include "Error.hpp"
#include "Font.hpp"

using std::span;
using std::string_view;
using std::vector;

namespace SFT::Text {

    enum class TextDirection {
        LeftToRight,
        RightToLeft,
        TopToBottom,
        BottomToTop,
    };

    // One OpenType feature to force on/off (or to a specific alternate index) during shaping,
    // overriding the font's own default feature set for the whole input — the "settings" half of
    // OpenType feature support; see Text::available_features (Text/Features.cppm) for the
    // "detection" half that lists what a font actually offers. `tag` is a packed 4-character code
    // (Text::feature_tag("liga")); `value` is 0 to disable, 1 (or any non-zero) to enable, or a
    // higher index to select an alternate for lookup-type-3 features (stylistic sets, 'salt').
    struct OpenTypeFeatureSetting {
        u32 tag = 0;
        u32 value = 1;
    };

    // Shaping input beyond the raw text: HarfBuzz needs a script/language/direction to pick the
    // right shaping rules (ligatures, reordering, mark positioning). `script` is an ISO 15924 tag
    // ("Latn", "Arab", ...), `language` a BCP 47 tag ("en", "ar-EG", ...); leave either empty to
    // let HarfBuzz guess from the text itself. `features` overrides specific OpenType features for
    // this shape() call (e.g. Text::disable_ligatures()); empty uses the font's defaults.
    struct ShapeOptions {
        string_view script;
        string_view language;
        TextDirection direction = TextDirection::LeftToRight;
        span<const OpenTypeFeatureSetting> features;
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

        [[nodiscard]] inline hb_direction_t to_hb_direction(TextDirection direction) noexcept {
            switch (direction) {
                case TextDirection::LeftToRight: return HB_DIRECTION_LTR;
                case TextDirection::RightToLeft: return HB_DIRECTION_RTL;
                case TextDirection::TopToBottom: return HB_DIRECTION_TTB;
                case TextDirection::BottomToTop: return HB_DIRECTION_BTT;
            }
            return HB_DIRECTION_LTR;
        }

    } // namespace Detail

    // Shapes UTF-8 text against `font` — HarfBuzz turns character codepoints into positioned glyph
    // indices, applying the font's ligatures, kerning, and script-specific reordering/mark
    // positioning. Font units, not pixels: multiply by (pixel_size / font.units_per_em()) to lay
    // out on screen.
    [[nodiscard]] inline TextExpected<vector<PositionedGlyph>> shape(const Font &font, string_view utf8,
                                                                     const ShapeOptions &options = {}) {
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

        hb_buffer_add_utf8(buffer, utf8.data(), static_cast<int>(utf8.size()), 0, static_cast<int>(utf8.size()));

        hb_buffer_set_direction(buffer, Detail::to_hb_direction(options.direction));
        if (!options.script.empty()) {
            hb_buffer_set_script(buffer, hb_script_from_string(options.script.data(), static_cast<int>(options.script.size())));
        }
        if (!options.language.empty()) {
            hb_buffer_set_language(buffer, hb_language_from_string(options.language.data(), static_cast<int>(options.language.size())));
        }
        hb_buffer_guess_segment_properties(buffer);

        vector<hb_feature_t> hb_features;
        hb_features.reserve(options.features.size());
        for (const OpenTypeFeatureSetting &setting : options.features) {
            hb_features.push_back(hb_feature_t{
                .tag = setting.tag,
                .value = setting.value,
                .start = HB_FEATURE_GLOBAL_START,
                .end = HB_FEATURE_GLOBAL_END,
            });
        }
        hb_shape(font.handle(), buffer, hb_features.empty() ? nullptr : hb_features.data(),
                static_cast<unsigned int>(hb_features.size()));

        const unsigned int glyph_count = hb_buffer_get_length(buffer);
        const hb_glyph_info_t *infos = hb_buffer_get_glyph_infos(buffer, nullptr);
        const hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(buffer, nullptr);
        if (glyph_count > 0 && (infos == nullptr || positions == nullptr)) {
            hb_buffer_destroy(buffer);
            return text_error(TextErrorCode::ShapingFailed, "HarfBuzz produced no glyph info/position data.");
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

} // namespace SFT::Text
