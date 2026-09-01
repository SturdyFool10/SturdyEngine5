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

#include <webp/decode.h>
#include <webp/demux.h>

#include <avif/avif.h>

#include <jxl/decode.h>

#include <openjpeg.h>

#include <tiffio.h>

#include <ImfIO.h>
#include <ImfRgbaFile.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace SFT::Engine::Detail {

    namespace {

        /// Decodes `encoded` through stb_image — every format stb_image itself recognizes by
        /// magic bytes (JPEG, PNG, BMP, GIF, PSD, HDR, PIC, PNM), plus the PNG-backed case of an
        /// ICO/CUR entry, which is a normal PNG at that point.
        ///
        /// @param encoded `encoded` value used by the operation.
        /// @param source Source value or resource, used only for error messages.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<DecodedImage> decode_with_stb(std::span<const std::byte> encoded,
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

                DecodedImage result{
                    .width = static_cast<u32>(width),
                    .height = static_cast<u32>(height),
                    .pixels = std::vector<std::byte>(static_cast<usize>(width) * static_cast<usize>(height) * 4),
                };
                for (usize i = 0; i < scanlines.size(); ++i) {
                    const Imf::Rgba &pixel = scanlines[i];
                    result.pixels[i * 4 + 0] = std::byte{linear_to_srgb_u8(static_cast<float>(pixel.r))};
                    result.pixels[i * 4 + 1] = std::byte{linear_to_srgb_u8(static_cast<float>(pixel.g))};
                    result.pixels[i * 4 + 2] = std::byte{linear_to_srgb_u8(static_cast<float>(pixel.b))};
                    // Alpha is not a light value: linear-to-sRGB encoding does not apply, just clamp.
                    result.pixels[i * 4 + 3] =
                        std::byte{static_cast<u8>(std::clamp(static_cast<float>(pixel.a), 0.0f, 1.0f) * 255.0f + 0.5f)};
                }
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

            DecodedImage result{
                .width = width,
                .height = height,
                .pixels = std::vector<std::byte>(static_cast<usize>(width) * height * 4),
            };
            const int decoded_ok = TIFFReadRGBAImageOriented(
                tiff, width, height, reinterpret_cast<uint32_t *>(result.pixels.data()), ORIENTATION_TOPLEFT, 0);
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
            static constexpr std::byte kPngMagic[8] = {
                std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'},
                std::byte{'\r'}, std::byte{'\n'}, std::byte{0x1A}, std::byte{'\n'},
            };
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
                                                               const std::filesystem::path &source) {
            const auto fail = [&source](const char *reason) {
                return std::unexpected(AssetError{
                    .code = AssetErrorCode::DecodeFailure,
                    .message = UString{std::string{"Could not decode WebP: "} + reason},
                    .source = source,
                });
            };

            const auto *bytes = reinterpret_cast<const uint8_t *>(encoded.data());
            int width = 0;
            int height = 0;
            uint8_t *decoded = WebPDecodeRGBA(bytes, encoded.size(), &width, &height);

            std::vector<std::byte> frame_bytes; // keeps `bytes`/`encoded` alive across the fallback below
            if (decoded == nullptr) {
                WebPData data{bytes, encoded.size()};
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
                .pixels = std::vector<std::byte>(byte_count),
            };
            std::memcpy(image.pixels.data(), decoded, byte_count);
            WebPFree(decoded);
            return image;
        }

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

            avifResult result = avifDecoderSetIOMemory(
                decoder, reinterpret_cast<const uint8_t *>(encoded.data()), encoded.size());
            if (result == AVIF_RESULT_OK) {
                result = avifDecoderParse(decoder);
            }
            if (result == AVIF_RESULT_OK) {
                result = avifDecoderNextImage(decoder);
            }
            if (result != AVIF_RESULT_OK) {
                const char *reason = avifResultToString(result);
                avifDecoderDestroy(decoder);
                return fail(reason != nullptr ? reason : "unknown decode failure.");
            }

            avifRGBImage rgb;
            avifRGBImageSetDefaults(&rgb, decoder->image);
            rgb.format = AVIF_RGB_FORMAT_RGBA;
            rgb.depth = 8;
            if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK) {
                avifDecoderDestroy(decoder);
                return fail("failed to allocate the RGBA conversion buffer.");
            }
            result = avifImageYUVToRGB(decoder->image, &rgb);
            if (result != AVIF_RESULT_OK) {
                const char *reason = avifResultToString(result);
                avifRGBImageFreePixels(&rgb);
                avifDecoderDestroy(decoder);
                return fail(reason != nullptr ? reason : "YUV-to-RGB conversion failed.");
            }

            // rgb.rowBytes may exceed width * 4 (row padding avifRGBImageAllocatePixels can add),
            // so each row is copied separately rather than assuming a tightly packed buffer.
            DecodedImage image{
                .width = rgb.width,
                .height = rgb.height,
                .pixels = std::vector<std::byte>(static_cast<usize>(rgb.width) * rgb.height * 4),
            };
            const usize dest_row_bytes = static_cast<usize>(rgb.width) * 4;
            for (u32 y = 0; y < rgb.height; ++y) {
                std::memcpy(image.pixels.data() + static_cast<usize>(y) * dest_row_bytes,
                           rgb.pixels + static_cast<usize>(y) * rgb.rowBytes, dest_row_bytes);
            }

            avifRGBImageFreePixels(&rgb);
            avifDecoderDestroy(decoder);
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

        /// Decodes a still JPEG XL image (the first frame, for a JXL animation).
        ///
        /// One-shot decode: the whole encoded buffer is handed over up front and
        /// `JxlDecoderCloseInput` immediately marks it as everything there is, so
        /// `JXL_DEC_NEED_MORE_INPUT` never legitimately occurs here — the caller already loaded the
        /// complete file, unlike a streaming decoder reading off a socket.
        ///
        /// @param encoded `encoded` value used by the operation.
        /// @param source Source value or resource, used only for error messages.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<DecodedImage> decode_jxl(std::span<const std::byte> encoded,
                                                              const std::filesystem::path &source) {
            const auto fail = [&source](const char *reason) {
                return std::unexpected(AssetError{
                    .code = AssetErrorCode::DecodeFailure,
                    .message = UString{std::string{"Could not decode JPEG XL: "} + reason},
                    .source = source,
                });
            };

            JxlDecoder *decoder = JxlDecoderCreate(nullptr);
            if (decoder == nullptr) {
                return fail("failed to create the JPEG XL decoder.");
            }

            // JXL_DEC_NEED_IMAGE_OUT_BUFFER is a JxlDecoderProcessInput status code (a small
            // sequential value, 5), not an "informative event" bitmask like the two below (values
            // >= 0x40) -- it is not subscribable, and OR-ing its bit pattern in here corrupts the
            // mask and makes JxlDecoderSubscribeEvents itself fail. It is still reported
            // automatically by JxlDecoderProcessInput at the right point regardless of subscription.
            if (JxlDecoderSubscribeEvents(decoder, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) !=
                JXL_DEC_SUCCESS) {
                JxlDecoderDestroy(decoder);
                return fail("failed to subscribe to decode events.");
            }
            if (JxlDecoderSetInput(decoder, reinterpret_cast<const uint8_t *>(encoded.data()),
                                   encoded.size()) != JXL_DEC_SUCCESS) {
                JxlDecoderDestroy(decoder);
                return fail("failed to set decode input.");
            }
            JxlDecoderCloseInput(decoder);

            const JxlPixelFormat format{
                .num_channels = 4,
                .data_type = JXL_TYPE_UINT8,
                .endianness = JXL_NATIVE_ENDIAN,
                .align = 0,
            };
            JxlBasicInfo info{};
            std::vector<std::byte> pixels;
            bool got_full_image = false;

            for (;;) {
                const JxlDecoderStatus status = JxlDecoderProcessInput(decoder);
                if (status == JXL_DEC_ERROR) {
                    JxlDecoderDestroy(decoder);
                    return fail("the bitstream is corrupt or uses an unsupported feature.");
                }
                if (status == JXL_DEC_NEED_MORE_INPUT) {
                    // Cannot legitimately happen for a one-shot decode of a complete buffer (see
                    // this function's own doc comment) -- treated as a decode failure rather than
                    // looping forever.
                    JxlDecoderDestroy(decoder);
                    return fail("the file is truncated.");
                }
                if (status == JXL_DEC_BASIC_INFO) {
                    if (JxlDecoderGetBasicInfo(decoder, &info) != JXL_DEC_SUCCESS) {
                        JxlDecoderDestroy(decoder);
                        return fail("failed to read image dimensions.");
                    }
                    continue;
                }
                if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                    size_t buffer_size = 0;
                    if (JxlDecoderImageOutBufferSize(decoder, &format, &buffer_size) != JXL_DEC_SUCCESS) {
                        JxlDecoderDestroy(decoder);
                        return fail("failed to determine the output buffer size.");
                    }
                    pixels.assign(buffer_size, std::byte{0});
                    if (JxlDecoderSetImageOutBuffer(decoder, &format, pixels.data(), pixels.size()) !=
                        JXL_DEC_SUCCESS) {
                        JxlDecoderDestroy(decoder);
                        return fail("failed to set the output buffer.");
                    }
                    continue;
                }
                if (status == JXL_DEC_FULL_IMAGE) {
                    // A JXL animation reports one FULL_IMAGE per frame, each overwriting `pixels`
                    // via its own NEED_IMAGE_OUT_BUFFER request -- stopping at the first is both
                    // the correct "decode a still image" behavior and avoids decoding every frame
                    // of a large animation just to throw all but the last away.
                    got_full_image = true;
                    break;
                }
                if (status == JXL_DEC_SUCCESS) {
                    break;
                }
                // Any other subscribed-but-unhandled event: ignored and looped past.
            }

            JxlDecoderDestroy(decoder);
            if (!got_full_image || info.xsize == 0 || info.ysize == 0 ||
                pixels.size() != static_cast<usize>(info.xsize) * info.ysize * 4) {
                return fail("no full image was decoded.");
            }

            return DecodedImage{
                .width = info.xsize,
                .height = info.ysize,
                .pixels = std::move(pixels),
            };
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

            opj_image_t *image = nullptr;
            const bool decoded_ok = opj_read_header(stream, codec, &image) != OPJ_FALSE &&
                                    opj_decode(codec, stream, image) != OPJ_FALSE &&
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

            const u32 width = image->x1 - image->x0;
            const u32 height = image->y1 - image->y0;
            DecodedImage result{
                .width = width,
                .height = height,
                .pixels = std::vector<std::byte>(static_cast<usize>(width) * height * 4),
            };
            for (u32 y = 0; y < height; ++y) {
                for (u32 x = 0; x < width; ++x) {
                    const usize dest = (static_cast<usize>(y) * width + x) * 4;
                    if (is_grayscale) {
                        const u8 gray = opj_sample_u8(image->comps[0], x, y);
                        result.pixels[dest + 0] = std::byte{gray};
                        result.pixels[dest + 1] = std::byte{gray};
                        result.pixels[dest + 2] = std::byte{gray};
                        result.pixels[dest + 3] = std::byte{255};
                    } else {
                        result.pixels[dest + 0] = std::byte{opj_sample_u8(image->comps[0], x, y)};
                        result.pixels[dest + 1] = std::byte{opj_sample_u8(image->comps[1], x, y)};
                        result.pixels[dest + 2] = std::byte{opj_sample_u8(image->comps[2], x, y)};
                        result.pixels[dest + 3] =
                            image->numcomps >= 4 ? std::byte{opj_sample_u8(image->comps[3], x, y)} : std::byte{255};
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

            static constexpr std::byte kPngMagic[8] = {
                std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'},
                std::byte{'\r'}, std::byte{'\n'}, std::byte{0x1A}, std::byte{'\n'},
            };
            if (image.size() >= 8 && std::equal(std::begin(kPngMagic), std::end(kPngMagic), image.begin())) {
                return decode_with_stb(image, source);
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
                .pixels = std::vector<std::byte>(static_cast<usize>(width) * height * 4),
            };
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
                    result.pixels[dest + 0] = std::byte{r};
                    result.pixels[dest + 1] = std::byte{g};
                    result.pixels[dest + 2] = std::byte{b};
                    result.pixels[dest + 3] = std::byte{a};
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
                .pixels = std::vector<std::byte>(plane_size * 4),
            };
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
                    result.pixels[dest + 0] = std::byte{r};
                    result.pixels[dest + 1] = std::byte{g};
                    result.pixels[dest + 2] = std::byte{b};
                    result.pixels[dest + 3] = std::byte{a};
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

            DecodedImage result{
                .width = width,
                .height = height,
                .pixels = std::vector<std::byte>(static_cast<usize>(width) * height * 4),
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
                        const u8 r = static_cast<u8>(((word >> 22) & 0x3FFu) >> 2);
                        const u8 g = static_cast<u8>(((word >> 12) & 0x3FFu) >> 2);
                        const u8 b = static_cast<u8>(((word >> 2) & 0x3FFu) >> 2);
                        const usize dest = (static_cast<usize>(y) * width + x) * 4;
                        result.pixels[dest + 0] = std::byte{r};
                        result.pixels[dest + 1] = std::byte{g};
                        result.pixels[dest + 2] = std::byte{b};
                        result.pixels[dest + 3] = std::byte{255};
                    }
                }
            } else {
                const u32 bytes_per_sample = bit_size == 8 ? 1u : 2u;
                const usize unpadded_row_bytes = static_cast<usize>(width) * channels * bytes_per_sample;
                const usize row_bytes = ((unpadded_row_bytes + 3) / 4) * 4 + line_padding;
                if (encoded.size() < static_cast<usize>(image_data_offset) + row_bytes * height) {
                    return fail("pixel data is truncated.");
                }
                for (u32 y = 0; y < height; ++y) {
                    const usize row_offset = image_data_offset + static_cast<usize>(y) * row_bytes;
                    for (u32 x = 0; x < width; ++x) {
                        const usize pixel_offset = row_offset + static_cast<usize>(x) * channels * bytes_per_sample;
                        const auto sample = [&](u32 c) noexcept -> u8 {
                            if (bit_size == 8) return std::to_integer<u8>(encoded[pixel_offset + c]);
                            return static_cast<u8>(read_u16(pixel_offset + static_cast<usize>(c) * 2) >> 8);
                        };
                        const usize dest = (static_cast<usize>(y) * width + x) * 4;
                        result.pixels[dest + 0] = std::byte{sample(0)};
                        result.pixels[dest + 1] = std::byte{sample(1)};
                        result.pixels[dest + 2] = std::byte{sample(2)};
                        result.pixels[dest + 3] = has_alpha ? std::byte{sample(3)} : std::byte{255};
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

        /// Converts a 10-bit Cineon/DPX printing-density log code (the format both were designed
        /// around, predating either scene-linear or video formats in VFX) to an 8-bit sRGB-encoded
        /// display value. Uses the standard default Kodak Cineon calibration (black point code 95,
        /// white point code 685, 0.002 density units per code, 0.6 print-film gamma) rather than
        /// any per-file calibration — this file carries reference black/white *density* values per
        /// channel, but not the code-to-density step size itself, so a real colorist's LUT/CDL is
        /// what actually calibrates this in a film pipeline. Verified against real encoder output:
        /// a pure red source pixel decodes to log codes (684, 95, 95) with these constants, which
        /// round-trips back to approximately full-scale red. Same "decode correctly, defer real
        /// tone-mapping pipeline integration" stance as the EXR and PNG-HDR decoders in this file.
        [[nodiscard]] u8 cineon_log_to_srgb_u8(u32 code10) noexcept {
            constexpr float kBlackPoint = 95.0f;
            constexpr float kWhitePoint = 685.0f;
            constexpr float kGamma = 0.6f;
            constexpr float kStep = 0.002f;
            const float black_linear = std::pow(10.0f, (kBlackPoint - kWhitePoint) * kStep / kGamma);
            const float density = (static_cast<float>(code10) - kWhitePoint) * kStep / kGamma;
            const float linear = std::pow(10.0f, density);
            const float normalized = std::clamp((linear - black_linear) / (1.0f - black_linear), 0.0f, 1.0f);
            return linear_to_srgb_u8(normalized);
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

            DecodedImage result{
                .width = width,
                .height = height,
                .pixels = std::vector<std::byte>(static_cast<usize>(width) * height * 4),
            };
            for (u32 y = 0; y < height; ++y) {
                const usize row_offset = image_data_offset + static_cast<usize>(y) * row_bytes;
                for (u32 x = 0; x < width; ++x) {
                    const u32 word = read_u32(row_offset + static_cast<usize>(x) * 4);
                    const u32 r10 = (word >> 22) & 0x3FFu;
                    const u32 g10 = (word >> 12) & 0x3FFu;
                    const u32 b10 = (word >> 2) & 0x3FFu;
                    const usize dest = (static_cast<usize>(y) * width + x) * 4;
                    result.pixels[dest + 0] = std::byte{cineon_log_to_srgb_u8(r10)};
                    result.pixels[dest + 1] = std::byte{cineon_log_to_srgb_u8(g10)};
                    result.pixels[dest + 2] = std::byte{cineon_log_to_srgb_u8(b10)};
                    result.pixels[dest + 3] = std::byte{255};
                }
            }
            return result;
        }

    } // namespace

    /// Decodes image rgba8.
    ///
    /// @param encoded `encoded` value used by the operation.
    /// @param source Source value or resource.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::DecodeFailure`.
    AssetExpected<DecodedImage> decode_image_rgba8(
        std::span<const std::byte> encoded,
        const std::filesystem::path &source) {
        const Foundation::Stopwatch stopwatch;

        AssetExpected<DecodedImage> image = [&]() -> AssetExpected<DecodedImage> {
            if (looks_like_ico(encoded)) {
                return decode_ico(encoded, source);
            }
            if (looks_like_webp(encoded)) {
                return decode_webp(encoded, source);
            }
            if (looks_like_avif(encoded)) {
                return decode_avif(encoded, source);
            }
            if (looks_like_jxl(encoded)) {
                return decode_jxl(encoded, source);
            }
            if (const std::optional<OPJ_CODEC_FORMAT> jp2_format = jp2_codec_format(encoded)) {
                return decode_jp2(encoded, *jp2_format, source);
            }
            if (looks_like_tiff(encoded)) {
                return decode_tiff(encoded, source);
            }
            if (looks_like_exr(encoded)) {
                return decode_exr(encoded, source);
            }
            if (looks_like_sgi(encoded)) {
                return decode_sgi(encoded, source);
            }
            if (looks_like_dpx(encoded)) {
                return decode_dpx(encoded, source);
            }
            if (looks_like_cineon(encoded)) {
                return decode_cineon(encoded, source);
            }
            return decode_with_stb(encoded, source);
        }();
        if (!image) {
            return image;
        }

        image->hdr = scan_png_hdr_metadata(encoded);
        if (image->hdr.present) {
            // Not yet applied: the pixels above are always 8-bit SDR regardless of what cICP
            // reports (see PngHdrMetadata's own doc comment). Logged so real HDR content silently
            // rendering as SDR is at least visible, not a mystery.
            Foundation::log_warn(
                "ImageDecode: '{}' carries HDR PNG metadata (cICP transfer_function={}, "
                "color_primaries={}) that is not yet applied — decoded as 8-bit SDR.",
                source.string(), image->hdr.transfer_function, image->hdr.color_primaries);
        }

        Foundation::log_info("ImageDecode: decoded '{}' ({}x{}, {} encoded bytes) in {}",
                             source.string(),
                             image->width,
                             image->height,
                             encoded.size(),
                             stopwatch.elapsed_human());
        return image;
    }

} // namespace SFT::Engine::Detail
