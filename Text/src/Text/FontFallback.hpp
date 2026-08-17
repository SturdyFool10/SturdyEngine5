#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <span>
#include <vector>
#pragma endregion

#include "Error.hpp"
#include "Font.hpp"
#include "Shape.hpp"

using std::vector;
using std::span;

namespace SFT::Text {

    struct FallbackFont {
        const Font *font = nullptr;
        u64 font_id = 0;
        bool is_color = false;
    };


    struct FontStack {
        const Font *primary = nullptr;
        const Font *emoji = nullptr;
        u64 primary_font_id = 0;
        u64 emoji_font_id = 0;


        span<const FallbackFont> fallbacks{};
        bool emoji_is_color = true;
    };


    struct ShapedRun {
        vector<PositionedGlyph> glyphs;
        TextDirection direction = TextDirection::LeftToRight;
        u64 font_id = 0;
        u32 units_per_em = 1000;
        bool is_color = false;
        f32 pen_origin_em = 0.0f;
        f32 advance_em = 0.0f;
    };

    struct ShapedLine {
        vector<ShapedRun> runs;
        TextDirection base_direction = TextDirection::LeftToRight;
        f32 advance_em = 0.0f;
    };


    /// Reports whether emoji codepoint holds.
    ///
    /// @param codepoint `codepoint` value used by the operation.
    ///
    /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool is_emoji_codepoint(char32_t codepoint) noexcept;

    namespace Detail {

        struct DecodedCodepoint {
            char32_t value = 0;
            usize length = 1;
        };

        /// Decodes UTF-8.
        ///
        /// @param text Text consumed by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] DecodedCodepoint decode_utf8(const ustr &text, usize offset) noexcept;

    } // namespace Detail


    /// Shapes with fallback using the supplied arguments and current state.
    ///
    /// @param fonts `fonts` value used by the operation.
    /// @param utf8 `utf8` value used by the operation.
    /// @param options Configuration values controlling the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `TextErrorCode::InvalidArgument`.
    [[nodiscard]] TextExpected<vector<PositionedGlyph>> shape_with_fallback(const FontStack &fonts, const ustr &utf8,
                                                                            const ShapeOptions &options = {});

    /// Shapes with fallback using the supplied arguments and current state.
    ///
    /// @param fonts `fonts` value used by the operation.
    /// @param utf8 `utf8` value used by the operation.
    /// @param features `features` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] TextExpected<vector<PositionedGlyph>> shape_with_fallback(
        const FontStack &fonts, const ustr &utf8, const OpenTypeFeatureOptions &features);

    /// Shapes with fallback using the supplied arguments and current state.
    ///
    /// @param fonts `fonts` value used by the operation.
    /// @param utf8 `utf8` value used by the operation.
    /// @param comma_separated_features `comma_separated_features` value used by the operation.
    /// @param options Configuration values controlling the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] TextExpected<vector<PositionedGlyph>> shape_with_fallback(
        const FontStack &fonts, const ustr &utf8, const ustr &comma_separated_features,
        const ShapeOptions &options = {});


    /// Shapes line with fallback using the supplied arguments and current state.
    ///
    /// @param fonts `fonts` value used by the operation.
    /// @param utf8 `utf8` value used by the operation.
    /// @param options Configuration values controlling the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `TextErrorCode::InvalidArgument`, `TextErrorCode::ShapingFailed`.
    [[nodiscard]] TextExpected<ShapedLine> shape_line_with_fallback(const FontStack &fonts, const ustr &utf8,
                                                                    const ShapeOptions &options = {});

    /// Shapes line with fallback using the supplied arguments and current state.
    ///
    /// @param fonts `fonts` value used by the operation.
    /// @param utf8 `utf8` value used by the operation.
    /// @param features `features` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] TextExpected<ShapedLine> shape_line_with_fallback(
        const FontStack &fonts, const ustr &utf8, const OpenTypeFeatureOptions &features);

    [[nodiscard]] TextExpected<ShapedLine> shape_line_with_fallback(
        const FontStack &fonts, const ustr &utf8, const ustr &comma_separated_features,
        const ShapeOptions &options = {});

} // namespace SFT::Text
