#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <msdfgen.h>
#include <optional>
#include <vector>
#pragma endregion

#include "Error.hpp"
#include "Outline.hpp"

using std::vector;
using std::optional;

namespace SFT::Text {


    enum class RasterFormat {
        SDF,
        MSDF,
        Color,
    };


    /// Selects raster format that best satisfies the supplied requirements.
    ///
    /// @param long_side_px `long_side_px` value used by the operation.
    /// @param previous `previous` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] RasterFormat select_raster_format(f32 long_side_px, RasterFormat previous = RasterFormat::SDF) noexcept;


    struct RasterizedGlyph {
        u32 width = 0;
        u32 height = 0;
        u32 channel_count = 0;
        f32 bearing_x = 0.0f;
        f32 bearing_top = 0.0f;
        vector<u8> pixels;
    };


    struct GlyphBounds {
        f32 left = 0.0f;
        f32 bottom = 0.0f;
        f32 right = 0.0f;
        f32 top = 0.0f;
        bool empty = true;

        /// Returns the current or globally available width value.
        ///
        /// @return Returns the current width value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr f32 width() const noexcept { return right - left; }
        /// Returns the current or globally available height value.
        ///
        /// @return Returns the current height value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr f32 height() const noexcept { return top - bottom; }
    };

    /// Performs the glyph bounds operation using the supplied arguments.
    ///
    /// @param outline `outline` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] GlyphBounds glyph_bounds(const GlyphOutline &outline);


    struct RasterTranslation {
        f32 x = 0.0f;
        f32 y = 0.0f;
    };


    struct RasterParams {
        u32 width = 0;
        u32 height = 0;
        f32 scale = 1.0f;
        f32 pixel_range = 4.0f;
        f32 padding_px = 4.0f;
        optional<RasterTranslation> translation;
    };

    namespace Detail {

        /// Converts the value to msdfgen shape representation.
        ///
        /// @param outline `outline` value used by the operation.
        ///
        /// @return Returns the value converted to msdfgen shape representation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] msdfgen::Shape to_msdfgen_shape(const GlyphOutline &outline);

        /// Converts the value to unorm byte representation.
        ///
        /// @param value Value consumed by the operation.
        ///
        /// @return Returns the value converted to unorm byte representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u8 to_unorm_byte(float value) noexcept;

    } // namespace Detail


    /// Rasterizes glyph using the supplied arguments and current state.
    ///
    /// @param outline `outline` value used by the operation.
    /// @param format Format used for the resource, render target, or conversion.
    /// @param params `params` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] TextExpected<RasterizedGlyph> rasterize_glyph(const GlyphOutline &outline, RasterFormat format,
                                                                       const RasterParams &params);

} // namespace SFT::Text
