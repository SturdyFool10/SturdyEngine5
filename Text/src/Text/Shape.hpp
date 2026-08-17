#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <hb.h>
#include <vector>
#pragma endregion

#include "Error.hpp"
#include "Features.hpp"
#include "Font.hpp"

using std::vector;

namespace SFT::Text {

    enum class TextDirection {
        Auto,
        LeftToRight,
        RightToLeft,
        TopToBottom,
        BottomToTop,
    };


    struct ShapeOptions {
        UString script;
        UString language;
        TextDirection direction = TextDirection::Auto;
        OpenTypeFeatureOptions features;
    };


    struct PositionedGlyph {
        u32 glyph_id = 0;
        f32 x_advance = 0.0f;
        f32 y_advance = 0.0f;
        f32 x_offset = 0.0f;
        f32 y_offset = 0.0f;


        u32 cluster = 0;


        u64 font_id = 0;


        bool is_color = false;
    };

    namespace Detail {

        /// Converts the value to hb direction representation.
        ///
        /// @param direction `direction` value used by the operation.
        ///
        /// @return Returns the value converted to hb direction representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] hb_direction_t to_hb_direction(TextDirection direction) noexcept;

    } // namespace Detail


    /// Shapes the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param font `font` value used by the operation.
    /// @param utf8 `utf8` value used by the operation.
    /// @param options Configuration values controlling the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] TextExpected<vector<PositionedGlyph>> shape(const Font &font, const ustr &utf8,
                                                              const ShapeOptions &options = {});


    /// Shapes the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param font `font` value used by the operation.
    /// @param utf8 `utf8` value used by the operation.
    /// @param features `features` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] TextExpected<vector<PositionedGlyph>> shape(const Font &font, const ustr &utf8,
                                                              const OpenTypeFeatureOptions &features);


    [[nodiscard]] TextExpected<vector<PositionedGlyph>> shape(const Font &font, const ustr &utf8,
                                                              const ustr &comma_separated_features,
                                                              const ShapeOptions &options = {});

} // namespace SFT::Text
