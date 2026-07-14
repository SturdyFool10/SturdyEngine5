#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <array>
#include <hb.h>
#include <hb-ot.h>
#include <string_view>
#include <vector>
#pragma endregion

#include "Font.hpp"
#include "Shape.hpp"

using std::array;
using std::string_view;
using std::vector;

namespace SFT::Text {

    // One OpenType layout feature a font supports (from its GSUB — substitution: ligatures, small
    // caps, stylistic sets — or GPOS — positioning: kerning, mark attachment — tables). This is
    // the "detection" half; Text::ShapeOptions::features (Text/Shape.cppm) is the "settings" half
    // that actually turns one on/off during shaping.
    struct OpenTypeFeature {
        u32 tag = 0;
        string_view name; // human-readable if this is a recognized common tag (feature_name()), else empty
    };

    // Packs a 4-character OpenType feature tag (e.g. "liga") into the u32 form OpenTypeFeature and
    // OpenTypeFeatureSetting both use.
    [[nodiscard]] constexpr u32 feature_tag(const char code[5]) noexcept {
        return HB_TAG(static_cast<unsigned char>(code[0]), static_cast<unsigned char>(code[1]),
                      static_cast<unsigned char>(code[2]), static_cast<unsigned char>(code[3]));
    }

    // A human-readable name for the common registered OpenType feature tags a font-settings UI is
    // most likely to expose — not the full ~150-entry OpenType feature registry. Returns an empty
    // string_view for any tag not in this table; callers should fall back to displaying the raw
    // 4-character tag in that case.
    [[nodiscard]] inline string_view feature_name(u32 tag) noexcept {
        struct Entry {
            u32 tag;
            string_view name;
        };
        static const array<Entry, 16> table{{
            {feature_tag("liga"), "Standard Ligatures"},
            {feature_tag("dlig"), "Discretionary Ligatures"},
            {feature_tag("clig"), "Contextual Ligatures"},
            {feature_tag("kern"), "Kerning"},
            {feature_tag("smcp"), "Small Capitals"},
            {feature_tag("c2sc"), "Small Capitals From Capitals"},
            {feature_tag("onum"), "Oldstyle Figures"},
            {feature_tag("lnum"), "Lining Figures"},
            {feature_tag("tnum"), "Tabular Figures"},
            {feature_tag("pnum"), "Proportional Figures"},
            {feature_tag("frac"), "Fractions"},
            {feature_tag("case"), "Case-Sensitive Forms"},
            {feature_tag("swsh"), "Swash"},
            {feature_tag("titl"), "Titling"},
            {feature_tag("subs"), "Subscript"},
            {feature_tag("sups"), "Superscript"},
        }};
        for (const Entry &entry : table) {
            if (entry.tag == tag) {
                return entry.name;
            }
        }
        return {};
    }

    namespace Detail {

        // Enumerates every feature tag one layout table (GSUB or GPOS) exposes, narrowed to
        // `script`/`language` when a matching script is found in the table, or the table's whole
        // feature list otherwise (an unrecognized/empty script falls back to this, same as an
        // unsupported script would).
        [[nodiscard]] inline vector<u32> table_feature_tags(hb_face_t *face, hb_tag_t table_tag, string_view script,
                                                             string_view language) {
            unsigned int script_index = HB_OT_LAYOUT_NO_SCRIPT_INDEX;
            unsigned int language_index = HB_OT_LAYOUT_DEFAULT_LANGUAGE_INDEX;
            bool have_script = false;

            if (!script.empty()) {
                const hb_script_t hb_script = hb_script_from_string(script.data(), static_cast<int>(script.size()));
                const hb_language_t hb_language =
                    language.empty() ? HB_LANGUAGE_INVALID : hb_language_from_string(language.data(), static_cast<int>(language.size()));

                array<hb_tag_t, HB_OT_MAX_TAGS_PER_SCRIPT> script_tags{};
                array<hb_tag_t, HB_OT_MAX_TAGS_PER_LANGUAGE> language_tags{};
                unsigned int script_count = static_cast<unsigned int>(script_tags.size());
                unsigned int language_count = static_cast<unsigned int>(language_tags.size());
                hb_ot_tags_from_script_and_language(hb_script, hb_language, &script_count, script_tags.data(), &language_count,
                                                    language_tags.data());

                for (unsigned int i = 0; i < script_count && !have_script; ++i) {
                    if (hb_ot_layout_table_find_script(face, table_tag, script_tags[i], &script_index)) {
                        have_script = true;
                        if (language_count > 0) {
                            hb_ot_layout_script_select_language(face, table_tag, script_index, language_count,
                                                                language_tags.data(), &language_index);
                        }
                    }
                }
            }

            unsigned int probe = 0;
            const unsigned int total = have_script ? hb_ot_layout_language_get_feature_tags(face, table_tag, script_index,
                                                                                             language_index, 0, &probe, nullptr)
                                                    : hb_ot_layout_table_get_feature_tags(face, table_tag, 0, &probe, nullptr);
            if (total == 0) {
                return {};
            }
            vector<hb_tag_t> tags(total);
            unsigned int written = total;
            if (have_script) {
                hb_ot_layout_language_get_feature_tags(face, table_tag, script_index, language_index, 0, &written, tags.data());
            } else {
                hb_ot_layout_table_get_feature_tags(face, table_tag, 0, &written, tags.data());
            }
            tags.resize(written);
            return vector<u32>(tags.begin(), tags.end());
        }

    } // namespace Detail

    // Enumerates every OpenType layout feature `font` supports — GSUB (substitution: ligatures,
    // small caps, stylistic sets, ...) and GPOS (positioning: kerning, mark attachment, ...)
    // combined and de-duplicated — narrowed to `script`/`language` (same ISO 15924 / BCP 47
    // conventions as Text::ShapeOptions) when given.
    [[nodiscard]] inline vector<OpenTypeFeature> available_features(const Font &font, string_view script = {}, string_view language = {}) {
        vector<OpenTypeFeature> features;
        if (!font) {
            return features;
        }
        hb_face_t *face = font.face_handle();

        for (hb_tag_t table_tag : {HB_OT_TAG_GSUB, HB_OT_TAG_GPOS}) {
            for (u32 tag : Detail::table_feature_tags(face, table_tag, script, language)) {
                bool already_listed = false;
                for (const OpenTypeFeature &existing : features) {
                    if (existing.tag == tag) {
                        already_listed = true;
                        break;
                    }
                }
                if (!already_listed) {
                    features.push_back(OpenTypeFeature{.tag = tag, .name = feature_name(tag)});
                }
            }
        }
        return features;
    }

    // The handful of genuinely common feature toggles, as ready-to-use ShapeOptions::features
    // arrays — not a general preset system, just the couple of settings a font-settings screen
    // most often exposes as a plain on/off checkbox.
    [[nodiscard]] inline vector<OpenTypeFeatureSetting> disable_ligatures() {
        return {
            OpenTypeFeatureSetting{.tag = feature_tag("liga"), .value = 0},
            OpenTypeFeatureSetting{.tag = feature_tag("clig"), .value = 0},
            OpenTypeFeatureSetting{.tag = feature_tag("dlig"), .value = 0},
        };
    }

    [[nodiscard]] inline vector<OpenTypeFeatureSetting> enable_small_caps() {
        return {
            OpenTypeFeatureSetting{.tag = feature_tag("smcp"), .value = 1},
            OpenTypeFeatureSetting{.tag = feature_tag("c2sc"), .value = 1},
        };
    }

} // namespace SFT::Text
