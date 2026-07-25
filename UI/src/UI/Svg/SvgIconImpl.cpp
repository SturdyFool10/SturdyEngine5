#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#pragma endregion

#include "SvgIcon.hpp"

#include <lunasvg.h>

namespace SFT::UI::Svg {

    std::optional<RasterizedSvg> rasterize_svg_file(const std::filesystem::path &path, f32 target_px) {
        std::unique_ptr<lunasvg::Document> document = lunasvg::Document::loadFromFile(path.string());
        if (!document) {
            return std::nullopt;
        }

        const f32 intrinsic_width = document->width();
        const f32 intrinsic_height = document->height();
        if (intrinsic_width <= 0.0f || intrinsic_height <= 0.0f) {
            return std::nullopt;
        }
        const f32 longer_side = std::max(intrinsic_width, intrinsic_height);
        const f32 scale = target_px / longer_side;
        const u32 width = std::max<u32>(static_cast<u32>(std::lround(intrinsic_width * scale)), 1u);
        const u32 height = std::max<u32>(static_cast<u32>(std::lround(intrinsic_height * scale)), 1u);

        // Fully transparent background (0xRRGGBBAA with alpha 0) — an icon composited into a UI
        // panel must not carry an opaque backing fill.
        lunasvg::Bitmap bitmap =
            document->renderToBitmap(static_cast<int>(width), static_cast<int>(height), 0x00000000);
        if (bitmap.isNull()) {
            return std::nullopt;
        }
        // lunasvg's own pixel data is ARGB32_Premultiplied (BGRA byte order in memory on a
        // little-endian machine); convertToRGBA() does the un-premultiply + channel reorder to
        // straight-alpha RGBA in place, so no hand-rolled pixel math (and no risk of getting the
        // premultiply/byte-order conversion wrong) is needed here.
        bitmap.convertToRGBA();

        RasterizedSvg result;
        result.width = width;
        result.height = height;
        result.rgba.resize(static_cast<usize>(width) * static_cast<usize>(height) * 4);
        const u8 *src = bitmap.data();
        const usize row_bytes = static_cast<usize>(width) * 4;
        const usize stride = static_cast<usize>(bitmap.stride());
        // Copy row-by-row rather than one big memcpy: lunasvg's stride can exceed width*4 (row
        // padding), while the destination buffer is tightly packed to match
        // Engine::TextureAssetDesc::rgba8's own tightly-packed contract.
        for (u32 row = 0; row < height; ++row) {
            std::memcpy(result.rgba.data() + static_cast<usize>(row) * row_bytes,
                        src + static_cast<usize>(row) * stride, row_bytes);
        }
        return result;
    }

} // namespace SFT::UI::Svg
