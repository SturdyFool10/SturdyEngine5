#pragma once

#include <Engine/Asset.hpp>
#include <Engine/ColorSpace.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace SFT::Engine::Detail {

    /// HDR metadata read from a PNG's `cICP`/`cLLI` chunks (PNG Third Edition's HDR PNG addition).
    ///
    /// `color_primaries`/`transfer_function` are raw ITU-T H.273 code points (the same values
    /// `cICP` stores), not re-encoded into an engine-specific enum — H.273 already has stable,
    /// widely-used numbering (16 = PQ, 18 = HLG, 1 = BT.709/sRGB primaries, ...) and re-deriving an
    /// equivalent enum here would just be one more thing to keep in sync with the spec for no
    /// benefit.
    struct PngHdrMetadata {
        bool present = false;
        u8 color_primaries = 2;   // ITU-T H.273; 2 = unspecified
        u8 transfer_function = 2; // ITU-T H.273; 2 = unspecified
        /// From `cLLI`, in cd/m^2. Zero means the chunk was absent or reported zero.
        u32 max_content_light_level = 0;
        u32 max_frame_average_light_level = 0;
    };

    /// Memory layout of `ImageFrame::pixels`. Every variant is four-channel RGBA, tightly packed
    /// (no row padding), top-to-bottom, in native byte order — the channel count and row packing
    /// are deliberately not variable, so a consumer only ever has to branch on component width.
    enum class PixelFormat : u8 {
        Rgba8,   ///< 4 bytes/pixel, unsigned normalized.
        Rgba16,  ///< 8 bytes/pixel, unsigned normalized.
        Rgba16F, ///< 8 bytes/pixel, IEEE 754 binary16.
        Rgba32F, ///< 16 bytes/pixel, IEEE 754 binary32.
    };

    /// Returns the number of bytes one pixel of `format` occupies.
    ///
    /// @param format `format` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] u32 bytes_per_pixel(PixelFormat format) noexcept;

    /// Where the decoder should land: how much precision to keep, and which color space to deliver.
    /// Each value names a complete, well-defined destination rather than a knob to combine.
    enum class DecodePrecision : u8 {
        /// Display-ready 8-bit sRGB: `PixelFormat::Rgba8`, sRGB transfer function, sRGB/BT.709
        /// primaries. A wide-gamut or HDR source is fully color-managed into that — its transfer
        /// curve inverted, its primaries converted with chromatic adaptation, and out-of-gamut
        /// colors mapped per `DecodeOptions::gamut_mapping`. What every caller that feeds an 8-bit
        /// `RGBA8Unorm`/`RGBA8UnormSrgb` GPU texture wants.
        Rgba8Srgb,
        /// The engine's working space: linear light in BT.709/sRGB primaries, at `Rgba16F` or
        /// wider so the linear values do not band and highlights above 1.0 survive. Same color
        /// management as `Rgba8Srgb` — the primaries end up identical — but nothing is clamped to
        /// the display range and nothing is re-encoded to a display curve. This is what a scene
        /// texture, an HDRI environment map, or anything the renderer will light with should be
        /// decoded as.
        SceneLinear,
        /// Whatever the file actually carries, with no color conversion at all: a 10-bit PQ AVIF
        /// stays 16-bit and PQ-encoded in BT.2020 primaries, an EXR stays half-float linear. The
        /// source's own encoding is reported through
        /// `DecodedImage::format`/`transfer_function`/`color_primaries`, and interpreting it is
        /// the caller's job. For tools that must round-trip or inspect a file exactly as authored.
        Native,
    };

    /// Knobs for `decode_image`. The defaults reproduce the historical
    /// `decode_image_rgba8` behaviour exactly: first frame only, 8-bit sRGB, full resolution.
    struct DecodeOptions {
        DecodePrecision precision = DecodePrecision::Rgba8Srgb;
        /// What to do with colors that fall outside the destination gamut when converting a
        /// wide-gamut source. Ignored by `DecodePrecision::Native`, which never converts, and by
        /// sources already in the working space, which never go out of gamut.
        GamutMapping gamut_mapping = GamutMapping::Desaturate;
        /// When true, an animated source (GIF, APNG, animated WebP, an AVIF image sequence, an
        /// animated JXL) yields every frame in `DecodedImage::frames` rather than just the first.
        /// Ignored by still formats, which always produce exactly one frame.
        bool decode_all_frames = false;
        /// When non-zero, ask the decoder for the cheapest approximation it can produce that is
        /// still at least this many pixels on both axes — a fast low-detail pass to show while the
        /// full-resolution decode runs. Codecs that support it natively (JPEG 2000's resolution
        /// levels, JXL's progressive DC frame) produce it without decoding the full image; every
        /// other format ignores the hint and decodes normally, which is a valid outcome rather
        /// than an error. Check `DecodedImage::is_preview` to find out which happened.
        u32 preview_min_dimension = 0;
    };

    /// One frame of a decoded image. A still image decodes to exactly one of these.
    struct ImageFrame {
        std::vector<std::byte> pixels;
        /// How long this frame is shown, in milliseconds. Zero for a still image, and for the
        /// single frame produced when `DecodeOptions::decode_all_frames` is false.
        u32 duration_ms = 0;
    };

    struct DecodedImage {
        u32 width = 0;
        u32 height = 0;
        PixelFormat format = PixelFormat::Rgba8;
        /// ITU-T H.273 code points describing what `frames`' samples actually mean. Defaults
        /// describe plain SDR sRGB, which is what a source carrying no color information at all
        /// is conventionally assumed to be. `DecodePrecision::Rgba8Srgb` always reports sRGB
        /// primaries with the sRGB transfer function and `SceneLinear` always reports sRGB
        /// primaries with the linear transfer function, because both convert to get there; only
        /// `Native` ever reports something else.
        u8 color_primaries = 1;   ///< 1 = BT.709 (the sRGB primaries).
        u8 transfer_function = 13; ///< 13 = sRGB.
        /// Frames in presentation order; never empty on a successful decode.
        std::vector<ImageFrame> frames;
        /// How many times an animation repeats. Zero means loop forever, which is both the
        /// GIF/WebP/APNG encoding of "infinite" and the right answer for a still image.
        u32 loop_count = 0;
        /// True when `DecodeOptions::preview_min_dimension` caused a reduced-detail decode, in
        /// which case `width`/`height` are the *preview's* size and `full_width`/`full_height`
        /// are the source's real size.
        bool is_preview = false;
        u32 full_width = 0;
        u32 full_height = 0;
        /// Only ever set for a PNG source; default-constructed (`present == false`) otherwise.
        PngHdrMetadata hdr;

        /// The first frame's pixels — the whole image, for the still images that are the common
        /// case. Undefined behaviour when `frames` is empty, which a successful decode never
        /// leaves it.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::vector<std::byte> &pixels() noexcept { return frames.front().pixels; }
        [[nodiscard]] const std::vector<std::byte> &pixels() const noexcept { return frames.front().pixels; }
    };

    /// Decodes any image format this engine supports, honouring `options`.
    ///
    /// @param encoded `encoded` value used by the operation.
    /// @param options `options` value used by the operation.
    /// @param source Source value or resource, used only for error messages.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] AssetExpected<DecodedImage> decode_image(
        std::span<const std::byte> encoded,
        const DecodeOptions &options,
        const std::filesystem::path &source);

    /// Decodes the first frame of any supported image format to 8-bit sRGB RGBA — `decode_image`
    /// with default `DecodeOptions`, kept as its own name because it is what almost every caller
    /// in this engine wants.
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
