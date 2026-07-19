#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <array>
#include <hb.h>
#include <hb-ot.h>
#include <optional>
#include <vector>
#pragma endregion

#include "Error.hpp"
#include "Font.hpp"

using std::array;
using std::optional;
using std::vector;

namespace SFT::Text {

    // One global or range-scoped OpenType feature override. `value == 0` disables the feature,
    // `1` enables it, and higher values select indexed alternates when the feature supports them.
    // `start`/`end` are UTF-8 cluster indices, matching hb_feature_t exactly.
    struct OpenTypeFeatureSetting {
        u32 tag = 0;
        u32 value = 1;
        u32 start = HB_FEATURE_GLOBAL_START;
        u32 end = HB_FEATURE_GLOBAL_END;
    };

    // Granular typed feature controls. An unset field preserves the font/HarfBuzz default; set it
    // to 0, 1, or an alternate index to override that feature. `custom` covers arbitrary tags,
    // character variants (`cv01`...), stylistic sets (`ss01`...), and cluster-scoped overrides.
    struct OpenTypeFeatureOptions {
        optional<u32> aalt;
        optional<u32> calt;
        optional<u32> case_;
        optional<u32> ccmp;
        optional<u32> clig;
        optional<u32> cpsp;
        optional<u32> curs;
        optional<u32> dlig;
        optional<u32> dnom;
        optional<u32> frac;
        optional<u32> kern;
        optional<u32> liga;
        optional<u32> lnum;
        optional<u32> locl;
        optional<u32> mark;
        optional<u32> mkmk;
        optional<u32> numr;
        optional<u32> onum;
        optional<u32> ordn;
        optional<u32> pnum;
        optional<u32> rclt;
        optional<u32> rlig;
        optional<u32> salt;
        optional<u32> sinf;
        optional<u32> smcp;
        optional<u32> c2sc;
        optional<u32> subs;
        optional<u32> sups;
        optional<u32> swsh;
        optional<u32> titl;
        optional<u32> tnum;
        optional<u32> zero;
        vector<OpenTypeFeatureSetting> custom;
    };

    // One OpenType layout feature a font supports (from its GSUB — substitution: ligatures, small
    // caps, stylistic sets — or GPOS — positioning: kerning, mark attachment — tables). This is
    // the "detection" half; Text::ShapeOptions::features (Text/Shape.cppm) is the "settings" half
    // that actually turns one on/off during shaping.
    struct OpenTypeFeature {
        u32 tag = 0;
        UString name; // human-readable if this is a recognized common tag (feature_name()), else empty
    };

    // Packs a 4-character OpenType feature tag (e.g. "liga") into the u32 form OpenTypeFeature and
    // OpenTypeFeatureSetting both use.
    [[nodiscard]] constexpr u32 feature_tag(const char code[5]) noexcept {
        return HB_TAG(static_cast<unsigned char>(code[0]), static_cast<unsigned char>(code[1]),
                      static_cast<unsigned char>(code[2]), static_cast<unsigned char>(code[3]));
    }

    // A human-readable name for the common registered OpenType feature tags a font-settings UI is
    // most likely to expose — not the full ~150-entry OpenType feature registry. Returns an empty
    // UString for any tag not in this table; callers should fall back to displaying the raw
    // 4-character tag in that case.
    [[nodiscard]] UString feature_name(u32 tag);

    // Flattens the typed fields plus `custom` into HarfBuzz settings. Later custom entries replace
    // an earlier typed/custom entry with the same tag and range.
    [[nodiscard]] vector<OpenTypeFeatureSetting> feature_settings(const OpenTypeFeatureOptions &features);

    // Parses comma-separated HarfBuzz feature expressions. Besides the simple
    // "calt, liga, clig" form this accepts disabling (`-liga` or `liga=0`), alternate indices
    // (`salt=2`), and HarfBuzz cluster ranges. Malformed/empty entries return InvalidArgument.
    [[nodiscard]] TextExpected<vector<OpenTypeFeatureSetting>> parse_feature_settings(const ustr &specification);

    namespace Detail {

        // Enumerates every feature tag one layout table (GSUB or GPOS) exposes, narrowed to
        // `script`/`language` when a matching script is found in the table, or the table's whole
        // feature list otherwise (an unrecognized/empty script falls back to this, same as an
        // unsupported script would).
        [[nodiscard]] vector<u32> table_feature_tags(hb_face_t *face, hb_tag_t table_tag, const ustr &script,
                                                     const ustr &language);

    } // namespace Detail

    // Enumerates every OpenType layout feature `font` supports — GSUB (substitution: ligatures,
    // small caps, stylistic sets, ...) and GPOS (positioning: kerning, mark attachment, ...)
    // combined and de-duplicated — narrowed to `script`/`language` (same ISO 15924 / BCP 47
    // conventions as Text::ShapeOptions) when given.
    [[nodiscard]] vector<OpenTypeFeature> available_features(const Font &font, const ustr &script = ustr{},
                                                              const ustr &language = ustr{});

    // The handful of genuinely common feature toggles, as ready-to-use ShapeOptions::features
    // arrays — not a general preset system, just the couple of settings a font-settings screen
    // most often exposes as a plain on/off checkbox.
    [[nodiscard]] OpenTypeFeatureOptions disable_ligatures();

    [[nodiscard]] OpenTypeFeatureOptions enable_small_caps();

} // namespace SFT::Text
