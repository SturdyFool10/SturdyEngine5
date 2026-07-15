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
    [[nodiscard]] string_view feature_name(u32 tag) noexcept;

    namespace Detail {

        // Enumerates every feature tag one layout table (GSUB or GPOS) exposes, narrowed to
        // `script`/`language` when a matching script is found in the table, or the table's whole
        // feature list otherwise (an unrecognized/empty script falls back to this, same as an
        // unsupported script would).
        [[nodiscard]] vector<u32> table_feature_tags(hb_face_t *face, hb_tag_t table_tag, string_view script,
                                                             string_view language);

    } // namespace Detail

    // Enumerates every OpenType layout feature `font` supports — GSUB (substitution: ligatures,
    // small caps, stylistic sets, ...) and GPOS (positioning: kerning, mark attachment, ...)
    // combined and de-duplicated — narrowed to `script`/`language` (same ISO 15924 / BCP 47
    // conventions as Text::ShapeOptions) when given.
    [[nodiscard]] vector<OpenTypeFeature> available_features(const Font &font, string_view script = {}, string_view language = {});

    // The handful of genuinely common feature toggles, as ready-to-use ShapeOptions::features
    // arrays — not a general preset system, just the couple of settings a font-settings screen
    // most often exposes as a plain on/off checkbox.
    [[nodiscard]] vector<OpenTypeFeatureSetting> disable_ligatures();

    [[nodiscard]] vector<OpenTypeFeatureSetting> enable_small_caps();

} // namespace SFT::Text
