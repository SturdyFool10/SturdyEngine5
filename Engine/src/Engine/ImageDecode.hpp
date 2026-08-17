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

    /// Decodes image rgba8.
    ///
    /// @param encoded `encoded` value used by the operation.
    /// @param source Source value or resource.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] AssetExpected<DecodedImage> decode_image_rgba8(
        std::span<const std::byte> encoded,
        const std::filesystem::path &source);

} // namespace SFT::Engine::Detail

