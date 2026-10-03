#pragma once

#include <Audio/Encode.hpp>
#include <Audio/Text.hpp>

#include <Foundation/FileIo.hpp>

#include <bit>
#include <cstring>

/// Pieces shared by the encoder backends. Internal: not part of the public API.
namespace SFT::Audio::detail {

    [[nodiscard]] constexpr u32 bytes_per_sample(SampleFormat f) noexcept {
        switch (f) {
            case SampleFormat::UInt8: return 1;
            case SampleFormat::Int16: return 2;
            case SampleFormat::Int24: return 3;
            case SampleFormat::Int32: return 4;
            case SampleFormat::Float32: return 4;
            case SampleFormat::Float64: return 8;
        }
        return 2;
    }

    [[nodiscard]] constexpr bool is_float(SampleFormat f) noexcept { return f == SampleFormat::Float32 || f == SampleFormat::Float64; }

    /// Converts float samples to a PCM byte stream: the one place sample formats, endianness and dither are handled, so
    /// WAV, Wave64, AIFF and raw output all agree.
    class PcmPacker {
      public:
        PcmPacker(SampleFormat format, bool big_endian, bool dither) : format_(format), big_endian_(big_endian), dither_(dither && (format == SampleFormat::UInt8 || format == SampleFormat::Int16)) {}

        [[nodiscard]] u32 sample_bytes() const noexcept { return bytes_per_sample(format_); }

        /// Appends `count` samples to `out`.
        void pack(const f32 *in, usize count, std::vector<std::byte> &out) {
            const usize base = out.size();
            out.resize(base + count * sample_bytes());
            std::byte *dst = out.data() + base;
            switch (format_) {
                case SampleFormat::UInt8:
                    for (usize i = 0; i < count; ++i) {
                        const f32 v = std::clamp(in[i] * 127.0f + dither_noise(1.0f), -128.0f, 127.0f);
                        dst[i] = static_cast<std::byte>(static_cast<u8>(static_cast<int>(std::lrint(v)) + 128));
                    }
                    break;
                case SampleFormat::Int16:
                    for (usize i = 0; i < count; ++i) {
                        const f32 v = std::clamp(in[i] * 32767.0f + dither_noise(1.0f), -32768.0f, 32767.0f);
                        store<u16>(dst + i * 2, static_cast<u16>(static_cast<i16>(std::lrint(v))));
                    }
                    break;
                case SampleFormat::Int24:
                    for (usize i = 0; i < count; ++i) {
                        const i32 v = static_cast<i32>(std::lrint(std::clamp(in[i], -1.0f, 1.0f) * 8388607.0f));
                        const u32 u = static_cast<u32>(v) & 0xFFFFFFu;
                        std::byte *p = dst + i * 3;
                        if (big_endian_) {
                            p[0] = static_cast<std::byte>(u >> 16); p[1] = static_cast<std::byte>(u >> 8); p[2] = static_cast<std::byte>(u);
                        } else {
                            p[0] = static_cast<std::byte>(u); p[1] = static_cast<std::byte>(u >> 8); p[2] = static_cast<std::byte>(u >> 16);
                        }
                    }
                    break;
                case SampleFormat::Int32:
                    for (usize i = 0; i < count; ++i) {
                        const f64 v = static_cast<f64>(std::clamp(in[i], -1.0f, 1.0f)) * 2147483647.0;
                        store<u32>(dst + i * 4, static_cast<u32>(static_cast<i32>(std::llrint(v))));
                    }
                    break;
                case SampleFormat::Float32:
                    for (usize i = 0; i < count; ++i) {
                        store<u32>(dst + i * 4, std::bit_cast<u32>(in[i]));
                    }
                    break;
                case SampleFormat::Float64:
                    for (usize i = 0; i < count; ++i) {
                        store<u64>(dst + i * 8, std::bit_cast<u64>(static_cast<f64>(in[i])));
                    }
                    break;
            }
        }

      private:
        template <typename T>
        void store(std::byte *dst, T value) const {
            if (big_endian_ == (std::endian::native == std::endian::big)) {
                std::memcpy(dst, &value, sizeof(T));
            } else {
                value = std::byteswap(value);
                std::memcpy(dst, &value, sizeof(T));
            }
        }

        // Triangular-PDF dither of +-1 LSB (two uniform noises); a tiny xorshift keeps it deterministic and allocation free.
        f32 dither_noise(f32 lsb) noexcept {
            if (!dither_) {
                return 0.0f;
            }
            const auto next = [this]() {
                state_ ^= state_ << 13;
                state_ ^= state_ >> 17;
                state_ ^= state_ << 5;
                return static_cast<f32>(state_ & 0xFFFFFFu) / 16777216.0f - 0.5f;
            };
            return (next() + next()) * lsb;
        }

        SampleFormat format_;
        bool big_endian_;
        bool dither_;
        u32 state_ = 0x9E3779B9u;
    };

    /// Tags as Vorbis comments (upper-case field names), plus the loop region as LOOPSTART/LOOPLENGTH so a loop survives a
    /// round trip through Ogg and FLAC.
    [[nodiscard]] inline std::vector<std::pair<UString, UString>> comment_fields(const EncodeOptions &options) {
        std::vector<std::pair<UString, UString>> fields;
        for (const auto &[key, value] : options.tags) {
            fields.emplace_back(ascii_upper(key), value);
        }
        if (options.loop && options.loop->end > options.loop->start) {
            fields.emplace_back("LOOPSTART", std::format("{}", options.loop->start));
            fields.emplace_back("LOOPLENGTH", std::format("{}", options.loop->end - options.loop->start));
        }
        return fields;
    }

    /// Shared state for backends: the output file and a sticky error.
    struct EncoderBase : AudioEncoder {
        [[nodiscard]] const UString &error() const override { return error_; }
        [[nodiscard]] u64 frames_written() const override { return frames_; }

      protected:
        /// Feeds interleaved audio to `encode(chunk, chunk_frames)` in bounded pieces and counts the frames. `encode` reports
        /// failures itself (through `fail`) and returns false to stop.
        template <class Encode>
        bool write_chunked(const f32 *interleaved, usize frames, u32 channels, Encode &&encode) {
            if (!error_.empty()) {
                return false;
            }
            constexpr usize chunk = 8192;
            for (usize done = 0; done < frames; done += chunk) {
                const usize n = std::min(chunk, frames - done);
                if (!encode(interleaved + done * channels, n)) {
                    return false;
                }
            }
            frames_ += frames;
            return true;
        }
        bool fail(UString message) {
            if (error_.empty()) {
                error_ = std::move(message);
            }
            return false;
        }
        UString error_;
        u64 frames_ = 0;
    };

} // namespace SFT::Audio::detail
