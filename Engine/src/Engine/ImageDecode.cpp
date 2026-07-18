#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "ImageDecode.hpp"

#include <cstring>
#include <limits>
#include <string>

namespace SFT::Engine::Detail {

    AssetExpected<DecodedImage> decode_image_rgba8(
        std::span<const std::byte> encoded,
        const std::filesystem::path &source) {
        if (encoded.size() > static_cast<usize>(std::numeric_limits<int>::max())) {
            return std::unexpected(AssetError{
                .code = AssetErrorCode::DecodeFailure,
                .message = UString{"Encoded texture is too large for the image decoder."_ustr},
                .source = source,
            });
        }

        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc *decoded = stbi_load_from_memory(
            reinterpret_cast<const stbi_uc *>(encoded.data()),
            static_cast<int>(encoded.size()),
            &width,
            &height,
            &channels,
            4);
        if (decoded == nullptr || width <= 0 || height <= 0) {
            const char *reason = stbi_failure_reason();
            std::string message = "Could not decode texture '" + source.string() + "'";
            message += reason ? std::string{": "} + reason : std::string{"."};
            return std::unexpected(AssetError{
                .code = AssetErrorCode::DecodeFailure,
                .message = UString{message},
                .source = source,
            });
        }

        const usize byte_count = static_cast<usize>(width) * static_cast<usize>(height) * 4u;
        DecodedImage image{
            .width = static_cast<u32>(width),
            .height = static_cast<u32>(height),
            .pixels = std::vector<std::byte>(byte_count),
        };
        std::memcpy(image.pixels.data(), decoded, byte_count);
        stbi_image_free(decoded);
        return image;
    }

} // namespace SFT::Engine::Detail
