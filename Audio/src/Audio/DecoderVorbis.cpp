// Ogg Vorbis through stb_vorbis (public domain). The implementation is compiled into this translation unit only.

#include <Audio/Decoder.hpp>

#if STURDY_AUDIO_VORBIS

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <Audio/VorbisComments.hpp>

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_INTEGER_CONVERSION
#include <stb_vorbis.c>

namespace SFT::Audio {

    namespace {

        class VorbisDecoder final : public StreamDecoder {
          public:
            ~VorbisDecoder() override {
                if (vorbis_ != nullptr) {
                    stb_vorbis_close(vorbis_);
                }
            }

            [[nodiscard]] bool open(EncodedBytes bytes) {
                bytes_ = std::move(bytes);
                int error = 0;
                vorbis_ = stb_vorbis_open_memory(reinterpret_cast<const unsigned char *>(bytes_->data()), static_cast<int>(bytes_->size()), &error, nullptr);
                if (vorbis_ == nullptr) {
                    return false;
                }
                const stb_vorbis_info vi = stb_vorbis_get_info(vorbis_);
                info_.channels = static_cast<u32>(vi.channels);
                info_.sample_rate = vi.sample_rate;
                info_.codec = "vorbis";
                info_.layout = vorbis_channel_layout(info_.channels); // channels come out in Vorbis order; the roles say which is which
                info_.total_frames = stb_vorbis_stream_length_in_samples(vorbis_);

                // Loop points and tags travel in the Vorbis comment header.
                const stb_vorbis_comment comments = stb_vorbis_get_comment(vorbis_);
                VorbisCommentReader reader{info_};
                for (int i = 0; i < comments.comment_list_length; ++i) {
                    reader.add(comments.comment_list[i]);
                }
                reader.finish();
                return info_.channels > 0 && info_.sample_rate > 0;
            }

            [[nodiscard]] const AudioStreamInfo &info() const override { return info_; }

            u64 read(f32 *out, u64 frames) override {
                const int channels = static_cast<int>(info_.channels);
                const int produced = stb_vorbis_get_samples_float_interleaved(vorbis_, channels, out, static_cast<int>(frames) * channels);
                position_ += static_cast<u64>(produced);
                return static_cast<u64>(produced);
            }

            bool seek(u64 frame) override {
                if (!stb_vorbis_seek(vorbis_, static_cast<unsigned int>(frame))) {
                    return false;
                }
                position_ = frame;
                return true;
            }

            [[nodiscard]] u64 position() const override { return position_; }

          private:
            EncodedBytes bytes_;
            stb_vorbis *vorbis_ = nullptr;
            AudioStreamInfo info_;
            u64 position_ = 0;
        };

        class VorbisBackend final : public DecoderBackend {
          public:
            [[nodiscard]] ustr name() const override { return "Ogg Vorbis (stb_vorbis)"_ustr; }
            [[nodiscard]] std::vector<UString> extensions() const override { return {".ogg", ".oga"}; }
            [[nodiscard]] int probe(std::span<const std::byte> header, const UString &extension) const override {
                if (header.size() >= 4 && std::memcmp(header.data(), "OggS", 4) == 0) {
                    // Ogg also wraps Opus/FLAC; a Vorbis stream announces itself in its first packet.
                    for (usize i = 0; i + 7 <= header.size(); ++i) {
                        if (header[i] == std::byte{0x01} && std::memcmp(header.data() + i + 1, "vorbis", 6) == 0) {
                            return 100;
                        }
                    }
                    return 30;
                }
                return (extension == ".ogg"_ustr || extension == ".oga"_ustr) ? 10 : 0;
            }
            [[nodiscard]] std::unique_ptr<StreamDecoder> open(EncodedBytes bytes) const override {
                auto decoder = std::make_unique<VorbisDecoder>();
                return decoder->open(std::move(bytes)) ? std::move(decoder) : nullptr;
            }
        };

    } // namespace

    std::unique_ptr<DecoderBackend> make_vorbis_backend() { return std::make_unique<VorbisBackend>(); }

} // namespace SFT::Audio

#else

namespace SFT::Audio {
    std::unique_ptr<DecoderBackend> make_vorbis_backend() { return nullptr; }
} // namespace SFT::Audio

#endif
