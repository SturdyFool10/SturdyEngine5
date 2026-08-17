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


    struct OpenTypeFeatureSetting {
        u32 tag = 0;
        u32 value = 1;
        u32 start = HB_FEATURE_GLOBAL_START;
        u32 end = HB_FEATURE_GLOBAL_END;
    };


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


    struct OpenTypeFeature {
        u32 tag = 0;
        UString name;
    };


    /// Performs the feature tag operation using the supplied arguments.
    ///
    /// @param code `code` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr u32 feature_tag(const char code[5]) noexcept {
        return HB_TAG(static_cast<unsigned char>(code[0]), static_cast<unsigned char>(code[1]),
                      static_cast<unsigned char>(code[2]), static_cast<unsigned char>(code[3]));
    }


    /// Returns a human-readable name for the supplied feature value.
    ///
    /// @param tag `tag` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] UString feature_name(u32 tag);


    /// Performs the feature settings operation using the supplied arguments.
    ///
    /// @param features `features` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] vector<OpenTypeFeatureSetting> feature_settings(const OpenTypeFeatureOptions &features);


    /// Parses feature settings into structured state.
    ///
    /// @param specification `specification` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `TextErrorCode::InvalidArgument`.
    [[nodiscard]] TextExpected<vector<OpenTypeFeatureSetting>> parse_feature_settings(const ustr &specification);

    namespace Detail {


        /// Performs the table feature tags operation using the supplied arguments.
        ///
        /// @param face `face` value used by the operation.
        /// @param table_tag `table_tag` value used by the operation.
        /// @param script `script` value used by the operation.
        /// @param language `language` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] vector<u32> table_feature_tags(hb_face_t *face, hb_tag_t table_tag, const ustr &script,
                                                     const ustr &language);

    } // namespace Detail


    /// Performs the available features operation using the supplied arguments.
    ///
    /// @param font `font` value used by the operation.
    /// @param script `script` value used by the operation.
    /// @param language `language` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] vector<OpenTypeFeature> available_features(const Font &font, const ustr &script = ustr{},
                                                              const ustr &language = ustr{});


    /// Disables ligatures using the supplied arguments and current state.
    ///
    /// @return Returns the current disable ligatures value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] OpenTypeFeatureOptions disable_ligatures();

    /// Enables small caps using the supplied arguments and current state.
    ///
    /// @return Returns the current enable small caps value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] OpenTypeFeatureOptions enable_small_caps();

} // namespace SFT::Text
