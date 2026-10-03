#include <Audio/EncodeCommon.hpp>

#include <algorithm>
#include <cmath>

namespace SFT::Audio {

    namespace {

        struct BigEndianBytes {
            std::vector<std::byte> data;
            void put8(u8 v) { data.push_back(static_cast<std::byte>(v)); }
            void put16(u16 v) { put8(v >> 8); put8(v & 0xFF); }
            void put32(u32 v) { put16(static_cast<u16>(v >> 16)); put16(static_cast<u16>(v & 0xFFFF)); }
            void tag(const char (&t)[5]) { for (int i = 0; i < 4; ++i) put8(static_cast<u8>(t[i])); }
            void raw(const void *p, usize n) {
                const auto *b = static_cast<const std::byte *>(p);
                data.insert(data.end(), b, b + n);
            }
            void pascal_string(const UString &s) {
                const usize n = std::min<usize>(s.size(), 255);
                put8(static_cast<u8>(n));
                raw(s.data(), n);
                if ((n + 1) & 1u) put8(0); // pstrings are padded to an even total length
            }
            void pad_to_even() { if (data.size() & 1u) put8(0); }
        };

        /// IEEE 754 80-bit extended precision, big endian: how AIFF stores the sample rate.
        void put_extended(BigEndianBytes &out, f64 value) {
            u16 exponent = 0;
            u64 mantissa = 0;
            if (value > 0.0) {
                int e = 0;
                const f64 fraction = std::frexp(value, &e); // value = fraction * 2^e, fraction in [0.5, 1)
                exponent = static_cast<u16>(e - 1 + 16383);
                mantissa = static_cast<u64>(std::ldexp(fraction, 64));
            }
            out.put16(exponent);
            out.put32(static_cast<u32>(mantissa >> 32));
            out.put32(static_cast<u32>(mantissa & 0xFFFFFFFFu));
        }

        class AiffEncoder final : public detail::EncoderBase {
          public:
            explicit AiffEncoder(const EncoderConfig &config)
                : config_(config), packer_(config.options.sample_format, true, config.options.dither) {}

            [[nodiscard]] std::expected<void, UString> start() {
                auto writer = Foundation::Io::FileWriter::create(config_.path);
                if (!writer) {
                    return std::unexpected(io_error(writer.error()));
                }
                writer_ = std::move(*writer);
                const SampleFormat format = config_.options.sample_format;
                // Integer PCM fits plain AIFF; floats need AIFF-C ('fl32'/'fl64'). 8-bit AIFF is signed, so UInt8 is promoted.
                aifc_ = detail::is_float(format);
                bytes_ = detail::bytes_per_sample(format == SampleFormat::UInt8 ? SampleFormat::Int16 : format);
                effective_ = format == SampleFormat::UInt8 ? SampleFormat::Int16 : format;
                packer_ = detail::PcmPacker(effective_, true, config_.options.dither);
                frame_bytes_ = bytes_ * config_.channels;

                BigEndianBytes head;
                head.tag("FORM");
                head.put32(0); // patched
                if (aifc_) {
                    head.tag("AIFC");
                    head.tag("FVER");
                    head.put32(4);
                    head.put32(0xA2805140u); // AIFC version 1
                } else {
                    head.tag("AIFF");
                }
                BigEndianBytes comm;
                comm.put16(static_cast<u16>(config_.channels));
                comm.put32(0); // frames, patched
                comm.put16(static_cast<u16>(bytes_ * 8));
                put_extended(comm, static_cast<f64>(config_.sample_rate));
                if (aifc_) {
                    if (effective_ == SampleFormat::Float64) comm.tag("fl64"); else comm.tag("fl32");
                    comm.pascal_string(effective_ == SampleFormat::Float64 ? "64-bit floating point" : "32-bit floating point");
                } else {
                    // 'sowt' would be little-endian; plain AIFF is always big endian PCM.
                }
                head.tag("COMM");
                head.put32(static_cast<u32>(comm.data.size()));
                comm_offset_ = head.data.size();
                head.raw(comm.data.data(), comm.data.size());
                head.pad_to_even();
                head.tag("SSND");
                ssnd_size_offset_ = head.data.size();
                head.put32(0); // patched
                head.put32(0); // data offset
                head.put32(0); // block size
                if (!writer_.write(head.data)) {
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
                BigEndianBytes tail;
                if (data_bytes & 1u) tail.put8(0);
                append_markers(tail);
                append_text(tail);
                if (!tail.data.empty() && !writer_.write(tail.data)) {
                    return std::unexpected(io_error(writer_.error()));
                }
                const u64 file_size = writer_.size();
                if (file_size - 8 > 0xFFFFFFFFull) {
                    return std::unexpected("audio: AIFF cannot hold more than 4 GB; use WAV (RF64) or Wave64 for this recording");
                }
                BigEndianBytes patch;
                patch.put32(static_cast<u32>(file_size - 8));
                if (!writer_.seek(4) || !writer_.write(patch.data)) return std::unexpected(io_error(writer_.error()));
                BigEndianBytes frames_field;
                frames_field.put32(static_cast<u32>(frames_));
                if (!writer_.seek(comm_offset_ + 2) || !writer_.write(frames_field.data)) return std::unexpected(io_error(writer_.error()));
                BigEndianBytes ssnd;
                ssnd.put32(static_cast<u32>(8 + data_bytes));
                if (!writer_.seek(ssnd_size_offset_) || !writer_.write(ssnd.data)) return std::unexpected(io_error(writer_.error()));
                return writer_.commit();
            }

          private:
            void append_markers(BigEndianBytes &out) const {
                const auto &markers = config_.options.markers;
                if (markers.empty()) {
                    return;
                }
                BigEndianBytes body;
                body.put16(static_cast<u16>(markers.size()));
                for (usize i = 0; i < markers.size(); ++i) {
                    body.put16(static_cast<u16>(i + 1));
                    body.put32(static_cast<u32>(std::min<u64>(markers[i].frame, 0xFFFFFFFFull)));
                    body.pascal_string(markers[i].name);
                }
                out.tag("MARK");
                out.put32(static_cast<u32>(body.data.size()));
                out.raw(body.data.data(), body.data.size());
                out.pad_to_even();
            }

            void append_text(BigEndianBytes &out) const {
                for (const auto &[key, value] : config_.options.tags) {
                    const char *id = key == "title"_ustr ? "NAME" : key == "artist"_ustr ? "AUTH" : key == "comment"_ustr ? "ANNO" : key == "copyright"_ustr ? "(c) " : nullptr;
                    if (id == nullptr) {
                        continue;
                    }
                    out.raw(id, 4);
                    out.put32(static_cast<u32>(value.size()));
                    out.raw(value.data(), value.size());
                    out.pad_to_even();
                }
            }

            EncoderConfig config_;
            detail::PcmPacker packer_;
            Foundation::Io::FileWriter writer_;
            std::vector<std::byte> scratch_;
            bool aifc_ = false;
            u32 bytes_ = 2, frame_bytes_ = 4;
            SampleFormat effective_ = SampleFormat::Int16;
            u64 comm_offset_ = 0, ssnd_size_offset_ = 0;
        };

        class AiffBackend final : public EncoderBackend {
          public:
            [[nodiscard]] ustr name() const override { return "AIFF"_ustr; }
            [[nodiscard]] AudioFormat format() const override { return AudioFormat::Aiff; }
            [[nodiscard]] u32 max_channels() const override { return 65535; }
            [[nodiscard]] std::expected<std::unique_ptr<AudioEncoder>, UString> open(const EncoderConfig &config) const override {
                auto encoder = std::make_unique<AiffEncoder>(config);
                if (auto started = encoder->start(); !started) {
                    return std::unexpected(started.error());
                }
                return std::unique_ptr<AudioEncoder>(std::move(encoder));
            }
        };

    } // namespace

    std::unique_ptr<EncoderBackend> make_aiff_encoder() { return std::make_unique<AiffBackend>(); }

} // namespace SFT::Audio
