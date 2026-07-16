#include "Features.hpp"

#include <algorithm>
#include <format>

using std::string_view;

namespace SFT::Text {

UString feature_name(u32 tag) {
    struct Entry {
        u32 tag;
        const char *name;
    };
    static constexpr array<Entry, 33> table{{
        {feature_tag("aalt"), "Access All Alternates"},
        {feature_tag("calt"), "Contextual Alternates"},
        {feature_tag("case"), "Case-Sensitive Forms"},
        {feature_tag("ccmp"), "Glyph Composition/Decomposition"},
        {feature_tag("clig"), "Contextual Ligatures"},
        {feature_tag("cpsp"), "Capital Spacing"},
        {feature_tag("curs"), "Cursive Positioning"},
        {feature_tag("dlig"), "Discretionary Ligatures"},
        {feature_tag("dnom"), "Denominators"},
        {feature_tag("frac"), "Fractions"},
        {feature_tag("kern"), "Kerning"},
        {feature_tag("liga"), "Standard Ligatures"},
        {feature_tag("lnum"), "Lining Figures"},
        {feature_tag("locl"), "Localized Forms"},
        {feature_tag("mark"), "Mark Positioning"},
        {feature_tag("mkmk"), "Mark-to-Mark Positioning"},
        {feature_tag("numr"), "Numerators"},
        {feature_tag("onum"), "Oldstyle Figures"},
        {feature_tag("ordn"), "Ordinals"},
        {feature_tag("pnum"), "Proportional Figures"},
        {feature_tag("rclt"), "Required Contextual Alternates"},
        {feature_tag("rlig"), "Required Ligatures"},
        {feature_tag("salt"), "Stylistic Alternates"},
        {feature_tag("sinf"), "Scientific Inferiors"},
        {feature_tag("smcp"), "Small Capitals"},
        {feature_tag("c2sc"), "Small Capitals From Capitals"},
        {feature_tag("subs"), "Subscript"},
        {feature_tag("sups"), "Superscript"},
        {feature_tag("swsh"), "Swash"},
        {feature_tag("titl"), "Titling"},
        {feature_tag("tnum"), "Tabular Figures"},
        {feature_tag("zero"), "Slashed Zero"},
        {feature_tag("rvrn"), "Required Variation Alternates"},
    }};
    for (const Entry &entry : table) {
        if (entry.tag == tag) {
            return UString{entry.name};
        }
    }
    const char first = static_cast<char>((tag >> 24u) & 0xFFu);
    const char second = static_cast<char>((tag >> 16u) & 0xFFu);
    const char third = static_cast<char>((tag >> 8u) & 0xFFu);
    const char fourth = static_cast<char>(tag & 0xFFu);
    if ((first == 's' && second == 's') || (first == 'c' && second == 'v')) {
        if (third >= '0' && third <= '9' && fourth >= '0' && fourth <= '9') {
            const unsigned int index = static_cast<unsigned int>(third - '0') * 10u +
                                       static_cast<unsigned int>(fourth - '0');
            return std::format("{} {}", first == 's' ? "Stylistic Set" : "Character Variant", index);
        }
    }
    return {};
}

namespace {

void set_feature(vector<OpenTypeFeatureSetting> &settings, OpenTypeFeatureSetting setting) {
    auto existing = std::ranges::find_if(settings, [&](const OpenTypeFeatureSetting &candidate) {
        return candidate.tag == setting.tag && candidate.start == setting.start && candidate.end == setting.end;
    });
    if (existing == settings.end()) {
        settings.push_back(setting);
    } else {
        *existing = setting;
    }
}

void set_typed_feature(vector<OpenTypeFeatureSetting> &settings, const char tag[5], const optional<u32> &value) {
    if (value) {
        set_feature(settings, OpenTypeFeatureSetting{.tag = feature_tag(tag), .value = *value});
    }
}

[[nodiscard]] string_view trim_ascii_whitespace(string_view token) noexcept {
    constexpr string_view whitespace = " \t\r\n\f\v";
    const usize first = token.find_first_not_of(whitespace);
    if (first == string_view::npos) {
        return {};
    }
    const usize last = token.find_last_not_of(whitespace);
    return token.substr(first, last - first + 1);
}

} // namespace

vector<OpenTypeFeatureSetting> feature_settings(const OpenTypeFeatureOptions &features) {
    vector<OpenTypeFeatureSetting> settings;
    settings.reserve(32 + features.custom.size());

    set_typed_feature(settings, "aalt", features.aalt);
    set_typed_feature(settings, "calt", features.calt);
    set_typed_feature(settings, "case", features.case_);
    set_typed_feature(settings, "ccmp", features.ccmp);
    set_typed_feature(settings, "clig", features.clig);
    set_typed_feature(settings, "cpsp", features.cpsp);
    set_typed_feature(settings, "curs", features.curs);
    set_typed_feature(settings, "dlig", features.dlig);
    set_typed_feature(settings, "dnom", features.dnom);
    set_typed_feature(settings, "frac", features.frac);
    set_typed_feature(settings, "kern", features.kern);
    set_typed_feature(settings, "liga", features.liga);
    set_typed_feature(settings, "lnum", features.lnum);
    set_typed_feature(settings, "locl", features.locl);
    set_typed_feature(settings, "mark", features.mark);
    set_typed_feature(settings, "mkmk", features.mkmk);
    set_typed_feature(settings, "numr", features.numr);
    set_typed_feature(settings, "onum", features.onum);
    set_typed_feature(settings, "ordn", features.ordn);
    set_typed_feature(settings, "pnum", features.pnum);
    set_typed_feature(settings, "rclt", features.rclt);
    set_typed_feature(settings, "rlig", features.rlig);
    set_typed_feature(settings, "salt", features.salt);
    set_typed_feature(settings, "sinf", features.sinf);
    set_typed_feature(settings, "smcp", features.smcp);
    set_typed_feature(settings, "c2sc", features.c2sc);
    set_typed_feature(settings, "subs", features.subs);
    set_typed_feature(settings, "sups", features.sups);
    set_typed_feature(settings, "swsh", features.swsh);
    set_typed_feature(settings, "titl", features.titl);
    set_typed_feature(settings, "tnum", features.tnum);
    set_typed_feature(settings, "zero", features.zero);

    for (const OpenTypeFeatureSetting &custom : features.custom) {
        set_feature(settings, custom);
    }
    return settings;
}

TextExpected<vector<OpenTypeFeatureSetting>> parse_feature_settings(const ustr &specification) {
    vector<OpenTypeFeatureSetting> settings;
    const string_view text = specification.cpp_string_view();
    if (text.empty()) {
        return settings;
    }

    usize offset = 0;
    while (offset <= text.size()) {
        const usize comma = text.find(',', offset);
        const usize end = comma == string_view::npos ? text.size() : comma;
        const string_view token = trim_ascii_whitespace(text.substr(offset, end - offset));
        if (token.empty()) {
            return text_error(TextErrorCode::InvalidArgument,
                              std::format("OpenType feature list contains an empty entry at byte {}.", offset));
        }

        hb_feature_t feature{};
        if (!hb_feature_from_string(token.data(), static_cast<int>(token.size()), &feature)) {
            return text_error(TextErrorCode::InvalidArgument,
                              std::format("Invalid OpenType feature expression '{}'.", token));
        }
        set_feature(settings, OpenTypeFeatureSetting{
                                  .tag = feature.tag,
                                  .value = feature.value,
                                  .start = feature.start,
                                  .end = feature.end,
                              });

        if (comma == string_view::npos) {
            break;
        }
        offset = comma + 1;
    }
    return settings;
}

} // namespace SFT::Text

namespace SFT::Text::Detail {

vector<u32> table_feature_tags(hb_face_t *face, hb_tag_t table_tag, const ustr &script,
                               const ustr &language) {
    unsigned int script_index = HB_OT_LAYOUT_NO_SCRIPT_INDEX;
    unsigned int language_index = HB_OT_LAYOUT_DEFAULT_LANGUAGE_INDEX;
    bool have_script = false;

    if (!script.empty()) {
        const hb_script_t hb_script =
            hb_script_from_string(script.data(), static_cast<int>(script.byte_size()));
        const hb_language_t hb_language =
            language.empty() ? HB_LANGUAGE_INVALID
                             : hb_language_from_string(language.data(), static_cast<int>(language.byte_size()));

        array<hb_tag_t, HB_OT_MAX_TAGS_PER_SCRIPT> script_tags{};
        array<hb_tag_t, HB_OT_MAX_TAGS_PER_LANGUAGE> language_tags{};
        unsigned int script_count = static_cast<unsigned int>(script_tags.size());
        unsigned int language_count = static_cast<unsigned int>(language_tags.size());
        hb_ot_tags_from_script_and_language(hb_script, hb_language, &script_count, script_tags.data(),
                                            &language_count, language_tags.data());

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
    const unsigned int total =
        have_script ? hb_ot_layout_language_get_feature_tags(face, table_tag, script_index, language_index,
                                                              0, &probe, nullptr)
                    : hb_ot_layout_table_get_feature_tags(face, table_tag, 0, &probe, nullptr);
    if (total == 0) {
        return {};
    }
    vector<hb_tag_t> tags(total);
    unsigned int written = total;
    if (have_script) {
        hb_ot_layout_language_get_feature_tags(face, table_tag, script_index, language_index, 0, &written,
                                               tags.data());
    } else {
        hb_ot_layout_table_get_feature_tags(face, table_tag, 0, &written, tags.data());
    }
    tags.resize(written);
    return vector<u32>(tags.begin(), tags.end());
}

} // namespace SFT::Text::Detail

namespace SFT::Text {

vector<OpenTypeFeature> available_features(const Font &font, const ustr &script, const ustr &language) {
    vector<OpenTypeFeature> features;
    if (!font) {
        return features;
    }
    hb_face_t *face = font.face_handle();

    for (hb_tag_t table_tag : {HB_OT_TAG_GSUB, HB_OT_TAG_GPOS}) {
        for (u32 tag : Detail::table_feature_tags(face, table_tag, script, language)) {
            const bool already_listed = std::ranges::any_of(features, [tag](const OpenTypeFeature &existing) {
                return existing.tag == tag;
            });
            if (!already_listed) {
                features.push_back(OpenTypeFeature{.tag = tag, .name = feature_name(tag)});
            }
        }
    }
    return features;
}

OpenTypeFeatureOptions disable_ligatures() {
    OpenTypeFeatureOptions features;
    features.clig = 0;
    features.dlig = 0;
    features.liga = 0;
    return features;
}

OpenTypeFeatureOptions enable_small_caps() {
    OpenTypeFeatureOptions features;
    features.smcp = 1;
    features.c2sc = 1;
    return features;
}

} // namespace SFT::Text
