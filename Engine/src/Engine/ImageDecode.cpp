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

#include <Engine/ImageDecode.hpp>

#include <Engine/HdrTransfer.hpp>

#include <webp/decode.h>
#include <webp/demux.h>

#include <avif/avif.h>

#include <jxl/decode.h>

#include <openjpeg.h>

#include <tiffio.h>

#include <ImfIO.h>
#include <ImfRgbaFile.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace SFT::Engine::Detail {

    namespace {

        /// The PNG file signature, shared by the PNG metadata scan, the APNG parser, and
        /// the ICO decoder's embedded-PNG check.
        inline constexpr std::byte kPngMagic[8] = {
            std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'},
            std::byte{'\r'}, std::byte{'\n'}, std::byte{0x1A}, std::byte{'\n'},
        };

        /// Reports whether `options` asks the decoder to hand back the source's own precision
        /// rather than 8-bit sRGB. Both `Native` and `SceneLinear` do: `SceneLinear` still has to
        /// be *given* the wide samples before it can convert them to linear working-space float,
        /// so a decoder narrowing to 8 bits first would throw away exactly what it needs.
        ///
        /// @param options `options` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool wants_wide_decode(const DecodeOptions &options) noexcept {
            return options.precision != DecodePrecision::Rgba8Srgb;
        }

        /// Reports whether `encoded` begins with either GIF signature (`GIF87a` or `GIF89a`).
        /// stb_image has no public "is this a GIF" predicate the way it does for HDR and 16-bit,
        /// and the multi-frame entry point must not be handed a non-GIF, so the two-byte version
        /// field is matched here directly.
        ///
        /// @param encoded `encoded` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool looks_like_gif(std::span<const std::byte> encoded) noexcept {
            if (encoded.size() < 6) return false;
            const auto matches = [&](const char *signature) {
                for (usize i = 0; i < 6; ++i) {
                    if (std::to_integer<char>(encoded[i]) != signature[i]) return false;
                }
                return true;
            };
            return matches("GIF87a") || matches("GIF89a");
        }

        /// Builds the "stb_image could not read this" error every stb-backed path below returns,
        /// tagged with stb's own thread-local failure reason when it left one.
        ///
        /// @param source Source value or resource, used only for error messages.
        ///
        /// @return Returns the error alternative describing why the operation failed.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::unexpected<AssetError> stb_failure(const std::filesystem::path &source) {
            const char *reason = stbi_failure_reason();
            std::string message = "Could not decode texture '" + source.string() + "'";
            message += reason != nullptr ? std::string{": "} + reason : std::string{"."};
            return std::unexpected(AssetError{
                .code = AssetErrorCode::DecodeFailure,
                .message = UString{message},
                .source = source,
            });
        }

        /// Decodes an animated GIF through stb_image's multi-frame entry point, which is separate
        /// from `stbi_load_from_memory` and is the only way to see past frame one. stb returns
        /// every frame already composited against its predecessor (it applies GIF's per-frame
        /// disposal and transparency itself), so each frame here is a complete image, not a delta.
        ///
        /// @param encoded `encoded` value used by the operation.
        /// @param source Source value or resource, used only for error messages.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<DecodedImage> decode_gif_frames(std::span<const std::byte> encoded,
                                                                   const std::filesystem::path &source) {
            int width = 0;
            int height = 0;
            int frame_count = 0;
            int channels = 0;
            int *delays = nullptr;
            stbi_uc *decoded = stbi_load_gif_from_memory(
                reinterpret_cast<const stbi_uc *>(encoded.data()),
                static_cast<int>(encoded.size()),
                &delays,
                &width,
                &height,
                &frame_count,
                &channels,
                4);
            if (decoded == nullptr || width <= 0 || height <= 0 || frame_count <= 0) {
                stbi_image_free(decoded);
                std::free(delays);
                return stb_failure(source);
            }

            const usize frame_bytes = static_cast<usize>(width) * static_cast<usize>(height) * 4u;
            DecodedImage image{
                .width = static_cast<u32>(width),
                .height = static_cast<u32>(height),
            };
            image.frames.reserve(static_cast<usize>(frame_count));
            for (int i = 0; i < frame_count; ++i) {
                ImageFrame frame{
                    .pixels = std::vector<std::byte>(frame_bytes),
                    // stb reports GIF delays in milliseconds already. A zero delay is what
                    // encoders write for "as fast as possible", which every browser clamps to
                    // 100ms to keep such files from pinning a core; matched here so a consumer
                    // driving playback off duration_ms does not spin.
                    .duration_ms = delays != nullptr && delays[i] > 0 ? static_cast<u32>(delays[i]) : 100u,
                };
                std::memcpy(frame.pixels.data(), decoded + static_cast<usize>(i) * frame_bytes, frame_bytes);
                image.frames.push_back(std::move(frame));
            }

            stbi_image_free(decoded);
            // Allocated by stb with plain malloc (not through its own allocator hooks), so it is
            // freed with plain free rather than stbi_image_free.
            std::free(delays);
            return image;
        }

        /// Decodes `encoded` through stb_image — every format stb_image itself recognizes by
        /// magic bytes (JPEG, PNG, BMP, GIF, PSD, HDR, PIC, PNM), plus the PNG-backed case of an
        /// ICO/CUR entry, which is a normal PNG at that point.
        ///
        /// Three different stb entry points are used depending on what the source actually holds:
        /// `stbi_loadf_from_memory` for Radiance `.hdr` (which is genuinely floating-point
        /// scene-linear light, and is the format Blender and every HDRI library ship
        /// environment maps in), `stbi_load_16_from_memory` for a 16-bit PNG, and the plain 8-bit
        /// path otherwise. The wide paths run only under `DecodePrecision::Native`, so the common
        /// 8-bit case never pays for a widening it is about to have narrowed straight back.
        ///
        /// @param encoded `encoded` value used by the operation.
        /// @param options `options` value used by the operation.
        /// @param source Source value or resource, used only for error messages.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<DecodedImage> decode_with_stb(std::span<const std::byte> encoded,
                                                                  const DecodeOptions &options,
                                                                  const std::filesystem::path &source) {
            if (encoded.size() > static_cast<usize>(std::numeric_limits<int>::max())) {
                return std::unexpected(AssetError{
                    .code = AssetErrorCode::DecodeFailure,
                    .message = UString{"Encoded texture is too large for the image decoder."_ustr},
                    .source = source,
                });
            }

            const auto *bytes = reinterpret_cast<const stbi_uc *>(encoded.data());
            const int size = static_cast<int>(encoded.size());

            if (options.decode_all_frames && looks_like_gif(encoded)) {
                return decode_gif_frames(encoded, source);
            }

            const bool native = wants_wide_decode(options);
            const bool is_hdr = native && stbi_is_hdr_from_memory(bytes, size) != 0;
            const bool is_16_bit = native && !is_hdr && stbi_is_16_bit_from_memory(bytes, size) != 0;

            int width = 0;
            int height = 0;
            int channels = 0;
            void *decoded = nullptr;
            if (is_hdr) {
                decoded = stbi_loadf_from_memory(bytes, size, &width, &height, &channels, 4);
            } else if (is_16_bit) {
                decoded = stbi_load_16_from_memory(bytes, size, &width, &height, &channels, 4);
            } else {
                decoded = stbi_load_from_memory(bytes, size, &width, &height, &channels, 4);
            }
            if (decoded == nullptr || width <= 0 || height <= 0) {
                stbi_image_free(decoded);
                return stb_failure(source);
            }

            DecodedImage image{
                .width = static_cast<u32>(width),
                .height = static_cast<u32>(height),
                .format = is_hdr    ? PixelFormat::Rgba32F
                          : is_16_bit ? PixelFormat::Rgba16
                                      : PixelFormat::Rgba8,
                // Radiance HDR is scene-referred linear light, so it is tagged as such (H.273
                // code 8) instead of the sRGB default; a 16-bit PNG is still sRGB-encoded, just
                // with more code values, so it keeps the default.
                .transfer_function = is_hdr ? u8{8} : u8{13},
            };
            const usize byte_count =
                static_cast<usize>(width) * static_cast<usize>(height) * bytes_per_pixel(image.format);
            image.frames.push_back(ImageFrame{.pixels = std::vector<std::byte>(byte_count)});
            std::memcpy(image.pixels().data(), decoded, byte_count);
            stbi_image_free(decoded);
            return image;
        }

        /// An `Imf::IStream` over an in-memory buffer — OpenEXR has no built-in memory-stream
        /// class of its own, only the file-based `Imf::StdIFStream`.
        class ExrMemoryStream final : public Imf::IStream {
          public:
            ExrMemoryStream(std::span<const std::byte> data, const char *filename) noexcept
                : Imf::IStream(filename), data_(data) {}

            bool read(char c[], int n) override {
                if (offset_ + static_cast<usize>(n) > data_.size()) {
                    throw std::runtime_error("EXR stream read past end of buffer");
                }
                std::memcpy(c, data_.data() + offset_, static_cast<usize>(n));
                offset_ += static_cast<usize>(n);
                return offset_ < data_.size();
            }

            uint64_t tellg() override { return offset_; }

            void seekg(uint64_t pos) override { offset_ = static_cast<usize>(pos); }

          private:
            std::span<const std::byte> data_;
            usize offset_ = 0;
        };

        /// Converts one linear light value to an 8-bit sRGB-encoded sample, clamping to [0, 1]
        /// first. EXR pixel data is scene-referred linear light (typically far outside [0, 1] for
        /// real HDR content), so decoding it straight to 8-bit without this would look either
        /// crushed-black or blown-out white depending on exposure -- this is a plain display
        /// transform, not real tone mapping (no highlight rolloff), matching the same "correct but
        /// not fancy SDR preview" scope as the PNG HDR metadata work above.
        ///
        /// @param linear `linear` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u8 linear_to_srgb_u8(float linear) noexcept {
            const float clamped = std::clamp(linear, 0.0f, 1.0f);
            const float encoded = clamped <= 0.0031308f ? clamped * 12.92f
                                                        : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
            return static_cast<u8>(std::clamp(encoded * 255.0f + 0.5f, 0.0f, 255.0f));
        }

        /// Converts a 10-bit Cineon/DPX printing-density log code (the format both were designed
        /// around, predating either scene-linear or video formats in VFX) to scene-linear light.
        /// Uses the standard default Kodak Cineon calibration (black point code 95, white point
        /// code 685, 0.002 density units per code, 0.6 print-film gamma) rather than any per-file
        /// calibration — this file carries reference black/white *density* values per channel, but
        /// not the code-to-density step size itself, so a real colorist's LUT/CDL is what actually
        /// calibrates this in a film pipeline. Verified against real encoder output: a pure red
        /// source pixel decodes to log codes (684, 95, 95) with these constants, which round-trips
        /// back to approximately full-scale red. Same "decode correctly, defer real tone-mapping
        /// pipeline integration" stance as the EXR and PNG-HDR decoders in this file.
        ///
        /// @param code10 `code10` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 cineon_log_to_linear(u32 code10) noexcept {
            constexpr float kBlackPoint = 95.0f;
            constexpr float kWhitePoint = 685.0f;
            constexpr float kGamma = 0.6f;
            constexpr float kStep = 0.002f;
            const float black_linear = std::pow(10.0f, (kBlackPoint - kWhitePoint) * kStep / kGamma);
            const float density = (static_cast<float>(code10) - kWhitePoint) * kStep / kGamma;
            const float linear = std::pow(10.0f, density);
            // Not clamped at the top: printing-density code values above the 685 white point are
            // real highlight information (that headroom is the entire point of shooting log), and
            // the native path keeps it. The 8-bit path clamps it away in linear_to_srgb_u8.
            return std::max((linear - black_linear) / (1.0f - black_linear), 0.0f);
        }

        /// Reports whether `encoded` begins with OpenEXR's magic number.
        ///
        /// @param encoded `encoded` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool looks_like_exr(std::span<const std::byte> encoded) noexcept {
            static constexpr std::byte kExrMagic[4] = {std::byte{0x76}, std::byte{0x2f}, std::byte{0x31},
                                                       std::byte{0x01}};
            return encoded.size() >= 4 && std::equal(std::begin(kExrMagic), std::end(kExrMagic), encoded.begin());
        }

        /// Decodes an OpenEXR image via the simplified RGBA API, which handles every EXR
        /// compression scheme and channel layout itself. Always reads the first (only, for a
        /// non-deep, non-multi-part image) part; multi-part/deep EXR files are rejected rather
        /// than guessed at.
        ///
        /// @param encoded `encoded` value used by the operation.
        /// @param source Source value or resource, used only for error messages.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<DecodedImage> decode_exr(std::span<const std::byte> encoded,
                                                              const std::filesystem::path &source) {
            const auto fail = [&source](const std::string &reason) {
                return std::unexpected(AssetError{
                    .code = AssetErrorCode::DecodeFailure,
                    .message = UString{"Could not decode OpenEXR: " + reason},
                    .source = source,
                });
            };

            try {
                ExrMemoryStream stream(encoded, source.string().c_str());
                Imf::RgbaInputFile file(stream);

                const Imath::Box2i &data_window = file.dataWindow();
                const i32 width = data_window.max.x - data_window.min.x + 1;
                const i32 height = data_window.max.y - data_window.min.y + 1;
                if (width <= 0 || height <= 0) {
                    return fail("reported non-positive dimensions.");
                }

                std::vector<Imf::Rgba> scanlines(static_cast<usize>(width) * static_cast<usize>(height));
                // setFrameBuffer's base is offset so pixel (dataWindow.min.x, dataWindow.min.y) --
                // the first one readPixels will actually touch -- lands at scanlines[0]; EXR data
                // windows are not required to start at (0, 0).
                Imf::Rgba *base = scanlines.data() - data_window.min.x - static_cast<isize>(data_window.min.y) * width;
                file.setFrameBuffer(base, 1, static_cast<usize>(width));
                file.readPixels(data_window.min.y, data_window.max.y);

                // Imf::Rgba is four `half`s in RGBA order, which is bit-for-bit
                // PixelFormat::Rgba16F, so the native path is a straight copy of the buffer
                // OpenEXR already filled -- no per-sample conversion at all. The linear light
                // values EXR stores are kept exactly as authored, including the above-1.0
                // highlights that made this format worth supporting; narrowing to 8-bit sRGB,
                // when a caller asks for it, is convert_to_rgba8_srgb's job now.
                static_assert(sizeof(Imf::Rgba) == 8, "Imf::Rgba must be four packed halfs to memcpy as Rgba16F.");
                const usize pixel_count = static_cast<usize>(width) * static_cast<usize>(height);
                DecodedImage result{
                    .width = static_cast<u32>(width),
                    .height = static_cast<u32>(height),
                    .format = PixelFormat::Rgba16F,
                    .color_primaries = 1, // EXR's default chromaticities are exactly BT.709's.
                    .transfer_function = 8, // Linear.
                    .frames = {ImageFrame{.pixels = std::vector<std::byte>(pixel_count * 8)}},
                };
                std::memcpy(result.pixels().data(), scanlines.data(), pixel_count * sizeof(Imf::Rgba));
                return result;
            } catch (const std::exception &e) {
                return fail(e.what());
            }
        }

        /// Reports whether `encoded` begins with a TIFF byte-order/magic-number header (`II*\0`
        /// little-endian or `MM\0*` big-endian).
        ///
        /// @param encoded `encoded` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool looks_like_tiff(std::span<const std::byte> encoded) noexcept {
            if (encoded.size() < 4) {
                return false;
            }
            const bool little_endian = encoded[0] == std::byte{'I'} && encoded[1] == std::byte{'I'} &&
                                       encoded[2] == std::byte{42} && encoded[3] == std::byte{0};
            const bool big_endian = encoded[0] == std::byte{'M'} && encoded[1] == std::byte{'M'} &&
                                    encoded[2] == std::byte{0} && encoded[3] == std::byte{42};
            return little_endian || big_endian;
        }

        /// Decodes a TIFF image via `TIFFReadRGBAImageOriented`, which handles every TIFF
        /// photometric interpretation (RGB, grayscale, palette, CMYK, YCbCr, ...) itself and always
        /// produces top-to-bottom RGBA8 — the same shape every other format in this file produces,
        /// so no separate per-photometric-interpretation handling is needed here the way JPEG 2000
        /// needed above.
        ///
        /// Scoped to what libtiff itself was built with support for (see
        /// `sturdy_fetch_libtiff()`'s own comment): uncompressed, LZW, and PackBits compression,
        /// all built into libtiff with no external dependency. A file needing a codec libtiff
        /// wasn't built with (Deflate/ZIP, JPEG-in-TIFF, JBIG, LZMA, ZSTD, WebP-in-TIFF) fails to
        /// decode cleanly rather than producing garbage.
        ///
        /// @param encoded `encoded` value used by the operation.
        /// @param source Source value or resource, used only for error messages.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<DecodedImage> decode_tiff(std::span<const std::byte> encoded,
                                                               const DecodeOptions &options,
                                                               const std::filesystem::path &source) {
            const auto fail = [&source](const char *reason) {
                return std::unexpected(AssetError{
                    .code = AssetErrorCode::DecodeFailure,
                    .message = UString{std::string{"Could not decode TIFF: "} + reason},
                    .source = source,
                });
            };

            struct MemoryStream {
                const std::byte *data;
                toff_t size;
                toff_t offset;
            } memory_stream{encoded.data(), static_cast<toff_t>(encoded.size()), 0};

            TIFF *tiff = TIFFClientOpen(
                source.string().c_str(), "r", &memory_stream,
                [](thandle_t handle, void *buffer, tmsize_t requested) -> tmsize_t {
                    auto &self = *static_cast<MemoryStream *>(handle);
                    const toff_t remaining = self.size - self.offset;
                    const tmsize_t to_read = static_cast<tmsize_t>(std::min<toff_t>(requested, remaining));
                    std::memcpy(buffer, self.data + self.offset, static_cast<usize>(to_read));
                    self.offset += static_cast<toff_t>(to_read);
                    return to_read;
                },
                [](thandle_t, void *, tmsize_t) -> tmsize_t {
                    return -1; // Writing is never valid for a read-only decode.
                },
                [](thandle_t handle, toff_t offset, int whence) -> toff_t {
                    auto &self = *static_cast<MemoryStream *>(handle);
                    toff_t base = 0;
                    if (whence == SEEK_CUR) {
                        base = self.offset;
                    } else if (whence == SEEK_END) {
                        base = self.size;
                    }
                    self.offset = std::min<toff_t>(base + offset, self.size);
                    return self.offset;
                },
                [](thandle_t) -> int {
                    return 0;
                },
                [](thandle_t handle) -> toff_t {
                    return static_cast<MemoryStream *>(handle)->size;
                },
                [](thandle_t, void **, toff_t *) -> int {
                    return 0; // No memory-mapping support; TIFFClientOpen falls back to read().
                },
                [](thandle_t, void *, toff_t) -> void {});
            if (tiff == nullptr) {
                return fail("the header is corrupt or uses an unsupported feature.");
            }

            uint32_t width = 0;
            uint32_t height = 0;
            if (TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) == 0 ||
                TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height) == 0 || width == 0 || height == 0) {
                TIFFClose(tiff);
                return fail("failed to read image dimensions.");
            }

            // TIFF is the format a Blender render or a scanner most often writes 16-bit or
            // 32-bit-float data into, and TIFFReadRGBAImageOriented always narrows to 8 bits. When
            // the caller asked for native precision and the file is in the straightforward layout
            // this path can read directly (contiguous samples, grey or RGB, no palette, no
            // subsampled YCbCr), scanlines are read raw instead so those bits survive. Anything
            // else -- palette, CMYK, YCbCr, separate planes, odd bit depths -- still goes through
            // libtiff's own universal RGBA reader below, which handles them all correctly at 8 bits.
            uint16_t bits_per_sample = 0;
            uint16_t samples_per_pixel = 0;
            uint16_t sample_format = SAMPLEFORMAT_UINT;
            uint16_t photometric = 0;
            uint16_t planar_config = PLANARCONFIG_CONTIG;
            TIFFGetFieldDefaulted(tiff, TIFFTAG_BITSPERSAMPLE, &bits_per_sample);
            TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLESPERPIXEL, &samples_per_pixel);
            TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLEFORMAT, &sample_format);
            TIFFGetFieldDefaulted(tiff, TIFFTAG_PLANARCONFIG, &planar_config);
            TIFFGetFieldDefaulted(tiff, TIFFTAG_PHOTOMETRIC, &photometric);

            const bool deep_uint = bits_per_sample == 16 && sample_format == SAMPLEFORMAT_UINT;
            const bool deep_float = bits_per_sample == 32 && sample_format == SAMPLEFORMAT_IEEEFP;
            const bool readable_layout =
                planar_config == PLANARCONFIG_CONTIG &&
                (photometric == PHOTOMETRIC_MINISBLACK || photometric == PHOTOMETRIC_RGB) &&
                (samples_per_pixel == 1 || samples_per_pixel == 3 || samples_per_pixel == 4) &&
                TIFFIsTiled(tiff) == 0;

            if (wants_wide_decode(options) && (deep_uint || deep_float) && readable_layout) {
                const PixelFormat pixel_format = deep_float ? PixelFormat::Rgba32F : PixelFormat::Rgba16;
                const usize component_bytes = deep_float ? sizeof(f32) : sizeof(u16);
                DecodedImage result{
                    .width = width,
                    .height = height,
                    .format = pixel_format,
                    // A float TIFF is scene-linear by convention (it is what Blender and every
                    // compositor write); a 16-bit integer TIFF is still sRGB-encoded, just finer.
                    .transfer_function = deep_float ? u8{8} : u8{13},
                };
                result.frames.push_back(ImageFrame{
                    .pixels = std::vector<std::byte>(static_cast<usize>(width) * height * bytes_per_pixel(pixel_format)),
                });
                std::vector<std::byte> &pixels = result.pixels();

                std::vector<std::byte> scanline(static_cast<usize>(TIFFScanlineSize(tiff)));
                const usize needed = static_cast<usize>(width) * samples_per_pixel * component_bytes;
                if (scanline.size() < needed) {
                    TIFFClose(tiff);
                    return fail("a scanline is smaller than the reported dimensions require.");
                }

                for (uint32_t y = 0; y < height; ++y) {
                    if (TIFFReadScanline(tiff, scanline.data(), y, 0) < 0) {
                        TIFFClose(tiff);
                        return fail("the pixel data is corrupt or uses an unsupported codec.");
                    }
                    for (uint32_t x = 0; x < width; ++x) {
                        const usize src = (static_cast<usize>(x) * samples_per_pixel) * component_bytes;
                        const usize dest = (static_cast<usize>(y) * width + x) * bytes_per_pixel(pixel_format);
                        // A greyscale source replicates its one sample across RGB; a source with
                        // no alpha channel gets a fully opaque one.
                        for (usize c = 0; c < 4; ++c) {
                            usize sample_index = c;
                            bool opaque_alpha = false;
                            if (samples_per_pixel == 1) {
                                sample_index = 0;
                                opaque_alpha = c == 3;
                            } else if (c == 3 && samples_per_pixel < 4) {
                                opaque_alpha = true;
                            }
                            if (opaque_alpha) {
                                if (deep_float) {
                                    const f32 one = 1.0f;
                                    std::memcpy(pixels.data() + dest + c * component_bytes, &one, sizeof(one));
                                } else {
                                    const u16 one = 65535;
                                    std::memcpy(pixels.data() + dest + c * component_bytes, &one, sizeof(one));
                                }
                                continue;
                            }
                            std::memcpy(pixels.data() + dest + c * component_bytes,
                                        scanline.data() + src + sample_index * component_bytes,
                                        component_bytes);
                        }
                    }
                }
                TIFFClose(tiff);
                return result;
            }

            DecodedImage result{
                .width = width,
                .height = height,
                .frames = {ImageFrame{.pixels = std::vector<std::byte>(static_cast<usize>(width) * height * 4)}},
            };
            std::vector<std::byte> &pixels = result.pixels();
            const int decoded_ok = TIFFReadRGBAImageOriented(
                tiff, width, height, reinterpret_cast<uint32_t *>(pixels.data()), ORIENTATION_TOPLEFT, 0);
            TIFFClose(tiff);
            if (decoded_ok == 0) {
                return fail("the pixel data is corrupt or uses an unsupported codec.");
            }
            return result;
        }

        /// Reports whether `encoded` is a JPEG 2000 file, and if so which of its two forms: a bare
        /// codestream (`.j2k`/`.jpc`, starting with the SOC marker `0xFF 0x4F`) or a JP2 file (an
        /// ISOBMFF-style container whose fixed 12-byte signature box was modeled directly on
        /// PNG's own signature).
        ///
        /// @param encoded `encoded` value used by the operation.
        ///
        /// @return The codec format to pass to `opj_create_decompress`, or `std::nullopt` when
        ///         `encoded` matches neither form.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::optional<OPJ_CODEC_FORMAT> jp2_codec_format(std::span<const std::byte> encoded) noexcept {
            if (encoded.size() >= 2 && encoded[0] == std::byte{0xFF} && encoded[1] == std::byte{0x4F}) {
                return OPJ_CODEC_J2K;
            }
            static constexpr std::byte kJp2Magic[12] = {
                std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x0C},
                std::byte{'j'},  std::byte{'P'},  std::byte{' '},  std::byte{' '},
                std::byte{0x0D}, std::byte{0x0A}, std::byte{0x87}, std::byte{0x0A},
            };
            if (encoded.size() >= 12 && std::equal(std::begin(kJp2Magic), std::end(kJp2Magic), encoded.begin())) {
                return OPJ_CODEC_JP2;
            }
            return std::nullopt;
        }

        /// Reports whether `encoded` begins with an ICO/CUR container's `ICONDIR` signature
        /// (reserved=0, type=1 for icon or 2 for cursor).
        ///
        /// @param encoded `encoded` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool looks_like_ico(std::span<const std::byte> encoded) noexcept {
            return encoded.size() >= 6 && encoded[0] == std::byte{0} && encoded[1] == std::byte{0} &&
                   (encoded[2] == std::byte{1} || encoded[2] == std::byte{2}) && encoded[3] == std::byte{0};
        }

        /// Reads a little-endian integer out of `data` at `offset`. ICO/BMP structures are
        /// defined little-endian regardless of host byte order, so these are used instead of
        /// reading through a packed struct (which would need the host to already be little-endian).
        ///
        /// @param data `data` value used by the operation.
        /// @param offset `offset` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u16 read_u16_le(std::span<const std::byte> data, usize offset) noexcept {
            return static_cast<u16>(std::to_integer<u16>(data[offset]) |
                                    static_cast<u16>(std::to_integer<u16>(data[offset + 1]) << 8));
        }

        /// @copydoc read_u16_le
        [[nodiscard]] u32 read_u32_le(std::span<const std::byte> data, usize offset) noexcept {
            return std::to_integer<u32>(data[offset]) | (std::to_integer<u32>(data[offset + 1]) << 8) |
                   (std::to_integer<u32>(data[offset + 2]) << 16) |
                   (std::to_integer<u32>(data[offset + 3]) << 24);
        }

        /// @copydoc read_u16_le
        [[nodiscard]] i32 read_i32_le(std::span<const std::byte> data, usize offset) noexcept {
            return static_cast<i32>(read_u32_le(data, offset));
        }

        /// Reads a big-endian `u32` out of `data` at `offset`. PNG chunk framing (length, and every
        /// multi-byte field inside a chunk's data) is big-endian, unlike ICO/BMP above.
        ///
        /// @param data `data` value used by the operation.
        /// @param offset `offset` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 read_u32_be(std::span<const std::byte> data, usize offset) noexcept {
            return (std::to_integer<u32>(data[offset]) << 24) | (std::to_integer<u32>(data[offset + 1]) << 16) |
                   (std::to_integer<u32>(data[offset + 2]) << 8) | std::to_integer<u32>(data[offset + 3]);
        }

        /// @copydoc read_u32_be
        [[nodiscard]] u16 read_u16_be(std::span<const std::byte> data, usize offset) noexcept {
            return static_cast<u16>((std::to_integer<u16>(data[offset]) << 8) | std::to_integer<u16>(data[offset + 1]));
        }

        /// Scans a PNG's chunk stream for the HDR PNG chunks (`cICP`, `cLLI`), stopping at the
        /// first `IDAT`/`IEND` — both are only ever meaningful before the pixel data starts, the
        /// same rule as PNG's older `gAMA`/`cHRM`/`sRGB` ancillary chunks. Not a PNG parser: it
        /// does not validate CRCs or decode anything, only walks length-prefixed chunk headers to
        /// find two specific four-byte type tags.
        ///
        /// @param encoded `encoded` value used by the operation.
        ///
        /// @return Returns the value produced by the operation; a default (`present == false`)
        ///         `PngHdrMetadata` when `encoded` is not a PNG or carries neither chunk.
        /// @note This function does not throw exceptions.
        [[nodiscard]] PngHdrMetadata scan_png_hdr_metadata(std::span<const std::byte> encoded) noexcept {
            PngHdrMetadata metadata{};
            if (encoded.size() < 8 || !std::equal(std::begin(kPngMagic), std::end(kPngMagic), encoded.begin())) {
                return metadata;
            }

            usize offset = 8;
            while (offset + 12 <= encoded.size()) {
                const u32 length = read_u32_be(encoded, offset);
                const usize type_offset = offset + 4;
                const usize data_offset = type_offset + 4;
                if (data_offset + static_cast<usize>(length) + 4 > encoded.size()) {
                    break; // Truncated; decode_with_stb's own decode surfaces the real error.
                }
                const auto is_type = [&](const char *tag) noexcept {
                    return std::to_integer<char>(encoded[type_offset]) == tag[0] &&
                           std::to_integer<char>(encoded[type_offset + 1]) == tag[1] &&
                           std::to_integer<char>(encoded[type_offset + 2]) == tag[2] &&
                           std::to_integer<char>(encoded[type_offset + 3]) == tag[3];
                };
                if (is_type("cICP") && length >= 2) {
                    metadata.present = true;
                    metadata.color_primaries = std::to_integer<u8>(encoded[data_offset]);
                    metadata.transfer_function = std::to_integer<u8>(encoded[data_offset + 1]);
                } else if (is_type("cLLI") && length >= 8) {
                    // Stored as a fixed-point value in units of 0.0001 cd/m^2.
                    metadata.max_content_light_level = read_u32_be(encoded, data_offset) / 10000;
                    metadata.max_frame_average_light_level = read_u32_be(encoded, data_offset + 4) / 10000;
                } else if (is_type("IDAT") || is_type("IEND")) {
                    break;
                }
                offset = data_offset + length + 4; // data + CRC
            }
            return metadata;
        }

        /// Reports whether `encoded` is a PNG carrying an `acTL` (animation control) chunk before
        /// its first `IDAT` — that is, an APNG rather than a still PNG. Every browser in the target
        /// set decodes APNG; stb_image ignores the animation chunks entirely and returns only the
        /// default image, so the animation path below has to parse the container itself.
        ///
        /// @param encoded `encoded` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool looks_like_apng(std::span<const std::byte> encoded) noexcept {
            if (encoded.size() < 8 || !std::equal(std::begin(kPngMagic), std::end(kPngMagic), encoded.begin())) {
                return false;
            }
            usize offset = 8;
            while (offset + 12 <= encoded.size()) {
                const u32 length = read_u32_be(encoded, offset);
                const usize type_offset = offset + 4;
                if (static_cast<u64>(type_offset) + 4 + length + 4 > encoded.size()) {
                    return false;
                }
                const auto is_type = [&](const char *tag) noexcept {
                    return std::to_integer<char>(encoded[type_offset]) == tag[0] &&
                           std::to_integer<char>(encoded[type_offset + 1]) == tag[1] &&
                           std::to_integer<char>(encoded[type_offset + 2]) == tag[2] &&
                           std::to_integer<char>(encoded[type_offset + 3]) == tag[3];
                };
                if (is_type("acTL")) {
                    return true;
                }
                // acTL is required to precede IDAT, so reaching the image data means this is an
                // ordinary PNG and there is no point scanning the (potentially large) rest.
                if (is_type("IDAT") || is_type("IEND")) {
                    return false;
                }
                offset = type_offset + 4 + length + 4;
            }
            return false;
        }

        /// Appends a complete PNG chunk (length, type, data, CRC) to `out`.
        ///
        /// @param out `out` value used by the operation.
        /// @param type Four-character chunk type.
        /// @param data `data` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void append_png_chunk(std::vector<std::byte> &out, const char *type, std::span<const std::byte> data) {
            const auto push_u32_be = [&out](u32 value) {
                out.push_back(static_cast<std::byte>((value >> 24) & 0xFFu));
                out.push_back(static_cast<std::byte>((value >> 16) & 0xFFu));
                out.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
                out.push_back(static_cast<std::byte>(value & 0xFFu));
            };
            push_u32_be(static_cast<u32>(data.size()));
            const usize crc_start = out.size();
            for (usize i = 0; i < 4; ++i) {
                out.push_back(static_cast<std::byte>(type[i]));
            }
            out.insert(out.end(), data.begin(), data.end());

            // PNG's CRC-32 covers the type and data but not the length. Computed here bitwise
            // rather than through a table: this runs once per assembled frame, not per byte of a
            // hot loop, and a table would have to be initialized and kept somewhere.
            u32 crc = 0xFFFFFFFFu;
            for (usize i = crc_start; i < out.size(); ++i) {
                crc ^= std::to_integer<u32>(out[i]);
                for (int bit = 0; bit < 8; ++bit) {
                    crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
                }
            }
            push_u32_be(crc ^ 0xFFFFFFFFu);
        }

        /// Decodes an APNG (animated PNG) into its full sequence of composited frames.
        ///
        /// APNG stores each frame as raw zlib-compressed image data in `fdAT` chunks, with no PNG
        /// wrapper of its own — so each frame is decoded by *rebuilding* a standalone single-frame
        /// PNG around it: the original `IHDR` with the frame's own width/height patched in, every
        /// ancillary chunk that affects pixel interpretation (palette, transparency, gamma) copied
        /// across, the frame's data re-tagged as `IDAT`, and an `IEND`. stb_image then decodes
        /// that ordinary PNG. This is far less code than a second PNG decoder and cannot disagree
        /// with the still path about how a PNG decodes, because it *is* the still path.
        ///
        /// Frames are then composited onto a persistent canvas following each frame's own dispose
        /// and blend operations, so every frame handed back is a complete image.
        ///
        /// @param encoded `encoded` value used by the operation.
        /// @param options `options` value used by the operation.
        /// @param source Source value or resource, used only for error messages.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<DecodedImage> decode_apng(std::span<const std::byte> encoded,
                                                              const DecodeOptions &options,
                                                              const std::filesystem::path &source) {
            const auto fail = [&source](const char *reason) {
                return std::unexpected(AssetError{
                    .code = AssetErrorCode::DecodeFailure,
                    .message = UString{std::string{"Could not decode APNG: "} + reason},
                    .source = source,
                });
            };

            /// One frame's `fcTL` control data plus the concatenated payload of its `fdAT`/`IDAT`
            /// chunks (a frame's data may be split across any number of them).
            struct ApngFrame {
                u32 width = 0;
                u32 height = 0;
                u32 x_offset = 0;
                u32 y_offset = 0;
                u32 duration_ms = 0;
                u8 dispose_op = 0; // 0 = none, 1 = background, 2 = previous.
                u8 blend_op = 0;   // 0 = source (replace), 1 = over (alpha blend).
                std::vector<std::byte> data;
            };

            std::span<const std::byte> ihdr;
            std::vector<std::byte> shared_chunks; // Ancillary chunks every frame's PNG needs.
            std::vector<ApngFrame> frames;
            u32 canvas_width = 0;
            u32 canvas_height = 0;
            u32 loop_count = 0;
            bool default_image_is_first_frame = false;
            bool seen_idat = false;

            usize offset = 8;
            while (offset + 12 <= encoded.size()) {
                const u32 length = read_u32_be(encoded, offset);
                const usize type_offset = offset + 4;
                const usize data_offset = type_offset + 4;
                if (static_cast<u64>(data_offset) + length + 4 > encoded.size()) {
                    return fail("a chunk runs past the end of the file.");
                }
                const auto is_type = [&](const char *tag) noexcept {
                    return std::to_integer<char>(encoded[type_offset]) == tag[0] &&
                           std::to_integer<char>(encoded[type_offset + 1]) == tag[1] &&
                           std::to_integer<char>(encoded[type_offset + 2]) == tag[2] &&
                           std::to_integer<char>(encoded[type_offset + 3]) == tag[3];
                };
                const std::span<const std::byte> data = encoded.subspan(data_offset, length);

                if (is_type("IHDR")) {
                    if (length < 13) {
                        return fail("the IHDR chunk is truncated.");
                    }
                    ihdr = data;
                    canvas_width = read_u32_be(encoded, data_offset);
                    canvas_height = read_u32_be(encoded, data_offset + 4);
                } else if (is_type("acTL")) {
                    if (length >= 8) {
                        loop_count = read_u32_be(encoded, data_offset + 4);
                    }
                } else if (is_type("fcTL")) {
                    if (length < 26) {
                        return fail("an fcTL chunk is truncated.");
                    }
                    ApngFrame frame;
                    frame.width = read_u32_be(encoded, data_offset + 4);
                    frame.height = read_u32_be(encoded, data_offset + 8);
                    frame.x_offset = read_u32_be(encoded, data_offset + 12);
                    frame.y_offset = read_u32_be(encoded, data_offset + 16);
                    const u16 delay_numerator = read_u16_be(encoded, data_offset + 20);
                    const u16 delay_denominator = read_u16_be(encoded, data_offset + 22);
                    // A zero denominator means 100, per the APNG spec; a zero delay means "as fast
                    // as possible", which browsers clamp to 100ms, matched here so a consumer
                    // driving playback off duration_ms does not spin.
                    const u32 denominator = delay_denominator == 0 ? 100u : delay_denominator;
                    frame.duration_ms = delay_numerator == 0
                                            ? 100u
                                            : static_cast<u32>(delay_numerator * 1000u / denominator);
                    frame.dispose_op = std::to_integer<u8>(encoded[data_offset + 24]);
                    frame.blend_op = std::to_integer<u8>(encoded[data_offset + 25]);
                    if (frame.width == 0 || frame.height == 0 ||
                        static_cast<u64>(frame.x_offset) + frame.width > canvas_width ||
                        static_cast<u64>(frame.y_offset) + frame.height > canvas_height) {
                        return fail("a frame's rectangle falls outside the canvas.");
                    }
                    // An fcTL appearing before the first IDAT marks the default image as frame
                    // one; an APNG whose first fcTL comes *after* IDAT is instead using its
                    // default image purely as a still fallback for decoders that ignore the
                    // animation chunks, and that image is not part of the animation at all.
                    if (frames.empty()) {
                        default_image_is_first_frame = !seen_idat;
                    }
                    frames.push_back(std::move(frame));
                } else if (is_type("IDAT")) {
                    seen_idat = true;
                    if (default_image_is_first_frame && !frames.empty()) {
                        frames.front().data.insert(frames.front().data.end(), data.begin(), data.end());
                    }
                } else if (is_type("fdAT")) {
                    if (length < 4) {
                        return fail("an fdAT chunk is truncated.");
                    }
                    if (frames.empty()) {
                        return fail("an fdAT chunk appears before any fcTL chunk.");
                    }
                    // The first four bytes are the sequence number, not image data.
                    std::vector<std::byte> &target = frames.back().data;
                    target.insert(target.end(), data.begin() + 4, data.end());
                } else if (is_type("PLTE") || is_type("tRNS") || is_type("gAMA") || is_type("cHRM") ||
                           is_type("sRGB") || is_type("iCCP") || is_type("sBIT") || is_type("cICP")) {
                    // Chunks that change how the sample values are interpreted; each rebuilt
                    // single-frame PNG needs them or it decodes to different colors than the
                    // still path would produce for the same bytes.
                    const char type[5] = {std::to_integer<char>(encoded[type_offset]),
                                          std::to_integer<char>(encoded[type_offset + 1]),
                                          std::to_integer<char>(encoded[type_offset + 2]),
                                          std::to_integer<char>(encoded[type_offset + 3]), '\0'};
                    append_png_chunk(shared_chunks, type, data);
                } else if (is_type("IEND")) {
                    break;
                }
                offset = data_offset + length + 4;
            }

            if (ihdr.empty() || canvas_width == 0 || canvas_height == 0) {
                return fail("no usable IHDR chunk.");
            }
            if (frames.empty()) {
                return fail("no animation frames.");
            }
            if (!options.decode_all_frames) {
                frames.resize(1);
            }

            const usize canvas_bytes = static_cast<usize>(canvas_width) * canvas_height * 4;
            DecodedImage image{
                .width = canvas_width,
                .height = canvas_height,
            };
            image.loop_count = loop_count;
            image.frames.reserve(frames.size());

            std::vector<std::byte> canvas(canvas_bytes, std::byte{0});
            std::vector<std::byte> previous_canvas;

            for (const ApngFrame &frame : frames) {
                if (frame.data.empty()) {
                    return fail("a frame carries no image data.");
                }

                // Rebuild a standalone PNG around this frame's data (see this function's own doc
                // comment). Only IHDR's width and height differ from the original's; every other
                // field (bit depth, color type, interlace) must be carried over unchanged or the
                // frame data would be interpreted differently than it was compressed.
                std::vector<std::byte> frame_png;
                frame_png.insert(frame_png.end(), std::begin(kPngMagic), std::end(kPngMagic));
                std::array<std::byte, 13> frame_ihdr{};
                std::copy_n(ihdr.begin(), 13, frame_ihdr.begin());
                const auto write_u32_be = [&frame_ihdr](usize at, u32 value) {
                    frame_ihdr[at + 0] = static_cast<std::byte>((value >> 24) & 0xFFu);
                    frame_ihdr[at + 1] = static_cast<std::byte>((value >> 16) & 0xFFu);
                    frame_ihdr[at + 2] = static_cast<std::byte>((value >> 8) & 0xFFu);
                    frame_ihdr[at + 3] = static_cast<std::byte>(value & 0xFFu);
                };
                write_u32_be(0, frame.width);
                write_u32_be(4, frame.height);
                append_png_chunk(frame_png, "IHDR", frame_ihdr);
                frame_png.insert(frame_png.end(), shared_chunks.begin(), shared_chunks.end());
                append_png_chunk(frame_png, "IDAT", frame.data);
                append_png_chunk(frame_png, "IEND", {});

                const AssetExpected<DecodedImage> decoded =
                    decode_with_stb(frame_png, DecodeOptions{}, source);
                if (!decoded) {
                    return fail("a frame's image data is corrupt.");
                }
                if (decoded->width != frame.width || decoded->height != frame.height) {
                    return fail("a frame decoded to unexpected dimensions.");
                }
                const std::vector<std::byte> &sub = decoded->pixels();

                if (frame.dispose_op == 2) {
                    // "Previous": the canvas must be restored after this frame, so snapshot it
                    // before compositing rather than after.
                    previous_canvas = canvas;
                }

                for (u32 y = 0; y < frame.height; ++y) {
                    for (u32 x = 0; x < frame.width; ++x) {
                        const usize src = (static_cast<usize>(y) * frame.width + x) * 4;
                        const usize dest =
                            ((static_cast<usize>(y) + frame.y_offset) * canvas_width + x + frame.x_offset) * 4;
                        if (frame.blend_op == 0) {
                            // APNG_BLEND_OP_SOURCE: overwrite, alpha included.
                            std::memcpy(canvas.data() + dest, sub.data() + src, 4);
                            continue;
                        }
                        // APNG_BLEND_OP_OVER: standard non-premultiplied source-over compositing.
                        const f32 src_alpha = static_cast<f32>(std::to_integer<u8>(sub[src + 3])) / 255.0f;
                        const f32 dest_alpha = static_cast<f32>(std::to_integer<u8>(canvas[dest + 3])) / 255.0f;
                        const f32 out_alpha = src_alpha + dest_alpha * (1.0f - src_alpha);
                        for (usize c = 0; c < 3; ++c) {
                            const f32 src_color = static_cast<f32>(std::to_integer<u8>(sub[src + c]));
                            const f32 dest_color = static_cast<f32>(std::to_integer<u8>(canvas[dest + c]));
                            const f32 blended =
                                out_alpha <= 0.0f
                                    ? 0.0f
                                    : (src_color * src_alpha + dest_color * dest_alpha * (1.0f - src_alpha)) /
                                          out_alpha;
                            canvas[dest + c] = std::byte{static_cast<u8>(std::clamp(blended + 0.5f, 0.0f, 255.0f))};
                        }
                        canvas[dest + 3] = std::byte{static_cast<u8>(std::clamp(out_alpha * 255.0f + 0.5f, 0.0f, 255.0f))};
                    }
                }

                image.frames.push_back(ImageFrame{.pixels = canvas, .duration_ms = frame.duration_ms});

                if (frame.dispose_op == 1) {
                    // "Background": clear this frame's rectangle to fully transparent black.
                    for (u32 y = 0; y < frame.height; ++y) {
                        const usize dest =
                            ((static_cast<usize>(y) + frame.y_offset) * canvas_width + frame.x_offset) * 4;
                        std::memset(canvas.data() + dest, 0, static_cast<usize>(frame.width) * 4);
                    }
                } else if (frame.dispose_op == 2) {
                    canvas = previous_canvas;
                }
            }

            if (image.frames.size() == 1) {
                image.frames.front().duration_ms = 0;
            }
            return image;
        }

        /// Reports whether `encoded` begins with a WebP container's RIFF/WEBP signature.
        ///
        /// @param encoded `encoded` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool looks_like_webp(std::span<const std::byte> encoded) noexcept {
            if (encoded.size() < 12) {
                return false;
            }
            const auto matches = [&encoded](usize offset, const char *tag) noexcept {
                return std::to_integer<char>(encoded[offset]) == tag[0] &&
                       std::to_integer<char>(encoded[offset + 1]) == tag[1] &&
                       std::to_integer<char>(encoded[offset + 2]) == tag[2] &&
                       std::to_integer<char>(encoded[offset + 3]) == tag[3];
            };
            return matches(0, "RIFF") && matches(8, "WEBP");
        }

        /// Decodes a WebP image.
        ///
        /// The simple decode API (`WebPDecodeRGBA`) handles every still-image case, including the
        /// extended-format (VP8X) container with alpha/ICC/EXIF — the one case it cannot handle is
        /// an animated WebP, whose payload is a sequence of `ANMF` frame chunks rather than a
        /// directly decodable bitstream. That case falls back to the demux API and decodes frame 1
        /// only, matching how a static `<img>` reference to an animated image shows its first frame.
        ///
        /// @param encoded `encoded` value used by the operation.
        /// @param source Source value or resource, used only for error messages.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<DecodedImage> decode_webp(std::span<const std::byte> encoded,
                                                               const DecodeOptions &options,
                                                               const std::filesystem::path &source) {
            const auto fail = [&source](const char *reason) {
                return std::unexpected(AssetError{
                    .code = AssetErrorCode::DecodeFailure,
                    .message = UString{std::string{"Could not decode WebP: "} + reason},
                    .source = source,
                });
            };

            const auto *bytes = reinterpret_cast<const uint8_t *>(encoded.data());
            const WebPData data{bytes, encoded.size()};

            // Animated WebP is a container of sub-frames, each of which may be a partial rectangle
            // composited onto the canvas with its own blend and dispose method. WebPAnimDecoder
            // does all of that reconstruction and hands back complete canvases, which is why the
            // animation path uses it rather than iterating raw fragments through WebPDemux.
            if (options.decode_all_frames) {
                WebPAnimDecoderOptions decoder_options;
                if (WebPAnimDecoderOptionsInit(&decoder_options) == 0) {
                    return fail("failed to initialize the animation decoder options.");
                }
                decoder_options.color_mode = MODE_RGBA;
                decoder_options.use_threads = 1;

                const std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)> decoder{
                    WebPAnimDecoderNew(&data, &decoder_options), &WebPAnimDecoderDelete};
                if (decoder == nullptr) {
                    return fail("not a recognizable WebP container.");
                }

                WebPAnimInfo info;
                if (WebPAnimDecoderGetInfo(decoder.get(), &info) == 0) {
                    return fail("could not read the animation header.");
                }
                if (info.canvas_width == 0 || info.canvas_height == 0) {
                    return fail("reported a zero-sized canvas.");
                }

                DecodedImage image{
                    .width = info.canvas_width,
                    .height = info.canvas_height,
                };
                image.loop_count = info.loop_count;
                image.frames.reserve(info.frame_count);

                const usize frame_bytes = static_cast<usize>(info.canvas_width) * info.canvas_height * 4u;
                // WebP frame timestamps are cumulative end-times in milliseconds, not per-frame
                // durations, so each frame's duration is the delta from the previous one.
                int previous_timestamp = 0;
                while (WebPAnimDecoderHasMoreFrames(decoder.get()) != 0) {
                    uint8_t *frame_pixels = nullptr;
                    int timestamp = 0;
                    if (WebPAnimDecoderGetNext(decoder.get(), &frame_pixels, &timestamp) == 0) {
                        return fail("a frame failed to decode.");
                    }
                    ImageFrame frame{
                        .pixels = std::vector<std::byte>(frame_bytes),
                        .duration_ms = static_cast<u32>(std::max(timestamp - previous_timestamp, 0)),
                    };
                    // The buffer WebPAnimDecoderGetNext returns belongs to the decoder and is
                    // overwritten by the next call, so each frame is copied out immediately.
                    std::memcpy(frame.pixels.data(), frame_pixels, frame_bytes);
                    image.frames.push_back(std::move(frame));
                    previous_timestamp = timestamp;
                }
                if (image.frames.empty()) {
                    return fail("container has no decodable frame.");
                }
                return image;
            }

            int width = 0;
            int height = 0;
            uint8_t *decoded = WebPDecodeRGBA(bytes, encoded.size(), &width, &height);

            std::vector<std::byte> frame_bytes; // keeps the fallback frame alive across the decode below
            if (decoded == nullptr) {
                // A still decode of an animated file fails, because the payload is a container
                // rather than a bare VP8/VP8L chunk; pulling frame one out through the demuxer is
                // what makes an animation still load as its own first frame.
                WebPDemuxer *demux = WebPDemux(&data);
                if (demux == nullptr) {
                    return fail("not a recognizable WebP container.");
                }
                WebPIterator iter{};
                const int got_frame = WebPDemuxGetFrame(demux, 1, &iter);
                if (got_frame != 0) {
                    const auto *fragment_bytes = reinterpret_cast<const std::byte *>(iter.fragment.bytes);
                    frame_bytes.assign(fragment_bytes, fragment_bytes + iter.fragment.size);
                    WebPDemuxReleaseIterator(&iter);
                }
                WebPDemuxDelete(demux);
                if (frame_bytes.empty()) {
                    return fail("container has no decodable frame.");
                }
                decoded = WebPDecodeRGBA(reinterpret_cast<const uint8_t *>(frame_bytes.data()),
                                        frame_bytes.size(), &width, &height);
                if (decoded == nullptr) {
                    return fail("frame data is corrupt.");
                }
            }

            if (width <= 0 || height <= 0) {
                WebPFree(decoded);
                return fail("reported non-positive dimensions.");
            }

            const usize byte_count = static_cast<usize>(width) * static_cast<usize>(height) * 4u;
            DecodedImage image{
                .width = static_cast<u32>(width),
                .height = static_cast<u32>(height),
                .frames = {ImageFrame{.pixels = std::vector<std::byte>(byte_count)}},
            };
            std::memcpy(image.pixels().data(), decoded, byte_count);
            WebPFree(decoded);
            return image;
        }

        /// Frees an `avifRGBImage`'s pixel buffer on scope exit. `avifRGBImage` is a plain stack
        /// struct with a separately allocated pixel buffer, so it fits neither unique_ptr nor any
        /// of libavif's own create/destroy pairs.
        struct AvifRgbPixelsGuard {
            avifRGBImage *image;
            explicit AvifRgbPixelsGuard(avifRGBImage *rgb) noexcept : image(rgb) {}
            ~AvifRgbPixelsGuard() { avifRGBImageFreePixels(image); }
            AvifRgbPixelsGuard(const AvifRgbPixelsGuard &) = delete;
            AvifRgbPixelsGuard &operator=(const AvifRgbPixelsGuard &) = delete;
        };

        /// Reports whether `encoded` is an ISOBMFF file whose `ftyp` box names `avif` or `avis`
        /// (a still AVIF image or an AVIF image sequence) as its major brand or among its
        /// compatible brands. Not a full ISOBMFF parser — just enough of one to route correctly;
        /// libavif's own parser does the real, robust work in `decode_avif`.
        ///
        /// @param encoded `encoded` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool looks_like_avif(std::span<const std::byte> encoded) noexcept {
            if (encoded.size() < 16) {
                return false;
            }
            const auto tag_at = [&encoded](usize offset, const char *tag) noexcept {
                return std::to_integer<char>(encoded[offset]) == tag[0] &&
                       std::to_integer<char>(encoded[offset + 1]) == tag[1] &&
                       std::to_integer<char>(encoded[offset + 2]) == tag[2] &&
                       std::to_integer<char>(encoded[offset + 3]) == tag[3];
            };
            if (!tag_at(4, "ftyp")) {
                return false;
            }
            const u32 box_size = read_u32_be(encoded, 0);
            const usize brands_end = std::min(static_cast<usize>(box_size), encoded.size());
            // Major brand at [8,12), minor version at [12,16), then compatible brands in 4-byte
            // groups to the end of the box.
            for (usize offset = 8; offset + 4 <= brands_end; offset += 4) {
                if (tag_at(offset, "avif") || tag_at(offset, "avis")) {
                    return true;
                }
            }
            return false;
        }

        /// Decodes a still AVIF image (the first frame, for an AVIF image sequence).
        ///
        /// @param encoded `encoded` value used by the operation.
        /// @param source Source value or resource, used only for error messages.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<DecodedImage> decode_avif(std::span<const std::byte> encoded,
                                                               const DecodeOptions &options,
                                                               const std::filesystem::path &source) {
            const auto fail = [&source](const char *reason) {
                return std::unexpected(AssetError{
                    .code = AssetErrorCode::DecodeFailure,
                    .message = UString{std::string{"Could not decode AVIF: "} + reason},
                    .source = source,
                });
            };

            avifDecoder *decoder = avifDecoderCreate();
            if (decoder == nullptr) {
                return fail("failed to create the AVIF decoder.");
            }
            // Owned through a unique_ptr rather than destroyed at each early return: the frame
            // loop below has enough exit paths that hand-placed avifDecoderDestroy calls were
            // becoming the most likely place for a leak to hide.
            const std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)> decoder_owner{
                decoder, &avifDecoderDestroy};

            avifResult result = avifDecoderSetIOMemory(
                decoder, reinterpret_cast<const uint8_t *>(encoded.data()), encoded.size());
            if (result == AVIF_RESULT_OK) {
                result = avifDecoderParse(decoder);
            }
            if (result != AVIF_RESULT_OK) {
                const char *reason = avifResultToString(result);
                return fail(reason != nullptr ? reason : "unknown decode failure.");
            }

            // AVIF carries real HDR: 10- and 12-bit AV1 with PQ or HLG transfer characteristics is
            // the format's headline feature and the whole reason browsers ship it for HDR photos.
            // Decoding those to 8 bits would throw away exactly the data this decoder exists to
            // read, so the native path asks libavif for a 16-bit RGB buffer whenever the source is
            // deeper than 8-bit and reports the source's own cICP values upward.
            const bool wide = wants_wide_decode(options) && decoder->image->depth > 8;
            const u32 rgb_depth = wide ? 16u : 8u;

            DecodedImage image;
            image.format = wide ? PixelFormat::Rgba16 : PixelFormat::Rgba8;
            if (wants_wide_decode(options)) {
                image.color_primaries = static_cast<u8>(decoder->image->colorPrimaries);
                image.transfer_function = static_cast<u8>(decoder->image->transferCharacteristics);
            }
            image.loop_count = 0; // AVIF sequences carry no repeat count; treated as looping.

            // avifDecoderNextImage walks the image sequence; for a still AVIF it succeeds exactly
            // once and then reports AVIF_RESULT_NO_IMAGES_REMAINING, so the same loop covers both
            // without a separate still-image path.
            while ((result = avifDecoderNextImage(decoder)) == AVIF_RESULT_OK) {
                avifRGBImage rgb;
                avifRGBImageSetDefaults(&rgb, decoder->image);
                rgb.format = AVIF_RGB_FORMAT_RGBA;
                rgb.depth = rgb_depth;
                if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK) {
                    return fail("failed to allocate the RGBA conversion buffer.");
                }
                const AvifRgbPixelsGuard free_rgb{&rgb};

                const avifResult convert = avifImageYUVToRGB(decoder->image, &rgb);
                if (convert != AVIF_RESULT_OK) {
                    const char *reason = avifResultToString(convert);
                    return fail(reason != nullptr ? reason : "YUV-to-RGB conversion failed.");
                }

                if (image.frames.empty()) {
                    image.width = rgb.width;
                    image.height = rgb.height;
                } else if (rgb.width != image.width || rgb.height != image.height) {
                    // Every frame of an AVIF sequence shares the track's dimensions; a file that
                    // disagrees is malformed, and silently keeping the first frame's size would
                    // hand callers a buffer whose length does not match width * height.
                    return fail("image sequence frames disagree on dimensions.");
                }

                // rgb.rowBytes may exceed the tightly packed row length (padding
                // avifRGBImageAllocatePixels can add), so rows are copied one at a time.
                const usize dest_row_bytes = static_cast<usize>(rgb.width) * bytes_per_pixel(image.format);
                ImageFrame frame{
                    .pixels = std::vector<std::byte>(dest_row_bytes * rgb.height),
                    .duration_ms = static_cast<u32>(decoder->imageTiming.duration * 1000.0 + 0.5),
                };
                for (u32 y = 0; y < rgb.height; ++y) {
                    std::memcpy(frame.pixels.data() + static_cast<usize>(y) * dest_row_bytes,
                                rgb.pixels + static_cast<usize>(y) * rgb.rowBytes, dest_row_bytes);
                }
                image.frames.push_back(std::move(frame));

                if (!options.decode_all_frames) {
                    break;
                }
            }

            if (image.frames.empty()) {
                const char *reason = avifResultToString(result);
                return fail(reason != nullptr ? reason : "file contains no decodable image.");
            }
            if (image.frames.size() == 1) {
                // A single frame is a still image (or a caller that asked for only the first one):
                // a duration on it would make an animation-aware consumer wait on nothing.
                image.frames.front().duration_ms = 0;
            }
            return image;
        }

        /// Reports whether `encoded` is a JPEG XL codestream or container, via libjxl's own
        /// signature check rather than hand-matching magic bytes — JXL has two valid forms (a bare
        /// codestream, and one boxed in an ISOBMFF-style container) and the library already knows
        /// how to recognize both.
        ///
        /// @param encoded `encoded` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool looks_like_jxl(std::span<const std::byte> encoded) noexcept {
            const JxlSignature signature =
                JxlSignatureCheck(reinterpret_cast<const uint8_t *>(encoded.data()), encoded.size());
            return signature == JXL_SIG_CODESTREAM || signature == JXL_SIG_CONTAINER;
        }

        /// Decodes a JPEG XL image.
        ///
        /// One-shot decode: the whole encoded buffer is handed over up front and
        /// `JxlDecoderCloseInput` immediately marks it as everything there is, so
        /// `JXL_DEC_NEED_MORE_INPUT` never legitimately occurs here — the caller already loaded the
        /// complete file, unlike a streaming decoder reading off a socket.
        ///
        /// Covers all three of JXL's headline capabilities rather than just its still 8-bit case:
        /// float pixel output for its wide-gamut/HDR modes (JXL is the only format here that can
        /// carry both a PQ transfer curve *and* more than 12 bits), animation, and progressive
        /// decoding — where `DecodeOptions::preview_min_dimension` stops at the DC frame, an
        /// eighth-scale approximation JXL encodes up front precisely so a viewer can show
        /// something before the rest of the file has even been read.
        ///
        /// @param encoded `encoded` value used by the operation.
        /// @param options `options` value used by the operation.
        /// @param source Source value or resource, used only for error messages.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<DecodedImage> decode_jxl(std::span<const std::byte> encoded,
                                                              const DecodeOptions &options,
                                                              const std::filesystem::path &source) {
            const auto fail = [&source](const char *reason) {
                return std::unexpected(AssetError{
                    .code = AssetErrorCode::DecodeFailure,
                    .message = UString{std::string{"Could not decode JPEG XL: "} + reason},
                    .source = source,
                });
            };

            const std::unique_ptr<JxlDecoder, decltype(&JxlDecoderDestroy)> decoder{
                JxlDecoderCreate(nullptr), &JxlDecoderDestroy};
            if (decoder == nullptr) {
                return fail("failed to create the JPEG XL decoder.");
            }

            const bool native = wants_wide_decode(options);
            const bool want_preview = options.preview_min_dimension > 0;

            // JXL_DEC_NEED_IMAGE_OUT_BUFFER is a JxlDecoderProcessInput status code (a small
            // sequential value, 5), not an "informative event" bitmask like the others (values
            // >= 0x40) -- it is not subscribable, and OR-ing its bit pattern in here corrupts the
            // mask and makes JxlDecoderSubscribeEvents itself fail. It is still reported
            // automatically by JxlDecoderProcessInput at the right point regardless of subscription.
            int events = JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE;
            if (want_preview) {
                events |= JXL_DEC_FRAME_PROGRESSION;
            }
            if (JxlDecoderSubscribeEvents(decoder.get(), events) != JXL_DEC_SUCCESS) {
                return fail("failed to subscribe to decode events.");
            }
            if (want_preview) {
                // Stop as soon as the DC frame is available. JxlDecoderFlushImage then yields that
                // low-resolution approximation without the rest of the codestream ever being
                // decoded.
                if (JxlDecoderSetProgressiveDetail(decoder.get(), kDC) != JXL_DEC_SUCCESS) {
                    return fail("failed to enable progressive decoding.");
                }
            }
            if (JxlDecoderSetInput(decoder.get(), reinterpret_cast<const uint8_t *>(encoded.data()),
                                   encoded.size()) != JXL_DEC_SUCCESS) {
                return fail("failed to set decode input.");
            }
            JxlDecoderCloseInput(decoder.get());

            JxlBasicInfo info{};
            // Filled in once the basic info arrives: JXL's own float mode is used whenever the
            // source is deeper than 8 bits or is genuinely floating-point, since anything less
            // would quantize away exactly what makes the file worth decoding natively.
            JxlPixelFormat format{
                .num_channels = 4,
                .data_type = JXL_TYPE_UINT8,
                .endianness = JXL_NATIVE_ENDIAN,
                .align = 0,
            };

            DecodedImage image;
            std::vector<std::byte> pixels;
            bool flushed_preview = false;

            for (;;) {
                const JxlDecoderStatus status = JxlDecoderProcessInput(decoder.get());
                if (status == JXL_DEC_ERROR) {
                    return fail("the bitstream is corrupt or uses an unsupported feature.");
                }
                if (status == JXL_DEC_NEED_MORE_INPUT) {
                    // Cannot legitimately happen for a one-shot decode of a complete buffer (see
                    // this function's own doc comment) -- treated as a decode failure rather than
                    // looping forever.
                    return fail("the file is truncated.");
                }
                if (status == JXL_DEC_BASIC_INFO) {
                    if (JxlDecoderGetBasicInfo(decoder.get(), &info) != JXL_DEC_SUCCESS) {
                        return fail("failed to read image dimensions.");
                    }
                    if (info.xsize == 0 || info.ysize == 0) {
                        return fail("reported zero dimensions.");
                    }
                    const bool deep = info.bits_per_sample > 8 || info.exponent_bits_per_sample > 0;
                    if (native && deep) {
                        format.data_type = JXL_TYPE_FLOAT;
                        image.format = PixelFormat::Rgba32F;
                    }
                    image.width = info.xsize;
                    image.height = info.ysize;
                    image.loop_count = info.have_animation != 0 ? info.animation.num_loops : 0u;
                    continue;
                }
                if (status == JXL_DEC_COLOR_ENCODING) {
                    JxlColorEncoding color{};
                    if (native && JxlDecoderGetColorAsEncodedProfile(
                                      decoder.get(), JXL_COLOR_PROFILE_TARGET_DATA, &color) == JXL_DEC_SUCCESS) {
                        // libjxl's enums are deliberately numbered as the H.273 code points they
                        // represent, so these are direct casts rather than a translation table.
                        image.color_primaries = static_cast<u8>(color.primaries);
                        image.transfer_function = static_cast<u8>(color.transfer_function);
                    }
                    continue;
                }
                if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                    size_t buffer_size = 0;
                    if (JxlDecoderImageOutBufferSize(decoder.get(), &format, &buffer_size) != JXL_DEC_SUCCESS) {
                        return fail("failed to determine the output buffer size.");
                    }
                    pixels.assign(buffer_size, std::byte{0});
                    if (JxlDecoderSetImageOutBuffer(decoder.get(), &format, pixels.data(), pixels.size()) !=
                        JXL_DEC_SUCCESS) {
                        return fail("failed to set the output buffer.");
                    }
                    continue;
                }
                if (status == JXL_DEC_FRAME_PROGRESSION) {
                    // The DC frame is ready. JxlDecoderFlushImage writes the approximation
                    // upscaled to full size into the output buffer, so the result has the image's
                    // real dimensions but only DC-level detail -- still exactly the "show
                    // something now" pass the preview hint asks for, obtained without decoding the
                    // remaining passes.
                    if (JxlDecoderFlushImage(decoder.get()) == JXL_DEC_SUCCESS) {
                        flushed_preview = true;
                        break;
                    }
                    // No flushable data yet (a frame smaller than one DC group, typically):
                    // fall through and let the full decode finish normally.
                    continue;
                }
                if (status == JXL_DEC_FULL_IMAGE) {
                    JxlFrameHeader header{};
                    u32 duration_ms = 0;
                    if (info.have_animation != 0 && info.animation.tps_numerator != 0 &&
                        JxlDecoderGetFrameHeader(decoder.get(), &header) == JXL_DEC_SUCCESS) {
                        // JXL frame durations are in ticks, with the tick rate given as a
                        // rational number in the animation header.
                        const double seconds = static_cast<double>(header.duration) *
                                               info.animation.tps_denominator / info.animation.tps_numerator;
                        duration_ms = static_cast<u32>(seconds * 1000.0 + 0.5);
                    }
                    image.frames.push_back(ImageFrame{.pixels = std::move(pixels), .duration_ms = duration_ms});
                    pixels.clear();
                    if (!options.decode_all_frames) {
                        // Stopping at the first frame is both the correct still-image behavior and
                        // avoids decoding every frame of a long animation to throw all but one away.
                        break;
                    }
                    continue;
                }
                if (status == JXL_DEC_SUCCESS) {
                    break;
                }
                // Any other subscribed-but-unhandled event: ignored and looped past.
            }

            if (flushed_preview) {
                image.is_preview = true;
                image.frames.push_back(ImageFrame{.pixels = std::move(pixels)});
            }
            if (image.frames.empty()) {
                return fail("no full image was decoded.");
            }

            const usize expected =
                static_cast<usize>(image.width) * image.height * bytes_per_pixel(image.format);
            for (const ImageFrame &frame : image.frames) {
                if (frame.pixels.size() != expected) {
                    return fail("a decoded frame's size does not match the reported dimensions.");
                }
            }
            if (image.frames.size() == 1) {
                image.frames.front().duration_ms = 0;
            }
            return image;
        }

        /// Reads a component sample at `(x, y)` in *full-resolution* pixel coordinates, upsampling
        /// by nearest-neighbor if this component is subsampled (`comp.dx`/`comp.dy` > 1, as
        /// chroma channels commonly are), and rescaling it to 8 bits (component precision can be
        /// anywhere from 1 to 16 bits, and may be signed).
        ///
        /// @param comp `comp` value used by the operation.
        /// @param x `x` value used by the operation.
        /// @param y `y` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u8 opj_sample_u8(const opj_image_comp_t &comp, u32 x, u32 y) noexcept {
            const u32 sx = std::min(x / std::max<u32>(comp.dx, 1), comp.w - 1);
            const u32 sy = std::min(y / std::max<u32>(comp.dy, 1), comp.h - 1);
            OPJ_INT32 value = comp.data[static_cast<usize>(sy) * comp.w + sx];
            if (comp.sgnd != 0) {
                value += static_cast<OPJ_INT32>(1) << (comp.prec - 1);
            }
            if (comp.prec > 8) {
                value >>= (comp.prec - 8);
            } else if (comp.prec < 8) {
                value <<= (8 - comp.prec);
            }
            return static_cast<u8>(std::clamp(value, 0, 255));
        }

        /// Reads a component sample the same way `opj_sample_u8` does, but rescales to a full
        /// 16-bit range instead of 8. JPEG 2000 routinely carries 12- and 16-bit imagery (it is
        /// the archival and medical-imaging format of choice precisely for that), so the native
        /// path keeps those extra bits rather than shifting them off.
        ///
        /// @param comp `comp` value used by the operation.
        /// @param x `x` value used by the operation.
        /// @param y `y` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u16 opj_sample_u16(const opj_image_comp_t &comp, u32 x, u32 y) noexcept {
            const u32 sx = std::min(x / std::max<u32>(comp.dx, 1), comp.w - 1);
            const u32 sy = std::min(y / std::max<u32>(comp.dy, 1), comp.h - 1);
            OPJ_INT32 value = comp.data[static_cast<usize>(sy) * comp.w + sx];
            if (comp.sgnd != 0) {
                value += static_cast<OPJ_INT32>(1) << (comp.prec - 1);
            }
            const OPJ_INT32 max_value = (static_cast<OPJ_INT32>(1) << comp.prec) - 1;
            value = std::clamp(value, 0, max_value);
            // Scaling rather than left-shifting: a plain shift leaves the low bits zero, so a
            // fully saturated source sample would come back slightly below 65535.
            return static_cast<u16>((static_cast<u32>(value) * 65535u + max_value / 2) / static_cast<u32>(max_value));
        }

        /// Decodes a JPEG 2000 image, either a bare codestream (`.j2k`/`.jpc`) or a JP2
        /// (ISOBMFF-boxed) file — `codec_format` distinguishes them, since OpenJPEG has no
        /// signature-sniffing entry point of its own the way libjxl does.
        ///
        /// Scoped to what this reasonably needs to support: grayscale, RGB, and RGBA component
        /// layouts (1/3/4 components respectively) in the SRGB or grayscale color space, each
        /// component independently up to 16-bit precision and independently subsampled (chroma
        /// subsampling). CMYK and other color spaces are rejected rather than misinterpreted as
        /// RGB.
        ///
        /// @param encoded `encoded` value used by the operation.
        /// @param codec_format Either `OPJ_CODEC_J2K` or `OPJ_CODEC_JP2`.
        /// @param source Source value or resource, used only for error messages.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<DecodedImage> decode_jp2(std::span<const std::byte> encoded,
                                                              OPJ_CODEC_FORMAT codec_format,
                                                              const DecodeOptions &options,
                                                              const std::filesystem::path &source) {
            const auto fail = [&source](const char *reason) {
                return std::unexpected(AssetError{
                    .code = AssetErrorCode::DecodeFailure,
                    .message = UString{std::string{"Could not decode JPEG 2000: "} + reason},
                    .source = source,
                });
            };

            struct MemoryStream {
                const std::byte *data;
                OPJ_SIZE_T size;
                OPJ_SIZE_T offset;
            } memory_stream{encoded.data(), encoded.size(), 0};

            opj_stream_t *stream = opj_stream_create(1 << 16, OPJ_TRUE);
            if (stream == nullptr) {
                return fail("failed to create the input stream.");
            }
            opj_stream_set_user_data(stream, &memory_stream, nullptr);
            opj_stream_set_user_data_length(stream, memory_stream.size);
            opj_stream_set_read_function(stream, [](void *buffer, OPJ_SIZE_T requested, void *user_data) -> OPJ_SIZE_T {
                auto &self = *static_cast<MemoryStream *>(user_data);
                const OPJ_SIZE_T remaining = self.size - self.offset;
                if (remaining == 0) {
                    return static_cast<OPJ_SIZE_T>(-1);
                }
                const OPJ_SIZE_T to_read = std::min(requested, remaining);
                std::memcpy(buffer, self.data + self.offset, to_read);
                self.offset += to_read;
                return to_read;
            });
            opj_stream_set_skip_function(stream, [](OPJ_OFF_T requested, void *user_data) -> OPJ_OFF_T {
                auto &self = *static_cast<MemoryStream *>(user_data);
                const OPJ_SIZE_T remaining = self.size - self.offset;
                const OPJ_SIZE_T to_skip = std::min(static_cast<OPJ_SIZE_T>(std::max<OPJ_OFF_T>(requested, 0)), remaining);
                self.offset += to_skip;
                return static_cast<OPJ_OFF_T>(to_skip);
            });
            opj_stream_set_seek_function(stream, [](OPJ_OFF_T requested, void *user_data) -> OPJ_BOOL {
                auto &self = *static_cast<MemoryStream *>(user_data);
                if (requested < 0 || static_cast<OPJ_SIZE_T>(requested) > self.size) {
                    return OPJ_FALSE;
                }
                self.offset = static_cast<OPJ_SIZE_T>(requested);
                return OPJ_TRUE;
            });

            opj_codec_t *codec = opj_create_decompress(codec_format);
            if (codec == nullptr) {
                opj_stream_destroy(stream);
                return fail("failed to create the codec.");
            }
            opj_dparameters_t params;
            opj_set_default_decoder_parameters(&params);
            if (opj_setup_decoder(codec, &params) == OPJ_FALSE) {
                opj_destroy_codec(codec);
                opj_stream_destroy(stream);
                return fail("failed to configure the decoder.");
            }

            // The decode is split around the header read so the resolution factor can be chosen
            // from the image's real dimensions: JPEG 2000's wavelet codestream is inherently
            // multi-resolution, and asking for a reduction of N decodes only the lowest levels,
            // reading a fraction of the codestream to produce an image 2^N smaller. That is a
            // genuine cheap preview, not a full decode followed by a downscale.
            opj_image_t *image = nullptr;
            bool decoded_ok = opj_read_header(stream, codec, &image) != OPJ_FALSE;
            u32 reduction = 0;
            if (decoded_ok && image != nullptr && options.preview_min_dimension > 0) {
                u32 preview_width = image->x1 - image->x0;
                u32 preview_height = image->y1 - image->y0;
                while (preview_width / 2 >= options.preview_min_dimension &&
                       preview_height / 2 >= options.preview_min_dimension) {
                    preview_width /= 2;
                    preview_height /= 2;
                    ++reduction;
                }
                // A request for more levels than the codestream has fails outright rather than
                // clamping, so a failure here just means "no preview available" and the full
                // decode below proceeds unreduced.
                if (reduction > 0 && opj_set_decoded_resolution_factor(codec, reduction) == OPJ_FALSE) {
                    reduction = 0;
                }
            }
            decoded_ok = decoded_ok && opj_decode(codec, stream, image) != OPJ_FALSE &&
                         opj_end_decompress(codec, stream) != OPJ_FALSE;
            opj_destroy_codec(codec);
            opj_stream_destroy(stream);
            if (!decoded_ok || image == nullptr) {
                if (image != nullptr) {
                    opj_image_destroy(image);
                }
                return fail("the codestream is corrupt or uses an unsupported feature.");
            }

            const bool is_grayscale = image->numcomps == 1 && image->color_space != OPJ_CLRSPC_CMYK;
            const bool is_rgb_ish = (image->numcomps == 3 || image->numcomps == 4) &&
                                    image->color_space != OPJ_CLRSPC_CMYK;
            if ((!is_grayscale && !is_rgb_ish) || image->x1 <= image->x0 || image->y1 <= image->y0) {
                opj_image_destroy(image);
                return fail("unsupported component/color-space layout (only grayscale, RGB, and RGBA are).");
            }
            for (u32 c = 0; c < image->numcomps; ++c) {
                if (image->comps[c].data == nullptr || image->comps[c].prec == 0 || image->comps[c].prec > 16) {
                    opj_image_destroy(image);
                    return fail("a component decoded with no data or an unsupported precision.");
                }
            }

            // A reduced decode shrinks every component but leaves the image's x0/x1/y0/y1 grid
            // coordinates at full-resolution scale, so the output dimensions come from the
            // components themselves rather than from that grid.
            const u32 full_width = image->x1 - image->x0;
            const u32 full_height = image->y1 - image->y0;
            const u32 width = reduction > 0 ? image->comps[0].w : full_width;
            const u32 height = reduction > 0 ? image->comps[0].h : full_height;
            if (width == 0 || height == 0) {
                opj_image_destroy(image);
                return fail("decoded to zero dimensions.");
            }

            // Any component deeper than 8 bits is worth keeping at 16; an 8-bit-or-shallower
            // JPEG 2000 gains nothing from widening and stays in the cheap path.
            bool deep = false;
            for (u32 c = 0; c < image->numcomps; ++c) {
                deep = deep || image->comps[c].prec > 8;
            }
            const bool wide = wants_wide_decode(options) && deep;

            DecodedImage result{
                .width = width,
                .height = height,
                .format = wide ? PixelFormat::Rgba16 : PixelFormat::Rgba8,
            };
            result.is_preview = reduction > 0 && (width < full_width || height < full_height);
            result.full_width = full_width;
            result.full_height = full_height;
            const usize stride = bytes_per_pixel(result.format);
            result.frames.push_back(
                ImageFrame{.pixels = std::vector<std::byte>(static_cast<usize>(width) * height * stride)});
            std::vector<std::byte> &pixels = result.pixels();

            const auto store = [&](usize dest, usize channel, u32 x, u32 y, u32 component) {
                if (wide) {
                    const u16 value = opj_sample_u16(image->comps[component], x, y);
                    std::memcpy(pixels.data() + dest + channel * sizeof(u16), &value, sizeof(value));
                } else {
                    pixels[dest + channel] = std::byte{opj_sample_u8(image->comps[component], x, y)};
                }
            };
            const auto store_opaque = [&](usize dest, usize channel) {
                if (wide) {
                    const u16 value = 65535;
                    std::memcpy(pixels.data() + dest + channel * sizeof(u16), &value, sizeof(value));
                } else {
                    pixels[dest + channel] = std::byte{255};
                }
            };

            for (u32 y = 0; y < height; ++y) {
                for (u32 x = 0; x < width; ++x) {
                    const usize dest = (static_cast<usize>(y) * width + x) * stride;
                    if (is_grayscale) {
                        store(dest, 0, x, y, 0);
                        store(dest, 1, x, y, 0);
                        store(dest, 2, x, y, 0);
                        store_opaque(dest, 3);
                    } else {
                        store(dest, 0, x, y, 0);
                        store(dest, 1, x, y, 1);
                        store(dest, 2, x, y, 2);
                        if (image->numcomps >= 4) {
                            store(dest, 3, x, y, 3);
                        } else {
                            store_opaque(dest, 3);
                        }
                    }
                }
            }

            opj_image_destroy(image);
            return result;
        }

        /// Decodes the largest embedded image out of an ICO/CUR container.
        ///
        /// An entry is either a plain PNG (Vista-era large icons; handed to `decode_with_stb`) or a
        /// DIB: a `BITMAPINFOHEADER` immediately followed by pixel data, with no
        /// `BITMAPFILEHEADER`. Scoped to what real-world icon files actually contain — 32bpp BGRA
        /// (the modern, alpha-carrying case) and 24bpp BGR with a following 1bpp AND mask (the
        /// pre-Vista case). Older palette depths (1/4/8bpp) are rejected rather than guessed at;
        /// nothing still produces those.
        ///
        /// @param encoded `encoded` value used by the operation.
        /// @param source Source value or resource, used only for error messages.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<DecodedImage> decode_ico(std::span<const std::byte> encoded,
                                                              const DecodeOptions &options,
                                                              const std::filesystem::path &source) {
            const auto fail = [&source](const char *reason) {
                return std::unexpected(AssetError{
                    .code = AssetErrorCode::DecodeFailure,
                    .message = UString{std::string{"Could not decode ICO/CUR: "} + reason},
                    .source = source,
                });
            };

            const u16 count = read_u16_le(encoded, 4);
            if (count == 0 || encoded.size() < static_cast<usize>(6) + static_cast<usize>(count) * 16) {
                return fail("directory is truncated or empty.");
            }

            usize best_entry = 6;
            u32 best_area = 0;
            for (u16 i = 0; i < count; ++i) {
                const usize entry_offset = 6 + static_cast<usize>(i) * 16;
                const u32 width = encoded[entry_offset] == std::byte{0}
                                      ? 256u
                                      : std::to_integer<u32>(encoded[entry_offset]);
                const u32 height = encoded[entry_offset + 1] == std::byte{0}
                                       ? 256u
                                       : std::to_integer<u32>(encoded[entry_offset + 1]);
                const u32 area = width * height;
                if (area > best_area) {
                    best_area = area;
                    best_entry = entry_offset;
                }
            }

            const u32 image_size = read_u32_le(encoded, best_entry + 8);
            const u32 image_offset = read_u32_le(encoded, best_entry + 12);
            if (image_size < 4 || static_cast<u64>(image_offset) + image_size > encoded.size()) {
                return fail("embedded image is out of range.");
            }
            const std::span<const std::byte> image = encoded.subspan(image_offset, image_size);

            if (image.size() >= 8 && std::equal(std::begin(kPngMagic), std::end(kPngMagic), image.begin())) {
                return decode_with_stb(image, options, source);
            }

            if (image.size() < 40) {
                return fail("embedded DIB header is truncated.");
            }
            const u32 header_size = read_u32_le(image, 0);
            const i32 dib_width = read_i32_le(image, 4);
            // biHeight is double the icon's real height here: it covers the XOR color image
            // followed by an equal-height AND mask, stacked as one bottom-up bitmap.
            const i32 dib_height_field = read_i32_le(image, 8);
            const u16 bit_count = read_u16_le(image, 14);
            const u32 compression = read_u32_le(image, 16);
            if (header_size < 40 || dib_width <= 0 || dib_height_field <= 0 || (dib_height_field % 2) != 0) {
                return fail("embedded DIB has an unsupported header.");
            }
            if (compression != 0 /* BI_RGB, uncompressed */) {
                return fail("embedded DIB uses unsupported compression.");
            }
            if (bit_count != 32 && bit_count != 24) {
                return fail("embedded DIB bit depth is not supported (only 24 and 32 bpp are).");
            }

            const u32 width = static_cast<u32>(dib_width);
            const u32 height = static_cast<u32>(dib_height_field / 2);
            const usize pixel_data_offset = header_size;
            const usize row_bytes = ((static_cast<usize>(width) * bit_count + 31) / 32) * 4;
            const usize color_bytes = row_bytes * height;
            if (pixel_data_offset + color_bytes > image.size()) {
                return fail("embedded DIB pixel data is truncated.");
            }

            // AND mask: 1 bit per pixel, rows padded to a 4-byte boundary, immediately following
            // the color data. 32bpp icons already carry real alpha and conventionally ignore this
            // mask entirely (Windows itself does), so it is only consulted for the 24bpp case.
            const bool has_mask = bit_count == 24;
            const usize mask_row_bytes = ((static_cast<usize>(width) + 31) / 32) * 4;
            if (has_mask && pixel_data_offset + color_bytes + mask_row_bytes * height > image.size()) {
                return fail("embedded DIB AND mask is truncated.");
            }

            DecodedImage result{
                .width = width,
                .height = height,
                .frames = {ImageFrame{.pixels = std::vector<std::byte>(static_cast<usize>(width) * height * 4)}},
            };
            std::vector<std::byte> &pixels = result.pixels();
            // The DIB is stored bottom-up: row 0 on disk is the bottom row of the image.
            for (u32 y = 0; y < height; ++y) {
                const u32 dest_row = height - 1 - y;
                const usize color_row_offset = pixel_data_offset + static_cast<usize>(y) * row_bytes;
                const usize mask_row_offset = pixel_data_offset + color_bytes + static_cast<usize>(y) * mask_row_bytes;
                for (u32 x = 0; x < width; ++x) {
                    const usize src = color_row_offset + static_cast<usize>(x) * (bit_count / 8);
                    const u8 b = std::to_integer<u8>(image[src]);
                    const u8 g = std::to_integer<u8>(image[src + 1]);
                    const u8 r = std::to_integer<u8>(image[src + 2]);
                    u8 a = 255;
                    if (bit_count == 32) {
                        a = std::to_integer<u8>(image[src + 3]);
                    } else if (has_mask) {
                        const usize mask_byte_index = mask_row_offset + x / 8;
                        const u8 mask_byte = std::to_integer<u8>(image[mask_byte_index]);
                        const bool transparent = (mask_byte & (0x80u >> (x % 8))) != 0;
                        a = transparent ? 0 : 255;
                    }
                    const usize dest = (static_cast<usize>(dest_row) * width + x) * 4;
                    pixels[dest + 0] = std::byte{r};
                    pixels[dest + 1] = std::byte{g};
                    pixels[dest + 2] = std::byte{b};
                    pixels[dest + 3] = std::byte{a};
                }
            }
            return result;
        }

        [[nodiscard]] bool looks_like_sgi(std::span<const std::byte> encoded) noexcept {
            return encoded.size() >= 512 && read_u16_be(encoded, 0) == 0x01DAu;
        }

        /// Decodes one scanline of SGI's RLE codec into `dest` (exactly `dest.size()` `bpc`-byte
        /// units). A control byte's low 7 bits are a run length; if its top bit is set, that many
        /// verbatim units follow, otherwise a single unit follows and repeats that many times. A
        /// run length of 0 ends the scanline early (used when trailing units are all one value).
        ///
        /// @return false if the row is malformed (out of bounds, or doesn't produce exactly `dest.size()` units).
        [[nodiscard]] bool sgi_rle_decode_row(std::span<const std::byte> encoded, usize offset, usize length,
                                              u32 bpc, std::span<u16> dest) noexcept {
            usize pos = offset;
            const usize end = std::min(offset + length, encoded.size());
            usize written = 0;
            while (written < dest.size()) {
                if (pos >= end) return false;
                const u8 control = std::to_integer<u8>(encoded[pos++]);
                const u32 run = control & 0x7Fu;
                if (run == 0) break;
                if (written + run > dest.size()) return false;
                if (control & 0x80u) {
                    for (u32 i = 0; i < run; ++i) {
                        if (pos + bpc > end) return false;
                        dest[written++] = bpc == 1 ? std::to_integer<u16>(encoded[pos]) : read_u16_be(encoded, pos);
                        pos += bpc;
                    }
                } else {
                    if (pos + bpc > end) return false;
                    const u16 value = bpc == 1 ? std::to_integer<u16>(encoded[pos]) : read_u16_be(encoded, pos);
                    pos += bpc;
                    for (u32 i = 0; i < run; ++i) dest[written++] = value;
                }
            }
            return written == dest.size();
        }

        /// Decodes an SGI/Iris raster image (`.rgb`/`.rgba`/`.sgi`/`.bw`), storage mode VERBATIM
        /// (raw) or RLE, 1 or 2 bytes per channel, 1 (greyscale), 3 (RGB), or 4 (RGBA) channels.
        /// Pixel data is stored channel-major (all of channel 0's scanlines, then channel 1's, ...)
        /// with each scanline bottom-up on disk; both are un-done here so `DecodedImage` comes out
        /// top-down and channel-interleaved like every other decoder in this file. 16-bit-per-
        /// channel samples are downscaled to 8-bit by keeping the high byte.
        [[nodiscard]] AssetExpected<DecodedImage> decode_sgi(std::span<const std::byte> encoded,
                                                              const std::filesystem::path &source) {
            const auto fail = [&source](const char *reason) {
                return std::unexpected(AssetError{
                    .code = AssetErrorCode::DecodeFailure,
                    .message = UString{std::string{"Could not decode SGI/Iris image: "} + reason},
                    .source = source,
                });
            };

            const u8 storage = std::to_integer<u8>(encoded[2]);
            const u8 bpc = std::to_integer<u8>(encoded[3]);
            const u32 width = read_u16_be(encoded, 6);
            const u32 height = read_u16_be(encoded, 8);
            const u32 channels = read_u16_be(encoded, 10);

            if (width == 0 || height == 0) return fail("reported zero dimensions.");
            if (bpc != 1 && bpc != 2) return fail("unsupported bytes-per-channel (only 1 and 2 are).");
            if (channels != 1 && channels != 3 && channels != 4) {
                return fail("unsupported channel count (only 1, 3, and 4 are).");
            }
            if (storage != 0 && storage != 1) return fail("unrecognized storage mode.");

            const usize plane_size = static_cast<usize>(width) * height;
            std::vector<u16> planes(plane_size * channels);

            if (storage == 0) {
                const usize needed = plane_size * channels * bpc;
                if (encoded.size() < 512 + needed) return fail("pixel data is truncated.");
                usize pos = 512;
                for (usize i = 0; i < planes.size(); ++i) {
                    planes[i] = bpc == 1 ? std::to_integer<u16>(encoded[pos]) : read_u16_be(encoded, pos);
                    pos += bpc;
                }
            } else {
                const usize row_count = static_cast<usize>(height) * channels;
                const usize table_bytes = row_count * 4;
                if (encoded.size() < 512 + table_bytes * 2) return fail("RLE offset/length table is truncated.");
                const usize offset_table = 512;
                const usize length_table = 512 + table_bytes;
                for (u32 z = 0; z < channels; ++z) {
                    for (u32 y = 0; y < height; ++y) {
                        const usize idx = static_cast<usize>(z) * height + y;
                        const u32 row_offset = read_u32_be(encoded, offset_table + idx * 4);
                        const u32 row_length = read_u32_be(encoded, length_table + idx * 4);
                        if (static_cast<u64>(row_offset) + row_length > encoded.size()) {
                            return fail("RLE scanline is out of range.");
                        }
                        const std::span<u16> dest(planes.data() + static_cast<usize>(z) * plane_size +
                                                       static_cast<usize>(y) * width,
                                                   width);
                        if (!sgi_rle_decode_row(encoded, row_offset, row_length, bpc, dest)) {
                            return fail("RLE scanline is malformed.");
                        }
                    }
                }
            }

            DecodedImage result{
                .width = width,
                .height = height,
                .frames = {ImageFrame{.pixels = std::vector<std::byte>(plane_size * 4)}},
            };
            std::vector<std::byte> &pixels = result.pixels();
            const auto to_u8 = [bpc](u16 value) noexcept -> u8 {
                return bpc == 1 ? static_cast<u8>(value) : static_cast<u8>(value >> 8);
            };
            for (u32 y = 0; y < height; ++y) {
                const u32 dest_row = height - 1 - y; // Bottom-up on disk, top-down in DecodedImage.
                for (u32 x = 0; x < width; ++x) {
                    const usize src = static_cast<usize>(y) * width + x;
                    u8 r, g, b;
                    u8 a = 255;
                    if (channels == 1) {
                        r = g = b = to_u8(planes[src]);
                    } else {
                        r = to_u8(planes[0 * plane_size + src]);
                        g = to_u8(planes[1 * plane_size + src]);
                        b = to_u8(planes[2 * plane_size + src]);
                        if (channels == 4) a = to_u8(planes[3 * plane_size + src]);
                    }
                    const usize dest = (static_cast<usize>(dest_row) * width + x) * 4;
                    pixels[dest + 0] = std::byte{r};
                    pixels[dest + 1] = std::byte{g};
                    pixels[dest + 2] = std::byte{b};
                    pixels[dest + 3] = std::byte{a};
                }
            }
            return result;
        }

        [[nodiscard]] bool looks_like_dpx(std::span<const std::byte> encoded) noexcept {
            if (encoded.size() < 1408) return false;
            static constexpr std::byte kBe[4] = {std::byte{'S'}, std::byte{'D'}, std::byte{'P'}, std::byte{'X'}};
            static constexpr std::byte kLe[4] = {std::byte{'X'}, std::byte{'P'}, std::byte{'D'}, std::byte{'S'}};
            return std::equal(std::begin(kBe), std::end(kBe), encoded.begin()) ||
                   std::equal(std::begin(kLe), std::end(kLe), encoded.begin());
        }

        /// Decodes an SMPTE 268M (DPX) image: a single RGB or RGBA image element, uncompressed,
        /// unsigned samples at 8, 10, or 16 bits per sample. 10-bit is the common film-scan case and
        /// is only supported in its near-universal form: three 10-bit samples packed into one 32-bit
        /// word per pixel ("Filled Method A" packing, verified against real encoder output — the
        /// low 2 bits of the word are padding, then B, G, R from low to high). Every scanline is
        /// padded to a 4-byte boundary regardless of bit depth (the generic-header `line_padding`
        /// field, checked below, is *additional* padding on top of that, not instead of it — also
        /// verified against real encoder output). RLE-encoded elements (`encoding != 0`) are
        /// rejected rather than decoded, matching this file's general stance of erroring on
        /// unimplemented codecs rather than misinterpreting them.
        [[nodiscard]] AssetExpected<DecodedImage> decode_dpx(std::span<const std::byte> encoded,
                                                              const DecodeOptions &options,
                                                              const std::filesystem::path &source) {
            const auto fail = [&source](const char *reason) {
                return std::unexpected(AssetError{
                    .code = AssetErrorCode::DecodeFailure,
                    .message = UString{std::string{"Could not decode DPX: "} + reason},
                    .source = source,
                });
            };

            const bool big_endian = encoded[0] == std::byte{'S'};
            const auto read_u32 = [&](usize offset) noexcept -> u32 {
                return big_endian ? read_u32_be(encoded, offset) : read_u32_le(encoded, offset);
            };
            const auto read_u16 = [&](usize offset) noexcept -> u16 {
                return big_endian ? read_u16_be(encoded, offset) : read_u16_le(encoded, offset);
            };

            const u32 image_data_offset = read_u32(4);
            const u16 number_elements = read_u16(770);
            const u32 width = read_u32(772);
            const u32 height = read_u32(776);
            if (width == 0 || height == 0) return fail("reported zero dimensions.");
            if (number_elements == 0) return fail("no image elements present.");
            if (static_cast<u64>(image_data_offset) >= encoded.size()) return fail("image data offset is out of range.");

            // Only the first (and, for RGB/RGBA files, only) image element is supported.
            constexpr usize kElement = 780;
            const u32 data_sign = read_u32(kElement + 0);
            const u8 descriptor = std::to_integer<u8>(encoded[kElement + 20]);
            // SMPTE 268M transfer characteristic; 3 is "Logarithmic", the printing-density
            // encoding a film scan uses and the reason DPX exists alongside plain image formats.
            const u8 transfer_characteristic = std::to_integer<u8>(encoded[kElement + 21]);
            const u8 bit_size = std::to_integer<u8>(encoded[kElement + 23]);
            const u16 packing = read_u16(kElement + 24);
            const u16 encoding = read_u16(kElement + 26);
            const u32 line_padding = read_u32(kElement + 32);

            if (data_sign != 0) return fail("signed sample data is not supported.");
            if (encoding != 0) return fail("RLE-encoded DPX is not supported.");

            u32 channels;
            bool has_alpha;
            if (descriptor == 50) {
                channels = 3;
                has_alpha = false;
            } else if (descriptor == 51) {
                channels = 4;
                has_alpha = true;
            } else {
                return fail("unsupported image descriptor (only RGB and RGBA are).");
            }
            if (bit_size == 10 && channels != 3) return fail("10-bit is only supported for RGB, not RGBA.");
            if (bit_size == 10 && packing != 1) return fail("10-bit DPX must use \"Filled Method A\" packing.");
            if (bit_size != 8 && bit_size != 10 && bit_size != 16) {
                return fail("unsupported bit depth (only 8, 10, and 16 are).");
            }

            // 8-bit DPX has nothing extra to preserve, so only the 10- and 16-bit cases take a
            // wide path. Log-encoded film scans additionally become scene-linear half-float,
            // because the highlight headroom above the print white point is the data that makes a
            // DPX worth reading and no integer 0-1 encoding can hold it.
            const bool log_encoded = transfer_characteristic == 3 && bit_size == 10;
            const bool native = wants_wide_decode(options) && bit_size > 8;
            const PixelFormat pixel_format = !native            ? PixelFormat::Rgba8
                                             : log_encoded ? PixelFormat::Rgba16F
                                                           : PixelFormat::Rgba16;
            DecodedImage result{
                .width = width,
                .height = height,
                .format = pixel_format,
                .transfer_function = log_encoded && native ? u8{8} : u8{13}, // 8 = linear.
            };
            const usize stride = bytes_per_pixel(pixel_format);
            result.frames.push_back(
                ImageFrame{.pixels = std::vector<std::byte>(static_cast<usize>(width) * height * stride)});
            std::vector<std::byte> &pixels = result.pixels();

            // Writes one channel of one pixel, narrowing or widening `code` (which is always in
            // the source's own `sample_max`-relative range) to whatever `pixel_format` needs.
            const auto store = [&](usize dest, usize channel, u32 code, u32 sample_max) noexcept {
                switch (pixel_format) {
                    case PixelFormat::Rgba8:
                        pixels[dest + channel] =
                            std::byte{static_cast<u8>(code * 255u / std::max(sample_max, 1u))};
                        return;
                    case PixelFormat::Rgba16: {
                        const u16 value =
                            static_cast<u16>((static_cast<u64>(code) * 65535u + sample_max / 2) /
                                             std::max(sample_max, 1u));
                        std::memcpy(pixels.data() + dest + channel * sizeof(u16), &value, sizeof(value));
                        return;
                    }
                    case PixelFormat::Rgba16F: {
                        const u16 value = float_to_half(cineon_log_to_linear(code));
                        std::memcpy(pixels.data() + dest + channel * sizeof(u16), &value, sizeof(value));
                        return;
                    }
                    case PixelFormat::Rgba32F:
                        return; // Never produced by this decoder.
                }
            };
            const auto store_opaque = [&](usize dest) noexcept {
                switch (pixel_format) {
                    case PixelFormat::Rgba8: pixels[dest + 3] = std::byte{255}; return;
                    case PixelFormat::Rgba16: {
                        const u16 value = 65535;
                        std::memcpy(pixels.data() + dest + 3 * sizeof(u16), &value, sizeof(value));
                        return;
                    }
                    case PixelFormat::Rgba16F: {
                        const u16 value = float_to_half(1.0f);
                        std::memcpy(pixels.data() + dest + 3 * sizeof(u16), &value, sizeof(value));
                        return;
                    }
                    case PixelFormat::Rgba32F: return;
                }
            };

            if (bit_size == 10) {
                const usize row_bytes = static_cast<usize>(width) * 4 + line_padding;
                if (encoded.size() < static_cast<usize>(image_data_offset) + row_bytes * height) {
                    return fail("pixel data is truncated.");
                }
                for (u32 y = 0; y < height; ++y) {
                    const usize row_offset = image_data_offset + static_cast<usize>(y) * row_bytes;
                    for (u32 x = 0; x < width; ++x) {
                        const u32 word = read_u32(row_offset + static_cast<usize>(x) * 4);
                        const usize dest = (static_cast<usize>(y) * width + x) * stride;
                        store(dest, 0, (word >> 22) & 0x3FFu, 1023u);
                        store(dest, 1, (word >> 12) & 0x3FFu, 1023u);
                        store(dest, 2, (word >> 2) & 0x3FFu, 1023u);
                        store_opaque(dest);
                    }
                }
            } else {
                const u32 bytes_per_sample = bit_size == 8 ? 1u : 2u;
                const u32 sample_max = bit_size == 8 ? 255u : 65535u;
                const usize unpadded_row_bytes = static_cast<usize>(width) * channels * bytes_per_sample;
                const usize row_bytes = ((unpadded_row_bytes + 3) / 4) * 4 + line_padding;
                if (encoded.size() < static_cast<usize>(image_data_offset) + row_bytes * height) {
                    return fail("pixel data is truncated.");
                }
                for (u32 y = 0; y < height; ++y) {
                    const usize row_offset = image_data_offset + static_cast<usize>(y) * row_bytes;
                    for (u32 x = 0; x < width; ++x) {
                        const usize pixel_offset = row_offset + static_cast<usize>(x) * channels * bytes_per_sample;
                        const auto sample = [&](u32 c) noexcept -> u32 {
                            if (bit_size == 8) return std::to_integer<u32>(encoded[pixel_offset + c]);
                            return read_u16(pixel_offset + static_cast<usize>(c) * 2);
                        };
                        const usize dest = (static_cast<usize>(y) * width + x) * stride;
                        store(dest, 0, sample(0), sample_max);
                        store(dest, 1, sample(1), sample_max);
                        store(dest, 2, sample(2), sample_max);
                        if (has_alpha) {
                            store(dest, 3, sample(3), sample_max);
                        } else {
                            store_opaque(dest);
                        }
                    }
                }
            }
            return result;
        }

        [[nodiscard]] bool looks_like_cineon(std::span<const std::byte> encoded) noexcept {
            if (encoded.size() < 208) return false;
            // The on-disk magic bytes are the file's own byte order applied to the constant
            // 0x802A5FD7 -- verified against real encoder output, which writes 80 2A 5F D7 (i.e.
            // big-endian) and expects every other multi-byte field read the same way.
            static constexpr std::byte kBe[4] = {std::byte{0x80}, std::byte{0x2A}, std::byte{0x5F}, std::byte{0xD7}};
            static constexpr std::byte kLe[4] = {std::byte{0xD7}, std::byte{0x5F}, std::byte{0x2A}, std::byte{0x80}};
            return std::equal(std::begin(kBe), std::end(kBe), encoded.begin()) ||
                   std::equal(std::begin(kLe), std::end(kLe), encoded.begin());
        }


        /// Converts a 10-bit Cineon printing-density code value to an 8-bit sRGB sample.
        ///
        /// @param code10 `code10` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u8 cineon_log_to_srgb_u8(u32 code10) noexcept {
            return linear_to_srgb_u8(cineon_log_to_linear(code10));
        }

        /// Decodes a Kodak Cineon image: always a single RGB image element, packed the same way as
        /// DPX's 10-bit "Filled Method A" (Cineon predates DPX and DPX inherited this packing from
        /// it) -- one 32-bit big-endian word per pixel, no line padding, no RLE (Cineon's format has
        /// no encoding field at all; every real file is uncompressed). Only 10-bit (the standard,
        /// effectively universal case) is supported; other bit depths are rejected rather than
        /// guessed at, since they don't occur in practice and this file's own per-channel structures
        /// don't reliably indicate real channel identity (verified against real encoder output: a
        /// 3-channel RGB file writes identical, non-distinguishing designator bytes for all three
        /// channel entries -- everything actually needed (dimensions, bit depth, data offset) is
        /// read from the first entry only).
        [[nodiscard]] AssetExpected<DecodedImage> decode_cineon(std::span<const std::byte> encoded,
                                                                 const DecodeOptions &options,
                                                                 const std::filesystem::path &source) {
            const auto fail = [&source](const char *reason) {
                return std::unexpected(AssetError{
                    .code = AssetErrorCode::DecodeFailure,
                    .message = UString{std::string{"Could not decode Cineon: "} + reason},
                    .source = source,
                });
            };

            const bool big_endian = encoded[0] == std::byte{0x80};
            const auto read_u32 = [&](usize offset) noexcept -> u32 {
                return big_endian ? read_u32_be(encoded, offset) : read_u32_le(encoded, offset);
            };

            const u32 image_data_offset = read_u32(4);
            constexpr usize kFirstChannel = 196;
            const u8 bits_per_pixel = std::to_integer<u8>(encoded[kFirstChannel + 2]);
            const u32 width = read_u32(kFirstChannel + 4);
            const u32 height = read_u32(kFirstChannel + 8);

            if (width == 0 || height == 0) return fail("reported zero dimensions.");
            if (bits_per_pixel != 10) return fail("unsupported bit depth (only 10 is).");
            if (static_cast<u64>(image_data_offset) >= encoded.size()) return fail("image data offset is out of range.");

            const usize row_bytes = static_cast<usize>(width) * 4;
            if (encoded.size() < static_cast<usize>(image_data_offset) + row_bytes * height) {
                return fail("pixel data is truncated.");
            }

            // Cineon is printing-density log film scan data, whose whole point is the highlight
            // headroom above the 685 white point. Narrowing that to 8-bit sRGB throws exactly it
            // away, so the native path converts to scene-linear half-float and keeps it; the
            // 8-bit path is unchanged.
            const bool native = wants_wide_decode(options);
            DecodedImage result{
                .width = width,
                .height = height,
                .format = native ? PixelFormat::Rgba16F : PixelFormat::Rgba8,
                .transfer_function = native ? u8{8} : u8{13}, // 8 = linear.
            };
            const usize stride = bytes_per_pixel(result.format);
            result.frames.push_back(
                ImageFrame{.pixels = std::vector<std::byte>(static_cast<usize>(width) * height * stride)});
            std::vector<std::byte> &pixels = result.pixels();
            for (u32 y = 0; y < height; ++y) {
                const usize row_offset = image_data_offset + static_cast<usize>(y) * row_bytes;
                for (u32 x = 0; x < width; ++x) {
                    const u32 word = read_u32(row_offset + static_cast<usize>(x) * 4);
                    const u32 code[3] = {(word >> 22) & 0x3FFu, (word >> 12) & 0x3FFu, (word >> 2) & 0x3FFu};
                    const usize dest = (static_cast<usize>(y) * width + x) * stride;
                    if (native) {
                        for (usize c = 0; c < 3; ++c) {
                            const u16 half = float_to_half(cineon_log_to_linear(code[c]));
                            std::memcpy(pixels.data() + dest + c * sizeof(u16), &half, sizeof(half));
                        }
                        const u16 opaque = float_to_half(1.0f);
                        std::memcpy(pixels.data() + dest + 3 * sizeof(u16), &opaque, sizeof(opaque));
                    } else {
                        pixels[dest + 0] = std::byte{cineon_log_to_srgb_u8(code[0])};
                        pixels[dest + 1] = std::byte{cineon_log_to_srgb_u8(code[1])};
                        pixels[dest + 2] = std::byte{cineon_log_to_srgb_u8(code[2])};
                        pixels[dest + 3] = std::byte{255};
                    }
                }
            }
            return result;
        }

        /// Reads channel `channel` of pixel `index` out of a buffer in `format`, as a normalized
        /// float: 0-1 for the integer formats, and the stored value verbatim for the float ones
        /// (which are scene-linear and legitimately exceed 1).
        ///
        /// @param pixels `pixels` value used by the operation.
        /// @param format `format` value used by the operation.
        /// @param index Zero-based pixel index.
        /// @param channel Zero-based channel index, 0-3 for R, G, B, A.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 read_channel(const std::vector<std::byte> &pixels,
                                       PixelFormat format,
                                       usize index,
                                       usize channel) noexcept {
            const usize offset = index * bytes_per_pixel(format) + channel * (bytes_per_pixel(format) / 4);
            switch (format) {
                case PixelFormat::Rgba8:
                    return static_cast<f32>(std::to_integer<u8>(pixels[offset])) / 255.0f;
                case PixelFormat::Rgba16: {
                    u16 value = 0;
                    std::memcpy(&value, pixels.data() + offset, sizeof(value));
                    return static_cast<f32>(value) / 65535.0f;
                }
                case PixelFormat::Rgba16F: {
                    u16 value = 0;
                    std::memcpy(&value, pixels.data() + offset, sizeof(value));
                    return half_to_float(value);
                }
                case PixelFormat::Rgba32F: {
                    f32 value = 0.0f;
                    std::memcpy(&value, pixels.data() + offset, sizeof(value));
                    return value;
                }
            }
            return 0.0f;
        }

        /// Reports whether two sets of primaries are the same gamut, so an image already in the
        /// destination space can skip the matrix entirely.
        ///
        /// Compared exactly rather than with a tolerance: both sides come from the same table of
        /// standard chromaticities, so equal gamuts are bit-identical, and two gamuts that differ
        /// at all differ by far more than any sensible epsilon.
        ///
        /// @param lhs `lhs` value used by the operation.
        /// @param rhs `rhs` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool same_gamut(const ColorPrimaries &lhs, const ColorPrimaries &rhs) noexcept {
            const auto same = [](Chromaticity a, Chromaticity b) noexcept {
                return a.x == b.x && a.y == b.y;
            };
            return same(lhs.red, rhs.red) && same(lhs.green, rhs.green) && same(lhs.blue, rhs.blue) &&
                   same(lhs.white, rhs.white);
        }

        /// Converts one display-encoded or scene-linear sample to linear light using `transfer`.
        ///
        /// @param encoded `encoded` value used by the operation.
        /// @param transfer `transfer` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 to_linear(f32 encoded, TransferFunction transfer) noexcept {
            switch (transfer) {
                case TransferFunction::Linear:
                    return encoded;
                case TransferFunction::Pq:
                    return pq_eotf_to_linear(encoded);
                case TransferFunction::Hlg:
                    return hlg_eotf_to_linear(encoded);
                case TransferFunction::Srgb:
                case TransferFunction::Unsupported:
                    // Unsupported is treated as sRGB rather than as an error: it means a real curve
                    // this engine has no inverse for, and assuming the near-universal one is both
                    // the least-bad guess and never produces a black or blown result.
                    return encoded <= 0.04045f ? encoded / 12.92f
                                               : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
            }
            return encoded;
        }

        /// Converts `image` in place to the color space `precision` names, applying the full input
        /// color transform: the source's transfer function is inverted to linear light, the
        /// source's primaries are converted to the engine's working primaries with Bradford
        /// chromatic adaptation between their white points, out-of-gamut results are mapped back in
        /// per `mapping`, and the result is written in the destination's own encoding.
        ///
        /// This is the single place a decode leaves whatever the file happened to be in and lands
        /// somewhere the rest of the engine can reason about, which is why every decoder above is
        /// free to just produce the source's native representation and report what it was.
        ///
        /// `DecodePrecision::Native` returns immediately: it is the request to *not* do any of this.
        ///
        /// @param image `image` value used by the operation.
        /// @param precision `precision` value used by the operation.
        /// @param mapping `mapping` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void convert_to_destination(DecodedImage &image,
                                    DecodePrecision precision,
                                    GamutMapping mapping) noexcept {
            if (precision == DecodePrecision::Native) {
                return;
            }

            const bool scene_linear = precision == DecodePrecision::SceneLinear;
            const PixelFormat destination_format =
                scene_linear ? PixelFormat::Rgba16F : PixelFormat::Rgba8;

            const TransferFunction transfer = transfer_function_from_h273(image.transfer_function);
            // Whether a channel above 1.0 is a gamut artifact or real brightness, which decides
            // whether the gamut mapper is allowed to pull it down.
            //
            // A display-referred source (an sRGB-encoded PNG, say) is bounded at 1.0 by
            // definition, so anything above that came out of the primaries conversion and *is* a
            // gamut error -- desaturating it is exactly right. A scene-referred source (EXR's
            // linear light, PQ, HLG) legitimately carries highlights far above diffuse white;
            // desaturating those toward their own luminance turns a bright red highlight flat
            // white, which is a dynamic-range problem being mis-solved as a gamut one. Those are
            // left to clip at the 8-bit encode instead -- this decoder deliberately does no tone
            // mapping, which is the renderer's job and a stage it already has.
            const bool source_is_scene_referred =
                transfer == TransferFunction::Linear || transfer == TransferFunction::Pq ||
                transfer == TransferFunction::Hlg;
            const f32 maximum = scene_linear || source_is_scene_referred
                                    ? std::numeric_limits<f32>::infinity()
                                    : 1.0f;
            const ColorPrimaries destination = working_space_primaries();
            const std::optional<ColorPrimaries> source = primaries_from_h273(image.color_primaries);
            // A source that names no gamut (H.273 "unspecified", the overwhelmingly common case for
            // an untagged PNG or JPEG) is taken to already be in the working space, which is the
            // same assumption every non-color-managed pipeline makes and the only one available.
            const bool convert_primaries = source.has_value() && !same_gamut(*source, destination);
            const ColorMatrix3 matrix =
                convert_primaries ? color_conversion_matrix(*source, destination) : ColorMatrix3::identity();

            // The overwhelmingly common case: an ordinary 8-bit sRGB image asked for as 8-bit sRGB.
            // Nothing to invert, convert, map, or re-encode, so the whole per-pixel loop below is
            // skipped rather than run as an expensive identity.
            if (!scene_linear && !convert_primaries && image.format == PixelFormat::Rgba8 &&
                (transfer == TransferFunction::Srgb || transfer == TransferFunction::Unsupported)) {
                image.color_primaries = 1;
                image.transfer_function = 13;
                return;
            }

            const PixelFormat source_format = image.format;
            const usize pixel_count = static_cast<usize>(image.width) * image.height;
            const usize destination_stride = bytes_per_pixel(destination_format);

            for (ImageFrame &frame : image.frames) {
                std::vector<std::byte> converted(pixel_count * destination_stride);
                for (usize i = 0; i < pixel_count; ++i) {
                    std::array<f32, 3> rgb{
                        to_linear(read_channel(frame.pixels, source_format, i, 0), transfer),
                        to_linear(read_channel(frame.pixels, source_format, i, 1), transfer),
                        to_linear(read_channel(frame.pixels, source_format, i, 2), transfer),
                    };
                    if (convert_primaries) {
                        rgb = matrix.apply(rgb);
                    }
                    rgb = map_into_gamut(rgb, mapping, maximum);

                    const usize destination_offset = i * destination_stride;
                    if (scene_linear) {
                        for (usize c = 0; c < 3; ++c) {
                            const u16 half = float_to_half(rgb[c]);
                            std::memcpy(converted.data() + destination_offset + c * sizeof(u16), &half,
                                        sizeof(half));
                        }
                    } else {
                        for (usize c = 0; c < 3; ++c) {
                            converted[destination_offset + c] = std::byte{linear_to_srgb_u8(rgb[c])};
                        }
                    }

                    // Alpha is coverage, not light: no transfer function, no primaries, no gamut --
                    // only the range clamp its destination encoding requires.
                    const f32 alpha = std::clamp(read_channel(frame.pixels, source_format, i, 3), 0.0f, 1.0f);
                    if (scene_linear) {
                        const u16 half = float_to_half(alpha);
                        std::memcpy(converted.data() + destination_offset + 3 * sizeof(u16), &half,
                                    sizeof(half));
                    } else {
                        converted[destination_offset + 3] =
                            std::byte{static_cast<u8>(std::clamp(alpha * 255.0f + 0.5f, 0.0f, 255.0f))};
                    }
                }
                frame.pixels = std::move(converted);
            }

            image.format = destination_format;
            image.color_primaries = 1;                        // BT.709/sRGB primaries.
            image.transfer_function = scene_linear ? u8{8} : u8{13}; // 8 = linear, 13 = sRGB.
        }

    } // namespace

    /// Returns the number of bytes one pixel of `format` occupies.
    ///
    /// @param format `format` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    u32 bytes_per_pixel(PixelFormat format) noexcept {
        switch (format) {
            case PixelFormat::Rgba8: return 4;
            case PixelFormat::Rgba16: return 8;
            case PixelFormat::Rgba16F: return 8;
            case PixelFormat::Rgba32F: return 16;
        }
        return 4;
    }

    /// Decodes any image format this engine supports, honouring `options`.
    ///
    /// @param encoded `encoded` value used by the operation.
    /// @param options `options` value used by the operation.
    /// @param source Source value or resource, used only for error messages.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::DecodeFailure`.
    AssetExpected<DecodedImage> decode_image(
        std::span<const std::byte> encoded,
        const DecodeOptions &options,
        const std::filesystem::path &source) {
        const Foundation::Stopwatch stopwatch;

        AssetExpected<DecodedImage> image = [&]() -> AssetExpected<DecodedImage> {
            if (looks_like_ico(encoded)) {
                return decode_ico(encoded, options, source);
            }
            if (looks_like_webp(encoded)) {
                return decode_webp(encoded, options, source);
            }
            if (looks_like_avif(encoded)) {
                return decode_avif(encoded, options, source);
            }
            if (looks_like_jxl(encoded)) {
                return decode_jxl(encoded, options, source);
            }
            if (const std::optional<OPJ_CODEC_FORMAT> jp2_format = jp2_codec_format(encoded)) {
                return decode_jp2(encoded, *jp2_format, options, source);
            }
            if (looks_like_tiff(encoded)) {
                return decode_tiff(encoded, options, source);
            }
            if (looks_like_exr(encoded)) {
                return decode_exr(encoded, source);
            }
            if (looks_like_sgi(encoded)) {
                return decode_sgi(encoded, source);
            }
            if (looks_like_dpx(encoded)) {
                return decode_dpx(encoded, options, source);
            }
            if (looks_like_cineon(encoded)) {
                return decode_cineon(encoded, options, source);
            }
            // Checked last of the sniffers, and only when animation was actually asked for: an
            // APNG's default image decodes perfectly well through the ordinary still path, which
            // is both faster and exactly what a caller that did not ask for frames wants.
            if (options.decode_all_frames && looks_like_apng(encoded)) {
                return decode_apng(encoded, options, source);
            }
            return decode_with_stb(encoded, options, source);
        }();
        if (!image) {
            return image;
        }
        if (image->frames.empty()) {
            return std::unexpected(AssetError{
                .code = AssetErrorCode::DecodeFailure,
                .message = UString{"Image decoder produced no frames for '" + source.string() + "'."},
                .source = source,
            });
        }

        image->hdr = scan_png_hdr_metadata(encoded);
        if (image->hdr.present && image->transfer_function == 13) {
            // A decoder that already established its own transfer function (the AVIF/JXL/EXR
            // native-precision paths) is left alone; this only fills in the PNG case, where the
            // cICP chunk is the only place the information exists and stb_image does not look at
            // it. Doing it here rather than inside decode_with_stb keeps the chunk parsing in one
            // place for both the metadata field and the color information.
            image->color_primaries = image->hdr.color_primaries;
            image->transfer_function = image->hdr.transfer_function;
        }

        // The one place the decode leaves the file's own color space for the engine's. Decoders
        // above report what the source was; this converts it (or, for Native, deliberately does
        // not).
        convert_to_destination(*image, options.precision, options.gamut_mapping);

        if (image->full_width == 0) {
            image->full_width = image->width;
            image->full_height = image->height;
        }

        Foundation::log_info("ImageDecode: decoded '{}' ({}x{}{}, {} frame(s), {} encoded bytes) in {}",
                             source.string(),
                             image->width,
                             image->height,
                             image->is_preview ? " preview" : "",
                             image->frames.size(),
                             encoded.size(),
                             stopwatch.elapsed_human());
        return image;
    }

    /// Decodes the first frame of any supported image format to 8-bit sRGB RGBA.
    ///
    /// @param encoded `encoded` value used by the operation.
    /// @param source Source value or resource.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    AssetExpected<DecodedImage> decode_image_rgba8(
        std::span<const std::byte> encoded,
        const std::filesystem::path &source) {
        return decode_image(encoded, DecodeOptions{}, source);
    }

} // namespace SFT::Engine::Detail
