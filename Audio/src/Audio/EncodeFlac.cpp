#include <Audio/EncodeCommon.hpp>

#if STURDY_AUDIO_ENCODE_FLAC
#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>
#endif

namespace SFT::Audio {

#if STURDY_AUDIO_ENCODE_FLAC

    namespace {

        class FlacEncoder final : public detail::EncoderBase {
          public:
            explicit FlacEncoder(const EncoderConfig &config) : config_(config) {}
            ~FlacEncoder() override {
                if (encoder_ != nullptr) {
                    FLAC__stream_encoder_delete(encoder_);
                }
                for (FLAC__StreamMetadata *m : metadata_) {
                    FLAC__metadata_object_delete(m);
                }
            }

            [[nodiscard]] std::expected<void, UString> start() {
                auto writer = Foundation::Io::FileWriter::create(config_.path);
                if (!writer) {
                    return std::unexpected(io_error(writer.error()));
                }
                writer_ = std::move(*writer);
                encoder_ = FLAC__stream_encoder_new();
                if (encoder_ == nullptr) {
                    return std::unexpected("audio: could not create the FLAC encoder");
                }
                const SampleFormat format = config_.options.sample_format;
                bits_ = (format == SampleFormat::UInt8 || format == SampleFormat::Int16) ? 16 : 24;
                FLAC__stream_encoder_set_channels(encoder_, config_.channels);
                FLAC__stream_encoder_set_bits_per_sample(encoder_, bits_);
                FLAC__stream_encoder_set_sample_rate(encoder_, config_.sample_rate);
                FLAC__stream_encoder_set_compression_level(encoder_, std::min(config_.options.flac_compression, 8u));
                FLAC__stream_encoder_set_verify(encoder_, false);

                const auto fields = detail::comment_fields(config_.options);
                if (!fields.empty()) {
                    FLAC__StreamMetadata *comments = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
                    for (const auto &[key, value] : fields) {
                        FLAC__StreamMetadata_VorbisComment_Entry entry;
                        if (FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&entry, key.c_str(), value.c_str())) {
                            FLAC__metadata_object_vorbiscomment_append_comment(comments, entry, /*copy=*/false);
                        }
                    }
                    metadata_.push_back(comments);
                }
                if (!metadata_.empty()) {
                    FLAC__stream_encoder_set_metadata(encoder_, metadata_.data(), static_cast<unsigned>(metadata_.size()));
                }
                const FLAC__StreamEncoderInitStatus status = FLAC__stream_encoder_init_stream(encoder_, &FlacEncoder::on_write, &FlacEncoder::on_seek,
                                                                                               &FlacEncoder::on_tell, nullptr, this);
                if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
                    return std::unexpected(UString{std::format("audio: the FLAC encoder rejected the settings: {}", FLAC__StreamEncoderInitStatusString[status])});
                }
                return {};
            }

            bool write(const f32 *interleaved, usize frames) override {
                if (!error_.empty()) {
                    return false;
                }
                const f32 scale = bits_ == 16 ? 32767.0f : 8388607.0f;
                const usize total = frames * config_.channels;
                ints_.resize(total);
                for (usize i = 0; i < total; ++i) {
                    ints_[i] = static_cast<FLAC__int32>(std::lrint(std::clamp(interleaved[i], -1.0f, 1.0f) * scale));
                }
                if (!FLAC__stream_encoder_process_interleaved(encoder_, ints_.data(), static_cast<unsigned>(frames))) {
                    return fail(error_.empty() ? "audio: the FLAC encoder failed" : error_);
                }
                frames_ += frames;
                return error_.empty();
            }

            std::expected<void, UString> finish() override {
                if (!error_.empty()) {
                    return std::unexpected(error_);
                }
                if (!FLAC__stream_encoder_finish(encoder_) && error_.empty()) {
                    error_ = "audio: the FLAC encoder could not finish the stream";
                }
                if (!error_.empty()) {
                    return std::unexpected(error_);
                }
                return writer_.commit();
            }

          private:
            static FLAC__StreamEncoderWriteStatus on_write(const FLAC__StreamEncoder *, const FLAC__byte buffer[], size_t bytes, unsigned, unsigned, void *client) {
                auto *self = static_cast<FlacEncoder *>(client);
                if (!self->writer_.write(buffer, bytes)) {
                    self->fail(io_error(self->writer_.error()));
                    return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
                }
                return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
            }
            static FLAC__StreamEncoderSeekStatus on_seek(const FLAC__StreamEncoder *, FLAC__uint64 absolute_byte_offset, void *client) {
                auto *self = static_cast<FlacEncoder *>(client);
                return self->writer_.seek(absolute_byte_offset) ? FLAC__STREAM_ENCODER_SEEK_STATUS_OK : FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
            }
            static FLAC__StreamEncoderTellStatus on_tell(const FLAC__StreamEncoder *, FLAC__uint64 *absolute_byte_offset, void *client) {
                auto *self = static_cast<FlacEncoder *>(client);
                *absolute_byte_offset = self->writer_.tell();
                return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
            }

            EncoderConfig config_;
            Foundation::Io::FileWriter writer_;
            FLAC__StreamEncoder *encoder_ = nullptr;
            std::vector<FLAC__StreamMetadata *> metadata_;
            std::vector<FLAC__int32> ints_;
            u32 bits_ = 24;
        };

        class FlacBackend final : public EncoderBackend {
          public:
            [[nodiscard]] ustr name() const override { return "FLAC (libFLAC)"_ustr; }
            [[nodiscard]] AudioFormat format() const override { return AudioFormat::Flac; }
            [[nodiscard]] u32 max_channels() const override { return 8; }
            [[nodiscard]] u32 constrain_sample_rate(u32 requested) const override { return std::clamp(requested, 1u, 655350u); }
            [[nodiscard]] std::expected<std::unique_ptr<AudioEncoder>, UString> open(const EncoderConfig &config) const override {
                auto encoder = std::make_unique<FlacEncoder>(config);
                if (auto started = encoder->start(); !started) {
                    return std::unexpected(started.error());
                }
                return std::unique_ptr<AudioEncoder>(std::move(encoder));
            }
        };

    } // namespace

    std::unique_ptr<EncoderBackend> make_flac_encoder() { return std::make_unique<FlacBackend>(); }

#else

    std::unique_ptr<EncoderBackend> make_flac_encoder() { return nullptr; }

#endif

} // namespace SFT::Audio
