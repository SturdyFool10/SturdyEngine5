#include <Audio/Decoder.hpp>

#include <miniaudio.h>

#include <cstring>

namespace SFT::Audio {

    namespace {

        class MiniaudioDecoder final : public StreamDecoder {
          public:
            ~MiniaudioDecoder() override {
                if (ready_) {
                    ma_decoder_uninit(&decoder_);
                }
            }

            [[nodiscard]] bool open(EncodedBytes bytes, UString codec) {
                bytes_ = std::move(bytes);
                // Native channel count and rate, float samples: the registry's contract.
                ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
                if (ma_decoder_init_memory(bytes_->data(), bytes_->size(), &config, &decoder_) != MA_SUCCESS) {
                    return false;
                }
                ready_ = true;
                ma_format format;
                ma_uint32 channels = 0, rate = 0;
                if (ma_decoder_get_data_format(&decoder_, &format, &channels, &rate, nullptr, 0) != MA_SUCCESS || channels == 0 || rate == 0) {
                    return false;
                }
                info_.channels = channels;
                info_.sample_rate = rate;
                info_.codec = std::move(codec);
                ma_uint64 length = 0;
                if (ma_decoder_get_length_in_pcm_frames(&decoder_, &length) == MA_SUCCESS) {
                    info_.total_frames = length;
                }
                parse_wav_metadata(std::span<const std::byte>(bytes_->data(), bytes_->size()), info_);
                if (info_.layout.channels != info_.channels) {
                    info_.layout = layout_from_wave_mask(0, info_.channels); // FLAC and MP3 follow the WAVE channel order
                }
                return true;
            }

            [[nodiscard]] const AudioStreamInfo &info() const override { return info_; }

            u64 read(f32 *out, u64 frames) override {
                ma_uint64 read = 0;
                ma_decoder_read_pcm_frames(&decoder_, out, frames, &read);
                position_ += read;
                return read;
            }

            bool seek(u64 frame) override {
                if (ma_decoder_seek_to_pcm_frame(&decoder_, frame) != MA_SUCCESS) {
                    return false;
                }
                position_ = frame;
                return true;
            }

            [[nodiscard]] u64 position() const override { return position_; }

          private:
            EncodedBytes bytes_;
            ma_decoder decoder_{};
            bool ready_ = false;
            AudioStreamInfo info_;
            u64 position_ = 0;
        };

        class MiniaudioBackend final : public DecoderBackend {
          public:
            [[nodiscard]] ustr name() const override { return "miniaudio (WAV, W64, FLAC, MP3)"_ustr; }
            [[nodiscard]] std::vector<UString> extensions() const override {
                return {".wav", ".wave", ".w64", ".bwf"
#if STURDY_AUDIO_FLAC
                        , ".flac"
#endif
#if STURDY_AUDIO_MP3
                        , ".mp3"
#endif
                };
            }

            [[nodiscard]] int probe(std::span<const std::byte> header, const UString &extension) const override {
                const auto bytes = [&](usize i) { return i < header.size() ? static_cast<unsigned>(header[i]) : 0u; };
                // WAV: RIFF/RF64 containers and Sony Wave64 (a 16-byte GUID "riff" header).
                if (header.size() >= 12 && (std::memcmp(header.data(), "RIFF", 4) == 0 || std::memcmp(header.data(), "RF64", 4) == 0) &&
                    std::memcmp(header.data() + 8, "WAVE", 4) == 0) {
                    return 100;
                }
                if (header.size() >= 16 && std::memcmp(header.data(), "riff\x2e\x91\xcf\x11\xa5\xd6\x28\xdb\x04\xc1\x00\x00", 16) == 0) {
                    return 100;
                }
#if STURDY_AUDIO_FLAC
                if (header.size() >= 4 && std::memcmp(header.data(), "fLaC", 4) == 0) {
                    return 100;
                }
#endif
#if STURDY_AUDIO_MP3
                // MP3: an ID3v2 tag, or a frame sync (11 set bits).
                if (header.size() >= 3 && std::memcmp(header.data(), "ID3", 3) == 0) {
                    return 90;
                }
                if (bytes(0) == 0xFF && (bytes(1) & 0xE0) == 0xE0) {
                    return 60;
                }
#endif
                for (const UString &e : extensions()) {
                    if (e == extension) {
                        return 10;
                    }
                }
                return 0;
            }

            [[nodiscard]] std::unique_ptr<StreamDecoder> open(EncodedBytes bytes) const override {
                auto decoder = std::make_unique<MiniaudioDecoder>();
                const bool wav = bytes->size() >= 4 && (std::memcmp(bytes->data(), "RIFF", 4) == 0 || std::memcmp(bytes->data(), "RF64", 4) == 0 ||
                                                         std::memcmp(bytes->data(), "riff", 4) == 0);
                const bool flac = bytes->size() >= 4 && std::memcmp(bytes->data(), "fLaC", 4) == 0;
                if (!decoder->open(std::move(bytes), wav ? "wav" : (flac ? "flac" : "mp3"))) {
                    return nullptr;
                }
                return decoder;
            }
        };

    } // namespace

    std::unique_ptr<DecoderBackend> make_miniaudio_backend() { return std::make_unique<MiniaudioBackend>(); }

} // namespace SFT::Audio
