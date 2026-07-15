#include "Features.hpp"

namespace SFT::Text {

string_view feature_name(u32 tag) noexcept {
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

} // namespace SFT::Text

namespace SFT::Text::Detail {

vector<u32> table_feature_tags(hb_face_t *face, hb_tag_t table_tag, string_view script,
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

} // namespace SFT::Text::Detail

namespace SFT::Text {

vector<OpenTypeFeature> available_features(const Font &font, string_view script, string_view language) {
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

vector<OpenTypeFeatureSetting> disable_ligatures() {
        return {
            OpenTypeFeatureSetting{.tag = feature_tag("liga"), .value = 0},
            OpenTypeFeatureSetting{.tag = feature_tag("clig"), .value = 0},
            OpenTypeFeatureSetting{.tag = feature_tag("dlig"), .value = 0},
        };
    }

vector<OpenTypeFeatureSetting> enable_small_caps() {
        return {
            OpenTypeFeatureSetting{.tag = feature_tag("smcp"), .value = 1},
            OpenTypeFeatureSetting{.tag = feature_tag("c2sc"), .value = 1},
        };
    }

} // namespace SFT::Text
