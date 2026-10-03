#include <Audio/Decoder.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace SFT::Audio {

    namespace {

        u32 be32(const std::byte *p) {
            return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
        }
        u16 be16(const std::byte *p) { return static_cast<u16>((static_cast<u16>(p[0]) << 8) | static_cast<u16>(p[1])); }

        // 80-bit IEEE extended (sample rate) to double.
        f64 extended_to_double(const std::byte *p) {
            const u16 exponent_sign = be16(p);
            const u64 mantissa = (static_cast<u64>(be32(p + 2)) << 32) | be32(p + 6);
            const int exponent = static_cast<int>(exponent_sign & 0x7FFF);
            if (exponent == 0 && mantissa == 0) {
                return 0.0;
            }
            const f64 value = std::ldexp(static_cast<f64>(mantissa), exponent - 16383 - 63);
            return (exponent_sign & 0x8000) ? -value : value;
        }

        enum class Encoding { PcmBigEndian, PcmLittleEndian, Float32, Float64 };

        class AiffDecoder final : public StreamDecoder {
          public:
            [[nodiscard]] bool open(EncodedBytes bytes) {
                bytes_ = std::move(bytes);
                const std::byte *d = bytes_->data();
                const usize size = bytes_->size();
                if (size < 12 || std::memcmp(d, "FORM", 4) != 0) {
                    return false;
                }
                const bool aifc = std::memcmp(d + 8, "AIFC", 4) == 0;
                if (!aifc && std::memcmp(d + 8, "AIFF", 4) != 0) {
                    return false;
                }
                bool have_comm = false, have_ssnd = false;
                usize offset = 12;
                while (offset + 8 <= size) {
                    const std::byte *chunk = d + offset;
                    const u32 chunk_size = be32(chunk + 4);
                    const usize body = offset + 8;
                    if (body + chunk_size > size) {
                        break;
                    }
                    if (std::memcmp(chunk, "COMM", 4) == 0 && chunk_size >= 18) {
                        info_.channels = be16(d + body);
                        info_.total_frames = be32(d + body + 2);
                        bits_ = be16(d + body + 6);
                        info_.sample_rate = static_cast<u32>(std::lround(extended_to_double(d + body + 8)));
                        encoding_ = Encoding::PcmBigEndian;
                        info_.codec = "aiff";
                        if (aifc && chunk_size >= 22) {
                            const char *type = reinterpret_cast<const char *>(d + body + 18);
                            if (std::memcmp(type, "NONE", 4) == 0 || std::memcmp(type, "twos", 4) == 0 || std::memcmp(type, "in24", 4) == 0 ||
                                std::memcmp(type, "in32", 4) == 0) {
                                encoding_ = Encoding::PcmBigEndian;
                            } else if (std::memcmp(type, "sowt", 4) == 0) {
                                encoding_ = Encoding::PcmLittleEndian;
                            } else if (std::memcmp(type, "fl32", 4) == 0 || std::memcmp(type, "FL32", 4) == 0) {
                                encoding_ = Encoding::Float32;
                                bits_ = 32;
                            } else if (std::memcmp(type, "fl64", 4) == 0 || std::memcmp(type, "FL64", 4) == 0) {
                                encoding_ = Encoding::Float64;
                                bits_ = 64;
                            } else {
                                return false; // ulaw/alaw/ima4 and friends are not handled here
                            }
                            info_.codec = "aifc";
                        }
                        have_comm = true;
                    } else if (std::memcmp(chunk, "SSND", 4) == 0 && chunk_size >= 8) {
                        const u32 data_offset = be32(d + body);
                        data_begin_ = body + 8 + data_offset;
                        data_bytes_ = chunk_size >= 8 + data_offset ? chunk_size - 8 - data_offset : 0;
                        have_ssnd = true;
                    } else if (std::memcmp(chunk, "MARK", 4) == 0 && chunk_size >= 2) {
                        usize p = body + 2;
                        const u16 count = be16(d + body);
                        for (u16 i = 0; i < count && p + 7 <= body + chunk_size; ++i) {
                            const u32 position = be32(d + p + 2);
                            const u8 name_length = static_cast<u8>(d[p + 6]);
                            if (p + 7 + name_length > body + chunk_size) {
                                break;
                            }
                            info_.markers.push_back(AudioMarker{text_from_bytes(std::string_view{reinterpret_cast<const char *>(d + p + 7), name_length}), position});
                            p += 7 + name_length + ((name_length + 1) & 1u ? 1u : 0u);
                        }
                    }
                    offset = body + chunk_size + (chunk_size & 1u);
                }
                if (!have_comm || !have_ssnd || info_.channels == 0 || info_.sample_rate == 0 || bits_ == 0) {
                    return false;
                }
                bytes_per_sample_ = (bits_ + 7) / 8;
                frame_bytes_ = static_cast<usize>(bytes_per_sample_) * info_.channels;
                const u64 stored = frame_bytes_ > 0 ? data_bytes_ / frame_bytes_ : 0;
                info_.total_frames = std::min<u64>(info_.total_frames, stored);
                std::sort(info_.markers.begin(), info_.markers.end(), [](const AudioMarker &a, const AudioMarker &b) { return a.frame < b.frame; });
                return true;
            }

            [[nodiscard]] const AudioStreamInfo &info() const override { return info_; }

            u64 read(f32 *out, u64 frames) override {
                const u64 count = std::min<u64>(frames, info_.total_frames - std::min(info_.total_frames, position_));
                const std::byte *p = bytes_->data() + data_begin_ + position_ * frame_bytes_;
                const usize samples = static_cast<usize>(count) * info_.channels;
                for (usize i = 0; i < samples; ++i, p += bytes_per_sample_) {
                    out[i] = decode(p);
                }
                position_ += count;
                return count;
            }

            bool seek(u64 frame) override {
                position_ = std::min(frame, info_.total_frames);
                return true;
            }

            [[nodiscard]] u64 position() const override { return position_; }

          private:
            [[nodiscard]] f32 decode(const std::byte *p) const {
                switch (encoding_) {
                    case Encoding::Float32: {
                        u32 raw = be32(p);
                        f32 v;
                        std::memcpy(&v, &raw, 4);
                        return v;
                    }
                    case Encoding::Float64: {
                        u64 raw = (static_cast<u64>(be32(p)) << 32) | be32(p + 4);
                        f64 v;
                        std::memcpy(&v, &raw, 8);
                        return static_cast<f32>(v);
                    }
                    case Encoding::PcmLittleEndian: {
                        i32 v = 0;
                        for (u32 b = 0; b < bytes_per_sample_; ++b) {
                            v |= static_cast<i32>(static_cast<u8>(p[b])) << (8 * b);
                        }
                        const u32 shift = 32 - 8 * bytes_per_sample_;
                        v = static_cast<i32>(static_cast<u32>(v) << shift) >> shift; // sign-extend
                        return static_cast<f32>(v) / static_cast<f32>(1u << (8 * bytes_per_sample_ - 1));
                    }
                    case Encoding::PcmBigEndian: {
                        u32 raw = 0;
                        for (u32 b = 0; b < bytes_per_sample_; ++b) {
                            raw = (raw << 8) | static_cast<u8>(p[b]);
                        }
                        const u32 shift = 32 - 8 * bytes_per_sample_;
                        const i32 v = static_cast<i32>(raw << shift) >> shift;
                        return static_cast<f32>(v) / static_cast<f32>(1u << (8 * bytes_per_sample_ - 1));
                    }
                }
                return 0.0f;
            }

            EncodedBytes bytes_;
            AudioStreamInfo info_;
            Encoding encoding_ = Encoding::PcmBigEndian;
            u32 bits_ = 0;
            u32 bytes_per_sample_ = 2;
            usize frame_bytes_ = 0;
            usize data_begin_ = 0;
            u64 data_bytes_ = 0;
            u64 position_ = 0;
        };

        class AiffBackend final : public DecoderBackend {
          public:
            [[nodiscard]] ustr name() const override { return "AIFF / AIFF-C"_ustr; }
            [[nodiscard]] std::vector<UString> extensions() const override { return {".aif", ".aiff", ".aifc"}; }
            [[nodiscard]] int probe(std::span<const std::byte> header, const UString &extension) const override {
                if (header.size() >= 12 && std::memcmp(header.data(), "FORM", 4) == 0 &&
                    (std::memcmp(header.data() + 8, "AIFF", 4) == 0 || std::memcmp(header.data() + 8, "AIFC", 4) == 0)) {
                    return 100;
                }
                return (extension == ".aif"_ustr || extension == ".aiff"_ustr || extension == ".aifc"_ustr) ? 10 : 0;
            }
            [[nodiscard]] std::unique_ptr<StreamDecoder> open(EncodedBytes bytes) const override {
                auto decoder = std::make_unique<AiffDecoder>();
                return decoder->open(std::move(bytes)) ? std::move(decoder) : nullptr;
            }
        };

    } // namespace

    std::unique_ptr<DecoderBackend> make_aiff_backend() { return std::make_unique<AiffBackend>(); }

} // namespace SFT::Audio
