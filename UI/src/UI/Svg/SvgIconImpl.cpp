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

    /// Rasterizes svg file using the supplied arguments and current state.
    ///
    /// @param path Filesystem path identifying the target resource.
    /// @param target_px `target_px` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
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


        lunasvg::Bitmap bitmap =
            document->renderToBitmap(static_cast<int>(width), static_cast<int>(height), 0x00000000);
        if (bitmap.isNull()) {
            return std::nullopt;
        }


        bitmap.convertToRGBA();

        RasterizedSvg result;
        result.width = width;
        result.height = height;
        result.rgba.resize(static_cast<usize>(width) * static_cast<usize>(height) * 4);
        const u8 *src = bitmap.data();
        const usize row_bytes = static_cast<usize>(width) * 4;
        const usize stride = static_cast<usize>(bitmap.stride());


        for (u32 row = 0; row < height; ++row) {
            std::memcpy(result.rgba.data() + static_cast<usize>(row) * row_bytes,
                        src + static_cast<usize>(row) * stride, row_bytes);
        }
        return result;
    }

} // namespace SFT::UI::Svg
