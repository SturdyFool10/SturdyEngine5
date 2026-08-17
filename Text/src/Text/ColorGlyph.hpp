#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <hb.h>
#include <hb-ot.h>


#define STBI_ONLY_PNG
#include <stb_image.h>
#include <vector>
#pragma endregion

#include "Error.hpp"
#include "Font.hpp"
#include "Outline.hpp"
#include "Raster.hpp"

using std::vector;

namespace SFT::Text {


    enum class ColorGlyphFormat {
        None,
        Bitmap,
        Layered,
    };

    /// Performs the detect color format operation using the supplied arguments.
    ///
    /// @param font `font` value used by the operation.
    /// @param glyph_id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] ColorGlyphFormat detect_color_format(const Font &font, u32 glyph_id);

    struct ColorRasterParams {
        u32 width = 0;
        u32 height = 0;


        f32 pixel_size = 32.0f;


        f32 padding_px = 2.0f;
    };

    namespace Detail {

        /// Rasterizes bitmap glyph using the supplied arguments and current state.
        ///
        /// @param font `font` value used by the operation.
        /// @param glyph_id Identifier of the target object or resource.
        /// @param params `params` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `TextErrorCode::RasterizationFailed`.
        [[nodiscard]] TextExpected<RasterizedGlyph> rasterize_bitmap_glyph(const Font &font, u32 glyph_id,
                                                                                   const ColorRasterParams &params);

        /// Rasterizes layered glyph using the supplied arguments and current state.
        ///
        /// @param font `font` value used by the operation.
        /// @param glyph_id Identifier of the target object or resource.
        /// @param params `params` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `TextErrorCode::RasterizationFailed`.
        [[nodiscard]] TextExpected<RasterizedGlyph> rasterize_layered_glyph(const Font &font, u32 glyph_id,
                                                                                    const ColorRasterParams &params);

    } // namespace Detail


    /// Rasterizes color glyph using the supplied arguments and current state.
    ///
    /// @param font `font` value used by the operation.
    /// @param glyph_id Identifier of the target object or resource.
    /// @param params `params` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] TextExpected<RasterizedGlyph> rasterize_color_glyph(const Font &font, u32 glyph_id,
                                                                             const ColorRasterParams &params);

} // namespace SFT::Text
