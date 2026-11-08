#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>
#pragma endregion


namespace SFT::UI::Svg {

    struct RasterizedSvg {
        u32 width = 0;
        u32 height = 0;


        std::vector<std::byte> rgba;
    };


    /// Rasterizes svg file using the supplied arguments and current state.
    ///
    /// @param path Filesystem path identifying the target resource.
    /// @param target_px `target_px` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    [[nodiscard]] std::optional<RasterizedSvg> rasterize_svg_file(const std::filesystem::path &path, f32 target_px);

    /// Rasterizes SVG source held in memory (nothing touches the filesystem).
    ///
    /// @param svg The document's text.
    /// @param target_px The longer side of the result in pixels.
    [[nodiscard]] std::optional<RasterizedSvg> rasterize_svg_data(std::string_view svg, f32 target_px);

} // namespace SFT::UI::Svg
