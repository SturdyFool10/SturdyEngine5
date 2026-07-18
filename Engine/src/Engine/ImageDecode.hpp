#pragma once

#include "Asset.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace SFT::Engine::Detail {

    struct DecodedImage {
        u32 width = 0;
        u32 height = 0;
        std::vector<std::byte> pixels;
    };

    [[nodiscard]] AssetExpected<DecodedImage> decode_image_rgba8(
        std::span<const std::byte> encoded,
        const std::filesystem::path &source);

} // namespace SFT::Engine::Detail

