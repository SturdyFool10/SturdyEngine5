#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>
#pragma endregion

#include <Renderer/UI/Svg/SvgIcon.hpp>

#include <lunasvg.h>

namespace SFT::UI::Svg {

    /// Rasterizes svg file using the supplied arguments and current state.
    ///
    /// @param path Filesystem path identifying the target resource.
    /// @param target_px `target_px` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    namespace {
        /// Gives every fully transparent texel the colour of the nearest covered texel (a few texels deep), so the edge
        /// of a shape is anti-aliased by alpha alone. The rasteriser leaves transparent texels black; bilinear and mip
        /// filtering blend those into the edge and darken (or fringe) it, which reads as a jagged, dirty outline.
        void bleed_colour_into_transparent(std::vector<std::byte> &pixels, u32 width, u32 height) {
            auto *const base = reinterpret_cast<u8 *>(pixels.data());
            std::vector<u8> rgba(base, base + pixels.size());
            const auto index = [width](u32 x, u32 y) { return (static_cast<usize>(y) * width + x) * 4; };
            // 1 where the texel holds real colour: covered ones at first, then each pass adds the ring it filled.
            std::vector<u8> known(static_cast<usize>(width) * height);
            for (usize i = 0; i < known.size(); ++i) {
                known[i] = rgba[i * 4 + 3] != 0 ? 1 : 0;
            }
            constexpr u32 passes = 8;
            for (u32 pass = 0; pass < passes; ++pass) {
                std::vector<u8> next = known;
                std::vector<u8> out = rgba;
                bool changed = false;
                for (u32 y = 0; y < height; ++y) {
                    for (u32 x = 0; x < width; ++x) {
                        if (known[static_cast<usize>(y) * width + x] != 0) {
                            continue;
                        }
                        u32 sum[3]{};
                        u32 count = 0;
                        for (int dy = -1; dy <= 1; ++dy) {
                            for (int dx = -1; dx <= 1; ++dx) {
                                const int nx = static_cast<int>(x) + dx;
                                const int ny = static_cast<int>(y) + dy;
                                if (nx < 0 || ny < 0 || nx >= static_cast<int>(width) ||
                                    ny >= static_cast<int>(height)) {
                                    continue;
                                }
                                const auto n = static_cast<usize>(ny) * width + static_cast<usize>(nx);
                                if (known[n] == 0) {
                                    continue;
                                }
                                const usize at = n * 4;
                                sum[0] += rgba[at];
                                sum[1] += rgba[at + 1];
                                sum[2] += rgba[at + 2];
                                ++count;
                            }
                        }
                        if (count == 0) {
                            continue;
                        }
                        const usize at = index(x, y);
                        out[at] = static_cast<u8>(sum[0] / count);
                        out[at + 1] = static_cast<u8>(sum[1] / count);
                        out[at + 2] = static_cast<u8>(sum[2] / count);
                        next[static_cast<usize>(y) * width + x] = 1;
                        changed = true;
                    }
                }
                rgba = std::move(out);
                known = std::move(next);
                if (!changed) {
                    break;
                }
            }
            std::memcpy(base, rgba.data(), rgba.size());
        }

        std::optional<RasterizedSvg> rasterize_document(std::unique_ptr<lunasvg::Document> document, f32 target_px) {
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
            bleed_colour_into_transparent(result.rgba, width, height);
            return result;
        }
    } // namespace

    std::optional<RasterizedSvg> rasterize_svg_file(const std::filesystem::path &path, f32 target_px) {
        return rasterize_document(lunasvg::Document::loadFromFile(path.string()), target_px);
    }

    std::optional<RasterizedSvg> rasterize_svg_data(std::string_view svg, f32 target_px) {
        return rasterize_document(lunasvg::Document::loadFromData(svg.data(), svg.size()), target_px);
    }

} // namespace SFT::UI::Svg
