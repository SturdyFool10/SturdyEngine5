#pragma once

#include <Engine/Asset.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace SFT::Engine::Detail {

    /// HDR metadata read from a PNG's `cICP`/`cLLI` chunks (PNG Third Edition's HDR PNG addition).
    ///
    /// `color_primaries`/`transfer_function` are raw ITU-T H.273 code points (the same values
    /// `cICP` stores), not re-encoded into an engine-specific enum — H.273 already has stable,
    /// widely-used numbering (2 = PQ, 18 = HLG, 1 = BT.709/sRGB primaries, ...) and re-deriving an
    /// equivalent enum here would just be one more thing to keep in sync with the spec for no
    /// benefit. Present so a caller can detect real PQ/HLG HDR content; not yet consumed by the
    /// decode path itself, which still always produces 8-bit SDR pixels (see
    /// `Engine::Detail::decode_image_rgba8`'s own doc comment).
    struct PngHdrMetadata {
        bool present = false;
        u8 color_primaries = 2;   // ITU-T H.273; 2 = unspecified
        u8 transfer_function = 2; // ITU-T H.273; 2 = unspecified
        /// From `cLLI`, in cd/m^2. Zero means the chunk was absent or reported zero.
        u32 max_content_light_level = 0;
        u32 max_frame_average_light_level = 0;
    };

    struct DecodedImage {
        u32 width = 0;
        u32 height = 0;
        std::vector<std::byte> pixels;
        /// Only ever set for a PNG source; default-constructed (`present == false`) otherwise.
        PngHdrMetadata hdr;
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

