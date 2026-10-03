#include <Audio/EncodeCommon.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

#if STURDY_AUDIO_ENCODE_VORBIS || STURDY_AUDIO_ENCODE_OPUS
#include <ogg/ogg.h>
#endif
#if STURDY_AUDIO_ENCODE_VORBIS
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>
#endif
#if STURDY_AUDIO_ENCODE_OPUS
#include <Audio/OpusCodec.hpp>
#endif

namespace SFT::Audio {

#if STURDY_AUDIO_ENCODE_VORBIS || STURDY_AUDIO_ENCODE_OPUS

    namespace {

        /// Pages of one logical Ogg stream, written straight to a file.
        class OggWriter {
          public:
            explicit OggWriter(Foundation::Io::FileWriter &file) : file_(file) {
                // A time-derived serial keeps two files from the same session distinct if they are ever chained.
                const auto seed = static_cast<int>(std::chrono::steady_clock::now().time_since_epoch().count() & 0x7FFFFFFF);
                ogg_stream_init(&stream_, seed);
            }
            ~OggWriter() { ogg_stream_clear(&stream_); }
            OggWriter(const OggWriter &) = delete;
            OggWriter &operator=(const OggWriter &) = delete;

            void packet_in(ogg_packet &packet) { ogg_stream_packetin(&stream_, &packet); }
            /// Writes every complete page; `force` also writes a partial one (headers must each end a page).
            bool pages(bool force) {
                ogg_page page;
                while ((force ? ogg_stream_flush(&stream_, &page) : ogg_stream_pageout(&stream_, &page)) != 0) {
                    if (!file_.write(page.header, static_cast<usize>(page.header_len)) || !file_.write(page.body, static_cast<usize>(page.body_len))) {
                        return false;
                    }
                }
                return true;
            }

          private:
            Foundation::Io::FileWriter &file_;
            ogg_stream_state stream_{};
        };

    } // namespace

#endif

    // ================================================================================================================
    // Ogg Vorbis
    // ================================================================================================================

#if STURDY_AUDIO_ENCODE_VORBIS

    namespace {

        class VorbisEncoder final : public detail::EncoderBase {
          public:
            explicit VorbisEncoder(const EncoderConfig &config) : config_(config), permutation_(vorbis_channel_permutation(config.layout)) {}
            ~VorbisEncoder() override {
                if (ready_) {
                    vorbis_block_clear(&block_);
                    vorbis_dsp_clear(&dsp_);
                    vorbis_comment_clear(&comment_);
                    vorbis_info_clear(&info_);
                }
            }

            [[nodiscard]] std::expected<void, UString> start() {
                auto file = Foundation::Io::FileWriter::create(config_.path);
                if (!file) {
                    return std::unexpected(io_error(file.error()));
                }
                file_ = std::move(*file);
                ogg_ = std::make_unique<OggWriter>(file_);
                vorbis_info_init(&info_);
                const EncodeOptions &o = config_.options;
                int status;
                if (o.bitrate_bps > 0 && !o.variable_bitrate) {
                    // Constant bitrate: managed mode with min == nominal == max.
                    const long rate = static_cast<long>(o.bitrate_bps);
                    status = vorbis_encode_setup_managed(&info_, static_cast<long>(config_.channels), static_cast<long>(config_.sample_rate), rate, rate, rate);
                    if (status == 0) status = vorbis_encode_ctl(&info_, OV_ECTL_RATEMANAGE2_SET, nullptr);
                    if (status == 0) status = vorbis_encode_setup_init(&info_);
                } else if (o.bitrate_bps > 0) {
                    status = vorbis_encode_init(&info_, static_cast<long>(config_.channels), static_cast<long>(config_.sample_rate), -1, static_cast<long>(o.bitrate_bps), -1);
                } else {
                    // quality 0..1 maps onto Vorbis' -0.1..1.0; the default (~0.4) is about 128 kbps for stereo.
                    const f32 q = o.quality >= 0.0f ? std::clamp(o.quality, 0.0f, 1.0f) * 1.1f - 0.1f : 0.4f;
                    status = vorbis_encode_init_vbr(&info_, static_cast<long>(config_.channels), static_cast<long>(config_.sample_rate), q);
                }
                if (status != 0) {
                    vorbis_info_clear(&info_);
                    return std::unexpected("audio: libvorbis rejected the channel count, sample rate or bitrate settings");
                }
                vorbis_comment_init(&comment_);
                for (const auto &[key, value] : detail::comment_fields(o)) {
                    vorbis_comment_add_tag(&comment_, key.c_str(), value.c_str());
                }
                vorbis_analysis_init(&dsp_, &info_);
                vorbis_block_init(&dsp_, &block_);
                ready_ = true;

                ogg_packet header, header_comment, header_code;
                vorbis_analysis_headerout(&dsp_, &comment_, &header, &header_comment, &header_code);
                ogg_->packet_in(header);
                ogg_->packet_in(header_comment);
                ogg_->packet_in(header_code);
                if (!ogg_->pages(true)) {
                    return std::unexpected(io_error(file_.error()));
                }
                return {};
            }

            bool write(const f32 *interleaved, usize frames) override {
                if (!error_.empty()) {
                    return false;
                }
                constexpr usize chunk = 4096;
                const u32 channels = config_.channels;
                for (usize done = 0; done < frames; done += chunk) {
                    const usize n = std::min(chunk, frames - done);
                    float **planes = vorbis_analysis_buffer(&dsp_, static_cast<int>(n));
                    for (u32 slot = 0; slot < channels; ++slot) {
                        const u32 source = slot < permutation_.size() ? permutation_[slot] : slot;
                        for (usize i = 0; i < n; ++i) {
                            planes[slot][i] = interleaved[(done + i) * channels + source];
                        }
                    }
                    vorbis_analysis_wrote(&dsp_, static_cast<int>(n));
                    if (!drain()) {
                        return false;
                    }
                }
                frames_ += frames;
                return true;
            }

            std::expected<void, UString> finish() override {
                if (!error_.empty()) {
                    return std::unexpected(error_);
                }
                vorbis_analysis_wrote(&dsp_, 0); // end of stream
                if (!drain() || !ogg_->pages(true)) {
                    return std::unexpected(error_.empty() ? io_error(file_.error()) : error_);
                }
                return file_.commit();
            }

          private:
            bool drain() {
                while (vorbis_analysis_blockout(&dsp_, &block_) == 1) {
                    vorbis_analysis(&block_, nullptr);
                    vorbis_bitrate_addblock(&block_);
                    ogg_packet packet;
                    while (vorbis_bitrate_flushpacket(&dsp_, &packet)) {
                        ogg_->packet_in(packet);
                        if (!ogg_->pages(false)) {
                            return fail(io_error(file_.error()));
                        }
                    }
                }
                return true;
            }

            EncoderConfig config_;
            std::vector<u32> permutation_;
            Foundation::Io::FileWriter file_;
            std::unique_ptr<OggWriter> ogg_;
            vorbis_info info_{};
            vorbis_comment comment_{};
            vorbis_dsp_state dsp_{};
            vorbis_block block_{};
            bool ready_ = false;
        };

        class VorbisBackend final : public EncoderBackend {
          public:
            [[nodiscard]] ustr name() const override { return "Ogg Vorbis (libvorbis)"_ustr; }
            [[nodiscard]] AudioFormat format() const override { return AudioFormat::OggVorbis; }
            [[nodiscard]] u32 max_channels() const override { return 255; }
            [[nodiscard]] u32 constrain_sample_rate(u32 requested) const override { return std::clamp(requested, 4000u, 192000u); }
            [[nodiscard]] std::expected<std::unique_ptr<AudioEncoder>, UString> open(const EncoderConfig &config) const override {
                auto encoder = std::make_unique<VorbisEncoder>(config);
                if (auto started = encoder->start(); !started) {
                    return std::unexpected(started.error());
                }
                return std::unique_ptr<AudioEncoder>(std::move(encoder));
            }
        };

    } // namespace

    std::unique_ptr<EncoderBackend> make_vorbis_encoder() { return std::make_unique<VorbisBackend>(); }

#else

    std::unique_ptr<EncoderBackend> make_vorbis_encoder() { return nullptr; }

#endif

    // ================================================================================================================
    // Ogg Opus (RFC 7845), mono to 255 channels
    // ================================================================================================================

#if STURDY_AUDIO_ENCODE_OPUS

    namespace {

        class OpusStreamEncoder final : public detail::EncoderBase {
          public:
            explicit OpusStreamEncoder(const EncoderConfig &config) : config_(config) {}

            [[nodiscard]] std::expected<void, UString> start() {
                const EncodeOptions &o = config_.options;
                OpusEncoderSettings settings;
                settings.channels = config_.channels;
                settings.layout = config_.layout;
                settings.application = o.opus_application;
                settings.frame_ms = o.opus_frame_ms;
                settings.bitrate_bps = o.bitrate_bps;
                settings.quality = o.quality;
                settings.variable_bitrate = o.variable_bitrate;
                settings.complexity = o.opus_complexity;
                auto encoder = OpusEncoderHandle::create(settings);
                if (!encoder) {
                    return std::unexpected(encoder.error());
                }
                encoder_ = std::move(*encoder);
                const OpusStreamLayout &layout = encoder_->stream_layout();

                auto file = Foundation::Io::FileWriter::create(config_.path);
                if (!file) {
                    return std::unexpected(io_error(file.error()));
                }
                file_ = std::move(*file);
                ogg_ = std::make_unique<OggWriter>(file_);

                // Identification header, alone on its page.
                const u32 pre_skip = encoder_->pre_skip();
                std::vector<u8> head = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, static_cast<u8>(config_.channels)};
                head.push_back(static_cast<u8>(pre_skip & 0xFF));
                head.push_back(static_cast<u8>(pre_skip >> 8));
                for (int i = 0; i < 4; ++i) head.push_back(static_cast<u8>((48000u >> (8 * i)) & 0xFF)); // original input rate
                head.push_back(0);
                head.push_back(0); // output gain
                head.push_back(static_cast<u8>(layout.family));
                if (layout.family != 0) {
                    head.push_back(static_cast<u8>(layout.streams));
                    head.push_back(static_cast<u8>(layout.coupled));
                    for (u32 c = 0; c < config_.channels; ++c) head.push_back(layout.mapping[c]);
                }
                ogg_packet id{};
                id.packet = head.data();
                id.bytes = static_cast<long>(head.size());
                id.b_o_s = 1;
                id.granulepos = 0;
                id.packetno = packet_number_++;
                ogg_->packet_in(id);
                if (!ogg_->pages(true)) {
                    return std::unexpected(io_error(file_.error()));
                }

                // Comment header, also alone on its page.
                std::vector<u8> tags = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
                const auto put32 = [&tags](u32 v) { for (int i = 0; i < 4; ++i) tags.push_back(static_cast<u8>((v >> (8 * i)) & 0xFF)); };
                const std::string_view vendor = "SturdyEngine (libopus)"; // goes into the packet as raw bytes
                put32(static_cast<u32>(vendor.size()));
                tags.insert(tags.end(), vendor.begin(), vendor.end());
                const auto fields = detail::comment_fields(o);
                put32(static_cast<u32>(fields.size()));
                for (const auto &[key, value] : fields) {
                    const std::string entry = std::format("{}={}", key, value);
                    put32(static_cast<u32>(entry.size()));
                    tags.insert(tags.end(), entry.begin(), entry.end());
                }
                ogg_packet comments{};
                comments.packet = tags.data();
                comments.bytes = static_cast<long>(tags.size());
                comments.granulepos = 0;
                comments.packetno = packet_number_++;
                ogg_->packet_in(comments);
                if (!ogg_->pages(true)) {
                    return std::unexpected(io_error(file_.error()));
                }
                packet_buffer_.resize(static_cast<usize>(std::max(layout.streams, 1)) * 4000);
                return {};
            }

            bool write(const f32 *interleaved, usize frames) override {
                if (!error_.empty()) {
                    return false;
                }
                const u32 channels = config_.channels;
                pending_.insert(pending_.end(), interleaved, interleaved + frames * channels);
                input_frames_ += frames;
                frames_ += frames;
                return emit(false);
            }

            std::expected<void, UString> finish() override {
                if (!error_.empty()) {
                    return std::unexpected(error_);
                }
                if (!emit(true)) {
                    return std::unexpected(error_);
                }
                return file_.commit();
            }

          private:
            // Encodes whole frames from `pending_`; at the end pads with silence until every real sample (plus the encoder's
            // look-ahead) has been emitted and flags the last packet.
            bool emit(bool final) {
                const u32 channels = config_.channels;
                const u32 frame_size = encoder_->frame_size();
                const u64 needed_samples = input_frames_ + encoder_->pre_skip();
                const u64 needed_packets = (needed_samples + frame_size - 1) / frame_size;
                for (;;) {
                    const usize available = pending_.size() / channels;
                    if (available < frame_size) {
                        if (!final || packets_ >= needed_packets) {
                            break;
                        }
                        pending_.resize(static_cast<usize>(frame_size) * channels, 0.0f);
                    }
                    const int bytes = encoder_->encode(pending_.data(), packet_buffer_.data(), packet_buffer_.size());
                    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(static_cast<usize>(frame_size) * channels));
                    if (bytes < 0) {
                        return fail("audio: libopus failed to encode (error " + std::to_string(bytes) + ")");
                    }
                    ++packets_;
                    const bool last = final && packets_ >= needed_packets;
                    ogg_packet packet{};
                    packet.packet = packet_buffer_.data();
                    packet.bytes = bytes;
                    packet.e_o_s = last ? 1 : 0;
                    // Granule position counts decoder output at 48 kHz from the start, pre-skip included; the last packet is cut
                    // to the real length so the decoder can drop the padding.
                    packet.granulepos = last ? static_cast<ogg_int64_t>(needed_samples) : static_cast<ogg_int64_t>(packets_ * frame_size);
                    packet.packetno = packet_number_++;
                    ogg_->packet_in(packet);
                    if (!ogg_->pages(last)) {
                        return fail(io_error(file_.error()));
                    }
                    if (last) {
                        break;
                    }
                }
                return true;
            }

            EncoderConfig config_;
            Foundation::Io::FileWriter file_;
            std::unique_ptr<OggWriter> ogg_;
            std::unique_ptr<OpusEncoderHandle> encoder_;
            std::vector<f32> pending_;
            std::vector<unsigned char> packet_buffer_;
            u64 input_frames_ = 0, packets_ = 0;
            ogg_int64_t packet_number_ = 0;
        };

        class OpusBackend final : public EncoderBackend {
          public:
            [[nodiscard]] ustr name() const override { return "Opus (libopus)"_ustr; }
            [[nodiscard]] AudioFormat format() const override { return AudioFormat::Opus; }
            [[nodiscard]] u32 max_channels() const override { return 255; }
            [[nodiscard]] u32 constrain_sample_rate(u32) const override { return 48000; }
            [[nodiscard]] std::expected<std::unique_ptr<AudioEncoder>, UString> open(const EncoderConfig &config) const override {
                auto encoder = std::make_unique<OpusStreamEncoder>(config);
                if (auto started = encoder->start(); !started) {
                    return std::unexpected(started.error());
                }
                return std::unique_ptr<AudioEncoder>(std::move(encoder));
            }
        };

    } // namespace

    std::unique_ptr<EncoderBackend> make_opus_encoder() { return std::make_unique<OpusBackend>(); }

#else

    std::unique_ptr<EncoderBackend> make_opus_encoder() { return nullptr; }

#endif

} // namespace SFT::Audio
