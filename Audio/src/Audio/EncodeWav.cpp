#include <Audio/EncodeCommon.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace SFT::Audio {

    namespace {

        using detail::PcmPacker;

        // ---- little-endian byte building ---------------------------------------------------------------------------------

        struct Bytes {
            std::vector<std::byte> data;
            void put8(u8 v) { data.push_back(static_cast<std::byte>(v)); }
            void put16(u16 v) { put8(v & 0xFF); put8(v >> 8); }
            void put32(u32 v) { put16(static_cast<u16>(v & 0xFFFF)); put16(static_cast<u16>(v >> 16)); }
            void put64(u64 v) { put32(static_cast<u32>(v & 0xFFFFFFFFu)); put32(static_cast<u32>(v >> 32)); }
            void tag(const char (&t)[5]) { for (int i = 0; i < 4; ++i) put8(static_cast<u8>(t[i])); }
            void raw(const void *p, usize n) {
                const auto *b = static_cast<const std::byte *>(p);
                data.insert(data.end(), b, b + n);
            }
            void pad_to_even() { if (data.size() & 1u) put8(0); }
        };

        constexpr std::array<u8, 16> kSubformatTail = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71, 0x00, 0x00};

        // Wave64 GUIDs (little-endian field layout, as stored in the file).
        constexpr std::array<u8, 16> kW64Riff = {0x72, 0x69, 0x66, 0x66, 0x2E, 0x91, 0xCF, 0x11, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00};
        constexpr std::array<u8, 16> kW64Wave = {0x77, 0x61, 0x76, 0x65, 0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A};
        constexpr std::array<u8, 16> kW64Fmt = {0x66, 0x6D, 0x74, 0x20, 0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A};
        constexpr std::array<u8, 16> kW64Data = {0x64, 0x61, 0x74, 0x61, 0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A};

        const char *info_id(const UString &key) {
            if (key == "title"_ustr) return "INAM";
            if (key == "artist"_ustr) return "IART";
            if (key == "album"_ustr) return "IPRD";
            if (key == "comment"_ustr) return "ICMT";
            if (key == "date"_ustr) return "ICRD";
            if (key == "genre"_ustr) return "IGNR";
            if (key == "track"_ustr) return "ITRK";
            if (key == "software"_ustr) return "ISFT";
            return nullptr;
        }

        class WavFamilyEncoder final : public detail::EncoderBase {
          public:
            WavFamilyEncoder(bool wave64, const EncoderConfig &config) : wave64_(wave64), config_(config), packer_(config.options.sample_format, false, config.options.dither) {}

            [[nodiscard]] std::expected<void, UString> start() {
                WriteOptions write_options;
                write_options.atomic = true;
                auto writer = Foundation::Io::FileWriter::create(config_.path, write_options);
                if (!writer) {
                    return std::unexpected(io_error(writer.error()));
                }
                writer_ = std::move(*writer);
                Bytes header;
                const u32 bytes = detail::bytes_per_sample(config_.options.sample_format);
                frame_bytes_ = bytes * config_.channels;
                if (wave64_) {
                    header.raw(kW64Riff.data(), 16);
                    header.put64(0); // total size, patched in finish()
                    header.raw(kW64Wave.data(), 16);
                    Bytes fmt = format_body();
                    header.raw(kW64Fmt.data(), 16);
                    header.put64(24 + fmt.data.size());
                    header.raw(fmt.data.data(), fmt.data.size());
                    while (header.data.size() % 8) header.put8(0);
                    header.raw(kW64Data.data(), 16);
                    data_size_offset_ = header.data.size();
                    header.put64(0);
                    data_offset_ = header.data.size();
                } else {
                    header.tag("RIFF");
                    header.put32(0);
                    header.tag("WAVE");
                    header.tag("JUNK"); // becomes ds64 if the file outgrows 4 GB
                    header.put32(28);
                    for (int i = 0; i < 28; ++i) header.put8(0);
                    Bytes fmt = format_body();
                    header.tag("fmt ");
                    header.put32(static_cast<u32>(fmt.data.size()));
                    header.raw(fmt.data.data(), fmt.data.size());
                    header.pad_to_even();
                    if (detail::is_float(config_.options.sample_format)) {
                        header.tag("fact");
                        header.put32(4);
                        header.put32(0);
                        fact_offset_ = header.data.size() - 4;
                    }
                    header.tag("data");
                    data_size_offset_ = header.data.size();
                    header.put32(0);
                    data_offset_ = header.data.size();
                }
                if (!writer_.write(header.data)) {
                    return std::unexpected(writer_.error());
                }
                return {};
            }

            bool write(const f32 *interleaved, usize frames) override {
                return write_chunked(interleaved, frames, config_.channels, [this](const f32 *chunk, usize n) {
                    scratch_.clear();
                    packer_.pack(chunk, n * config_.channels, scratch_);
                    return writer_.write(scratch_) || fail(io_error(writer_.error()));
                });
            }

            std::expected<void, UString> finish() override {
                if (!error_.empty()) {
                    return std::unexpected(error_);
                }
                const u64 data_bytes = frames_ * frame_bytes_;
                Bytes tail;
                if (wave64_) {
                    // Chunks are padded to eight bytes in Wave64.
                    const u64 pad = (8 - ((data_offset_ + data_bytes) % 8)) % 8;
                    for (u64 i = 0; i < pad; ++i) tail.put8(0);
                } else {
                    if (data_bytes & 1u) tail.put8(0);
                    append_markers(tail);
                    append_info(tail);
                    append_loop(tail);
                }
                if (!tail.data.empty() && !writer_.write(tail.data)) {
                    return std::unexpected(io_error(writer_.error()));
                }
                const u64 file_size = writer_.size();
                if (wave64_) {
                    Bytes size_field;
                    size_field.put64(file_size);
                    if (!writer_.seek(16) || !writer_.write(size_field.data)) return std::unexpected(io_error(writer_.error()));
                    Bytes data_field;
                    data_field.put64(24 + data_bytes);
                    if (!writer_.seek(data_size_offset_) || !writer_.write(data_field.data)) return std::unexpected(io_error(writer_.error()));
                } else {
                    const u64 riff_size = file_size - 8;
                    const bool rf64 = riff_size > 0xFFFFFFFFull || data_bytes > 0xFFFFFFFFull;
                    Bytes head;
                    if (rf64) {
                        head.tag("RF64");
                        head.put32(0xFFFFFFFFu);
                        if (!writer_.seek(0) || !writer_.write(head.data)) return std::unexpected(io_error(writer_.error()));
                        Bytes ds64;
                        ds64.tag("ds64");
                        ds64.put32(28);
                        ds64.put64(riff_size);
                        ds64.put64(data_bytes);
                        ds64.put64(frames_);
                        ds64.put32(0);
                        if (!writer_.seek(12) || !writer_.write(ds64.data)) return std::unexpected(io_error(writer_.error()));
                        Bytes data_field;
                        data_field.put32(0xFFFFFFFFu);
                        if (!writer_.seek(data_size_offset_) || !writer_.write(data_field.data)) return std::unexpected(io_error(writer_.error()));
                    } else {
                        head.put32(static_cast<u32>(riff_size));
                        if (!writer_.seek(4) || !writer_.write(head.data)) return std::unexpected(io_error(writer_.error()));
                        Bytes data_field;
                        data_field.put32(static_cast<u32>(data_bytes));
                        if (!writer_.seek(data_size_offset_) || !writer_.write(data_field.data)) return std::unexpected(io_error(writer_.error()));
                    }
                    if (fact_offset_ != 0) {
                        Bytes fact;
                        fact.put32(static_cast<u32>(std::min<u64>(frames_, 0xFFFFFFFFull)));
                        if (!writer_.seek(fact_offset_) || !writer_.write(fact.data)) return std::unexpected(io_error(writer_.error()));
                    }
                }
                return writer_.commit();
            }

          private:
            using WriteOptions = Foundation::Io::WriteOptions;

            [[nodiscard]] Bytes format_body() const {
                const SampleFormat format = config_.options.sample_format;
                const u32 channels = config_.channels;
                const u32 bytes = detail::bytes_per_sample(format);
                const bool is_float = detail::is_float(format);
                u32 mask = 0;
                if (config_.layout.kind == ChannelKind::Speakers && config_.layout.channels == channels && channels <= 32) {
                    mask = wave_channel_mask(config_.layout.speakers);
                    if (std::popcount(mask) != static_cast<int>(channels)) {
                        mask = 0;
                    }
                }
                const bool extensible = channels > 2 || bytes > 2 || mask != 0;
                Bytes b;
                b.put16(extensible ? 0xFFFE : (is_float ? 3 : 1));
                b.put16(static_cast<u16>(channels));
                b.put32(config_.sample_rate);
                b.put32(config_.sample_rate * channels * bytes);
                b.put16(static_cast<u16>(channels * bytes));
                b.put16(static_cast<u16>(bytes * 8));
                if (extensible) {
                    b.put16(22);
                    b.put16(static_cast<u16>(bytes * 8)); // valid bits
                    b.put32(mask);
                    b.put16(is_float ? 3 : 1);
                    b.raw(kSubformatTail.data(), 14);
                }
                return b;
            }

            void append_markers(Bytes &out) const {
                const auto &markers = config_.options.markers;
                if (markers.empty()) {
                    return;
                }
                const u32 count = static_cast<u32>(markers.size());
                out.tag("cue ");
                out.put32(4 + count * 24);
                out.put32(count);
                for (u32 i = 0; i < count; ++i) {
                    const u32 frame = static_cast<u32>(std::min<u64>(markers[i].frame, 0xFFFFFFFFull));
                    out.put32(i + 1);
                    out.put32(frame);
                    out.tag("data");
                    out.put32(0);
                    out.put32(0);
                    out.put32(frame);
                }
                // Labels in a LIST adtl chunk.
                Bytes list;
                list.tag("adtl");
                for (u32 i = 0; i < count; ++i) {
                    const UString &name = markers[i].name;
                    list.tag("labl");
                    list.put32(4 + static_cast<u32>(name.byte_size()) + 1);
                    list.put32(i + 1);
                    list.raw(name.c_str(), name.byte_size() + 1);
                    list.pad_to_even();
                }
                out.tag("LIST");
                out.put32(static_cast<u32>(list.data.size()));
                out.raw(list.data.data(), list.data.size());
                out.pad_to_even();
            }

            void append_info(Bytes &out) const {
                Bytes list;
                list.tag("INFO");
                bool any = false;
                for (const auto &[key, value] : config_.options.tags) {
                    const char *id = info_id(key);
                    if (id == nullptr) {
                        continue;
                    }
                    any = true;
                    list.raw(id, 4);
                    list.put32(static_cast<u32>(value.size()) + 1);
                    list.raw(value.c_str(), value.size() + 1);
                    list.pad_to_even();
                }
                if (!any) {
                    return;
                }
                out.tag("LIST");
                out.put32(static_cast<u32>(list.data.size()));
                out.raw(list.data.data(), list.data.size());
                out.pad_to_even();
            }

            void append_loop(Bytes &out) const {
                const auto &loop = config_.options.loop;
                if (!loop || loop->end <= loop->start) {
                    return;
                }
                out.tag("smpl");
                out.put32(36 + 24);
                out.put32(0);                                                       // manufacturer
                out.put32(0);                                                       // product
                out.put32(static_cast<u32>(1000000000ull / std::max(config_.sample_rate, 1u))); // sample period, ns
                out.put32(60);                                                      // MIDI unity note
                out.put32(0);                                                       // pitch fraction
                out.put32(0);                                                       // SMPTE format
                out.put32(0);                                                       // SMPTE offset
                out.put32(1);                                                       // loops
                out.put32(0);                                                       // sampler data
                out.put32(0);                                                       // cue point id
                out.put32(0);                                                       // forward loop
                out.put32(static_cast<u32>(loop->start));
                out.put32(static_cast<u32>(loop->end - 1)); // stored inclusive
                out.put32(0);
                out.put32(0);                               // play count: infinite
            }

            bool wave64_;
            EncoderConfig config_;
            PcmPacker packer_;
            Foundation::Io::FileWriter writer_;
            std::vector<std::byte> scratch_;
            u32 frame_bytes_ = 0;
            u64 data_size_offset_ = 0, data_offset_ = 0, fact_offset_ = 0;
        };

        class WavBackend final : public EncoderBackend {
          public:
            explicit WavBackend(bool wave64) : wave64_(wave64) {}
            [[nodiscard]] ustr name() const override { return wave64_ ? "Wave64"_ustr : "WAV"_ustr; }
            [[nodiscard]] AudioFormat format() const override { return wave64_ ? AudioFormat::Wave64 : AudioFormat::Wav; }
            [[nodiscard]] std::expected<std::unique_ptr<AudioEncoder>, UString> open(const EncoderConfig &config) const override {
                auto encoder = std::make_unique<WavFamilyEncoder>(wave64_, config);
                if (auto started = encoder->start(); !started) {
                    return std::unexpected(started.error());
                }
                return std::unique_ptr<AudioEncoder>(std::move(encoder));
            }

          private:
            bool wave64_;
        };

        // ---- headerless PCM -------------------------------------------------------------------------------------------------

        class RawEncoder final : public detail::EncoderBase {
          public:
            explicit RawEncoder(const EncoderConfig &config) : channels_(config.channels), packer_(config.options.sample_format, false, config.options.dither) {}
            [[nodiscard]] std::expected<void, UString> start(const EncoderConfig &config) {
                auto writer = Foundation::Io::FileWriter::create(config.path);
                if (!writer) {
                    return std::unexpected(io_error(writer.error()));
                }
                writer_ = std::move(*writer);
                return {};
            }
            bool write(const f32 *interleaved, usize frames) override {
                scratch_.clear();
                packer_.pack(interleaved, frames * channels_, scratch_);
                frames_ += frames;
                return writer_.write(scratch_) || fail(io_error(writer_.error()));
            }
            std::expected<void, UString> finish() override {
                if (!error_.empty()) {
                    return std::unexpected(error_);
                }
                return writer_.commit();
            }

          private:
            u32 channels_;
            PcmPacker packer_;
            Foundation::Io::FileWriter writer_;
            std::vector<std::byte> scratch_;
        };

        class RawBackend final : public EncoderBackend {
          public:
            [[nodiscard]] ustr name() const override { return "raw PCM"_ustr; }
            [[nodiscard]] AudioFormat format() const override { return AudioFormat::RawPcm; }
            [[nodiscard]] std::expected<std::unique_ptr<AudioEncoder>, UString> open(const EncoderConfig &config) const override {
                auto encoder = std::make_unique<RawEncoder>(config);
                if (auto started = encoder->start(config); !started) {
                    return std::unexpected(started.error());
                }
                return std::unique_ptr<AudioEncoder>(std::move(encoder));
            }
        };

    } // namespace

    std::unique_ptr<EncoderBackend> make_wav_encoder() { return std::make_unique<WavBackend>(false); }
    std::unique_ptr<EncoderBackend> make_wave64_encoder() { return std::make_unique<WavBackend>(true); }
    std::unique_ptr<EncoderBackend> make_raw_encoder() { return std::make_unique<RawBackend>(); }

} // namespace SFT::Audio
