#pragma once

#include <Foundation/Foundation.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace SFT::Engine::Detail {


    struct TextureMipChain {
        std::vector<std::byte> data;
        u32 mip_levels = 0;
    };

    /// Returns the requested texture mip level count.
    ///
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    ///
    /// @return Returns the requested count or size.
    /// @note This function does not throw exceptions.
    [[nodiscard]] u32 texture_mip_level_count(u32 width, u32 height) noexcept;


    /// Performs the generate rgba8 mip chain operation using the supplied arguments.
    ///
    /// @param rgba8 `rgba8` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param srgb `srgb` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    [[nodiscard]] std::optional<TextureMipChain> generate_rgba8_mip_chain(
        std::span<const std::byte> rgba8, u32 width, u32 height, bool srgb);


    /// Builds the same area-averaged mip chain as `generate_rgba8_mip_chain`, for an
    /// `RGBA16Float` texture whose samples are scene-linear light.
    ///
    /// Simpler than the 8-bit case despite the wider format: that one has to decode sRGB to linear
    /// before averaging and re-encode afterwards, because averaging display-encoded values is
    /// wrong. These samples are already linear, so the average is taken directly — and nothing is
    /// clamped, so a highlight above 1.0 keeps its energy as it filters down the chain instead of
    /// being flattened at every level.
    ///
    /// @param rgba16f `rgba16f` value used by the operation, as tightly packed half-float RGBA.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    [[nodiscard]] std::optional<TextureMipChain> generate_rgba16f_mip_chain(
        std::span<const std::byte> rgba16f, u32 width, u32 height);

} // namespace SFT::Engine::Detail
