/// Progressive JPEG XL decoding on real files: libjxl encodes multi-group images here (a 1024x768 photo-like gradient with
/// noise, lossy VarDCT with and without progressive AC, and lossless modular), and `decode_image_progressive` must
///   * report the image several times, always at full size, ending with exactly one `is_final` call;
///   * improve: the first report differs from the last, and reports in between move towards it (the share of pixels that
///     already equal the final image never goes down);
///   * finish with the same pixels a plain decode gives;
///   * stop when the sink says so.

#include <Engine/ImageDecode.hpp>

#include <jxl/encode.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {
    using SFT::u32;
    using SFT::usize;
    namespace Engine = SFT::Engine;

    u32 kWidth = 1024;
    u32 kHeight = 768;

    std::vector<std::uint8_t> make_image() {
        std::vector<std::uint8_t> pixels(static_cast<usize>(kWidth) * kHeight * 4);
        std::uint32_t state = 12345u;
        for (u32 y = 0; y < kHeight; ++y) {
            for (u32 x = 0; x < kWidth; ++x) {
                state = state * 1664525u + 1013904223u;
                const int noise = static_cast<int>((state >> 24) & 0x1F) - 16;
                std::uint8_t *p = pixels.data() + (static_cast<usize>(y) * kWidth + x) * 4;
                p[0] = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(x * 255 / kWidth) + noise, 0, 255));
                p[1] = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(y * 255 / kHeight) + noise, 0, 255));
                p[2] = static_cast<std::uint8_t>(std::clamp<int>(((x / 64 + y / 64) % 2) * 160 + 40 + noise, 0, 255));
                p[3] = 255;
            }
        }
        return pixels;
    }

    enum class Mode { VarDct, VarDctProgressive, Lossless };

    std::vector<std::byte> encode(const std::vector<std::uint8_t> &pixels, Mode mode) {
        auto encoder = std::unique_ptr<JxlEncoder, decltype(&JxlEncoderDestroy)>(JxlEncoderCreate(nullptr), &JxlEncoderDestroy);
        JxlBasicInfo info;
        JxlEncoderInitBasicInfo(&info);
        info.xsize = kWidth;
        info.ysize = kHeight;
        info.bits_per_sample = 8;
        info.num_color_channels = 3;
        info.num_extra_channels = 1;
        info.alpha_bits = 8;
        info.uses_original_profile = mode == Mode::Lossless ? JXL_TRUE : JXL_FALSE;
        assert(JxlEncoderSetBasicInfo(encoder.get(), &info) == JXL_ENC_SUCCESS);
        JxlColorEncoding color;
        JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
        assert(JxlEncoderSetColorEncoding(encoder.get(), &color) == JXL_ENC_SUCCESS);
        JxlEncoderFrameSettings *settings = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
        if (mode == Mode::Lossless) {
            assert(JxlEncoderSetFrameLossless(settings, JXL_TRUE) == JXL_ENC_SUCCESS);
        } else {
            assert(JxlEncoderSetFrameDistance(settings, 1.0f) == JXL_ENC_SUCCESS);
        }
        if (mode == Mode::VarDctProgressive) {
            JxlEncoderFrameSettingsSetOption(settings, JXL_ENC_FRAME_SETTING_PROGRESSIVE_AC, 1);
            JxlEncoderFrameSettingsSetOption(settings, JXL_ENC_FRAME_SETTING_PROGRESSIVE_DC, 1);
        }
        // Several groups (the default 256x256 here is 4x3 of them), whatever the mode.
        JxlPixelFormat format{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
        assert(JxlEncoderAddImageFrame(settings, &format, pixels.data(), pixels.size()) == JXL_ENC_SUCCESS);
        JxlEncoderCloseInput(encoder.get());
        std::vector<std::byte> out(64 * 1024);
        std::uint8_t *next = reinterpret_cast<std::uint8_t *>(out.data());
        usize available = out.size();
        JxlEncoderStatus status;
        while ((status = JxlEncoderProcessOutput(encoder.get(), &next, &available)) == JXL_ENC_NEED_MORE_OUTPUT) {
            const usize used = static_cast<usize>(next - reinterpret_cast<std::uint8_t *>(out.data()));
            out.resize(out.size() * 2);
            next = reinterpret_cast<std::uint8_t *>(out.data()) + used;
            available = out.size() - used;
        }
        assert(status == JXL_ENC_SUCCESS);
        out.resize(static_cast<usize>(next - reinterpret_cast<std::uint8_t *>(out.data())));
        return out;
    }

    /// Share of the pixels of `shown` equal to `final_pixels`.
    double equal_share(std::span<const std::byte> shown, const std::vector<std::byte> &final_pixels) {
        usize equal = 0;
        const usize count = final_pixels.size() / 4;
        for (usize i = 0; i < count; ++i) {
            equal += std::memcmp(shown.data() + i * 4, final_pixels.data() + i * 4, 4) == 0 ? 1 : 0;
        }
        return static_cast<double>(equal) / static_cast<double>(count);
    }

    void run(const char *name, Mode mode, const std::vector<std::uint8_t> &source) {
        const std::vector<std::byte> encoded = encode(source, mode);
        std::printf("%s: %zu encoded bytes\n", name, encoded.size());

        auto reference = Engine::decode_image_rgba8(encoded, {});
        assert(reference.has_value());
        assert(reference->width == kWidth && reference->height == kHeight);

        std::vector<std::vector<std::byte>> reports;
        u32 final_calls = 0;
        auto result = Engine::decode_image_progressive(
            encoded, Engine::DecodeOptions{},
            [&](u32 width, u32 height, std::span<const std::byte> rgba8, bool is_final) {
                assert(width == kWidth && height == kHeight);
                assert(rgba8.size() == static_cast<usize>(width) * height * 4);
                assert(final_calls == 0); // nothing after the final call
                final_calls += is_final ? 1u : 0u;
                if (!is_final) reports.emplace_back(rgba8.begin(), rgba8.end());
                return true;
            },
            {});
        assert(result.has_value());
        assert(final_calls == 1);
        assert(result->pixels() == reference->pixels());

        std::printf("%s: %zu intermediate reports\n", name, reports.size());
        assert(reports.size() >= 3);
        double previous = -1.0;
        for (const auto &report : reports) {
            const double share = equal_share(report, reference->pixels());
            std::printf("  %.3f of pixels already final\n", share);
            assert(share + 1e-9 >= previous);
            previous = share;
        }
        // It really was a work in progress: the first report is not the finished image.
        assert(equal_share(reports.front(), reference->pixels()) < 0.999);

        u32 calls = 0;
        auto stopped = Engine::decode_image_progressive(
            encoded, Engine::DecodeOptions{}, [&](u32, u32, std::span<const std::byte>, bool) { return ++calls < 2; }, {});
        assert(!stopped.has_value());
        assert(calls == 2);
    }
} // namespace

int main() {
    // A flat, tiny-bitstream image (a few dozen bytes for 512x512): nothing to refine, but it must still decode and report.
    {
        kWidth = 512;
        kHeight = 512;
        std::vector<std::uint8_t> flat(static_cast<usize>(kWidth) * kHeight * 4, 128);
        for (usize i = 3; i < flat.size(); i += 4) flat[i] = 255;
        for (Mode mode : {Mode::VarDct, Mode::VarDctProgressive, Mode::Lossless}) {
            const std::vector<std::byte> encoded = encode(flat, mode);
            std::printf("flat: %zu encoded bytes\n", encoded.size());
            auto reference = Engine::decode_image_rgba8(encoded, {});
            assert(reference.has_value());
            u32 finals = 0;
            auto result = Engine::decode_image_progressive(
                encoded, Engine::DecodeOptions{},
                [&](u32, u32, std::span<const std::byte>, bool is_final) { finals += is_final ? 1u : 0u; return true; }, {});
            assert(result.has_value());
            assert(finals == 1);
            assert(result->pixels() == reference->pixels());
        }
        kWidth = 1024;
        kHeight = 768;
    }
    const std::vector<std::uint8_t> source = make_image();
    run("vardct", Mode::VarDct, source);
    run("vardct-progressive", Mode::VarDctProgressive, source);
    run("lossless", Mode::Lossless, source);
    std::puts("JxlProgressiveDecodeTest passed");
    return 0;
}
