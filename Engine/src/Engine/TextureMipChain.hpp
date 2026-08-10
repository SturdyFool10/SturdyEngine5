#pragma once

#include <Foundation/src/Foundation.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace SFT::Engine::Detail {

    // Tightly packed RGBA8 mip levels ordered from the full-resolution base image down to 1x1.
    // Each level's dimensions are max(previous / 2, 1); consumers can therefore derive every
    // offset from the base dimensions without storing a separate level table.
    struct TextureMipChain {
        std::vector<std::byte> data;
        u32 mip_levels = 0;
    };

    [[nodiscard]] u32 texture_mip_level_count(u32 width, u32 height) noexcept;

    // Builds a complete box-filtered mip chain. RGB channels are filtered in linear light for sRGB
    // textures and converted back to sRGB for storage; alpha and linear textures are averaged directly.
    [[nodiscard]] std::optional<TextureMipChain> generate_rgba8_mip_chain(
        std::span<const std::byte> rgba8, u32 width, u32 height, bool srgb);

} // namespace SFT::Engine::Detail
