#include <Audio/OpusCodec.hpp>

#if STURDY_AUDIO_OPUS

#if __has_include(<opus/opus_multistream.h>)
#include <opus/opus_multistream.h>
#else
#include <opus_multistream.h>
#endif

#include <algorithm>
#include <cmath>

namespace SFT::Audio {

    std::expected<std::unique_ptr<OpusEncoderHandle>, UString> OpusEncoderHandle::create(const OpusEncoderSettings &s) {
        std::unique_ptr<OpusEncoderHandle> self(new OpusEncoderHandle());
        const u32 channels = s.channels;
        self->channels_ = channels;
        // Mapping family: 0 for mono/stereo, 1 (Vorbis order) for labelled surround up to 8 channels, 2 for ambisonics, and 255
        // (independent channels) for everything else, which is what multi-channel recordings usually are.
        int family = 255;
        if (channels <= 2) {
            family = 0;
        } else if (s.layout.kind == ChannelKind::Ambisonic) {
            family = 2;
        } else if (channels <= 8 && s.layout.kind == ChannelKind::Speakers) {
            family = 1;
            self->permutation_ = vorbis_channel_permutation(s.layout);
        }
        int application = OPUS_APPLICATION_AUDIO;
        if (s.application == OpusApplication::Voice) application = OPUS_APPLICATION_VOIP;
        if (s.application == OpusApplication::LowDelay) application = OPUS_APPLICATION_RESTRICTED_LOWDELAY;

        int error = OPUS_OK;
        OpusStreamLayout &layout = self->layout_;
        layout.family = family;
        self->encoder_ = opus_multistream_surround_encoder_create(48000, static_cast<int>(channels), family, &layout.streams, &layout.coupled,
                                                                  layout.mapping.data(), application, &error);
        if (self->encoder_ == nullptr || error != OPUS_OK) {
            return std::unexpected("audio: libopus could not be set up for " + std::to_string(channels) + " channels" +
                                   (family == 2 ? " (ambisonics needs (order+1)^2 channels, optionally plus two non-diegetic ones)" : ""));
        }

        int frame_code = OPUS_FRAMESIZE_20_MS;
        u32 frame_size = 960;
        const f32 ms = s.frame_ms;
        if (ms <= 2.75f) { frame_code = OPUS_FRAMESIZE_2_5_MS; frame_size = 120; }
        else if (ms <= 7.5f) { frame_code = OPUS_FRAMESIZE_5_MS; frame_size = 240; }
        else if (ms <= 15.0f) { frame_code = OPUS_FRAMESIZE_10_MS; frame_size = 480; }
        else if (ms <= 30.0f) { frame_code = OPUS_FRAMESIZE_20_MS; frame_size = 960; }
        else if (ms <= 50.0f) { frame_code = OPUS_FRAMESIZE_40_MS; frame_size = 1920; }
        else { frame_code = OPUS_FRAMESIZE_60_MS; frame_size = 2880; }
        self->frame_size_ = frame_size;
        opus_multistream_encoder_ctl(self->encoder_, OPUS_SET_EXPERT_FRAME_DURATION(frame_code));
        opus_multistream_encoder_ctl(self->encoder_, OPUS_SET_VBR(s.variable_bitrate ? 1 : 0));
        opus_multistream_encoder_ctl(self->encoder_, OPUS_SET_COMPLEXITY(static_cast<int>(std::min(s.complexity, 10u))));
        if (s.bitrate_bps > 0) {
            self->set_bitrate(s.bitrate_bps);
        } else if (s.quality >= 0.0f) {
            // 0..1 spans 24-256 kbps for a stereo pair's worth of streams.
            const f32 per_stream = 24000.0f + std::clamp(s.quality, 0.0f, 1.0f) * 232000.0f;
            self->set_bitrate(static_cast<u32>(per_stream * static_cast<f32>(layout.streams) * 0.75f + per_stream * 0.25f * static_cast<f32>(layout.coupled)));
        } else {
            opus_multistream_encoder_ctl(self->encoder_, OPUS_SET_BITRATE(OPUS_AUTO));
        }
        opus_int32 lookahead = 0;
        opus_multistream_encoder_ctl(self->encoder_, OPUS_GET_LOOKAHEAD(&lookahead));
        self->pre_skip_ = static_cast<u32>(std::max<opus_int32>(lookahead, 0));
        if (!self->permutation_.empty()) {
            self->reordered_.resize(static_cast<usize>(frame_size) * channels);
        }
        return self;
    }

    OpusEncoderHandle::~OpusEncoderHandle() {
        if (encoder_ != nullptr) {
            opus_multistream_encoder_destroy(encoder_);
        }
    }

    void OpusEncoderHandle::set_bitrate(u32 bits_per_second) {
        opus_multistream_encoder_ctl(encoder_, OPUS_SET_BITRATE(static_cast<opus_int32>(bits_per_second)));
    }

    int OpusEncoderHandle::encode(const f32 *interleaved, u8 *out, usize capacity) {
        const f32 *input = interleaved;
        if (!permutation_.empty()) {
            for (u32 i = 0; i < frame_size_; ++i) {
                for (u32 slot = 0; slot < channels_; ++slot) {
                    reordered_[static_cast<usize>(i) * channels_ + slot] = interleaved[static_cast<usize>(i) * channels_ + permutation_[slot]];
                }
            }
            input = reordered_.data();
        }
        return opus_multistream_encode_float(encoder_, input, static_cast<int>(frame_size_), out, static_cast<opus_int32>(capacity));
    }

    std::expected<std::unique_ptr<OpusDecoderHandle>, UString> OpusDecoderHandle::create(u32 channels, const OpusStreamLayout &layout, i16 gain_q8) {
        std::unique_ptr<OpusDecoderHandle> self(new OpusDecoderHandle());
        int error = OPUS_OK;
        self->decoder_ = opus_multistream_decoder_create(48000, static_cast<int>(channels), layout.streams, layout.coupled, layout.mapping.data(), &error);
        if (self->decoder_ == nullptr || error != OPUS_OK) {
            return std::unexpected("audio: libopus rejected the stream layout (" + std::to_string(channels) + " channels, " + std::to_string(layout.streams) + " streams)");
        }
        if (gain_q8 != 0) {
            opus_multistream_decoder_ctl(self->decoder_, OPUS_SET_GAIN(gain_q8));
        }
        self->channels_ = channels;
        return self;
    }

    OpusDecoderHandle::~OpusDecoderHandle() {
        if (decoder_ != nullptr) {
            opus_multistream_decoder_destroy(decoder_);
        }
    }

    int OpusDecoderHandle::decode(const u8 *packet, usize bytes, f32 *out, u32 conceal_frames) {
        if (packet == nullptr) {
            return opus_multistream_decode_float(decoder_, nullptr, 0, out, static_cast<int>(std::min(conceal_frames, max_frame_samples)), 0);
        }
        return opus_multistream_decode_float(decoder_, packet, static_cast<opus_int32>(bytes), out, static_cast<int>(max_frame_samples), 0);
    }

    void OpusDecoderHandle::reset() { opus_multistream_decoder_ctl(decoder_, OPUS_RESET_STATE); }

} // namespace SFT::Audio

#else

// Opus left out of the build: the types still exist (the network code names them) but cannot be created.
namespace SFT::Audio {

    std::expected<std::unique_ptr<OpusEncoderHandle>, UString> OpusEncoderHandle::create(const OpusEncoderSettings &) {
        return std::unexpected("audio: this build has no Opus codec (STURDY_AUDIO_OPUS=OFF)");
    }
    OpusEncoderHandle::~OpusEncoderHandle() = default;
    void OpusEncoderHandle::set_bitrate(u32) {}
    int OpusEncoderHandle::encode(const f32 *, u8 *, usize) { return -1; }
    std::expected<std::unique_ptr<OpusDecoderHandle>, UString> OpusDecoderHandle::create(u32, const OpusStreamLayout &, i16) {
        return std::unexpected("audio: this build has no Opus codec (STURDY_AUDIO_OPUS=OFF)");
    }
    OpusDecoderHandle::~OpusDecoderHandle() = default;
    int OpusDecoderHandle::decode(const u8 *, usize, f32 *, u32) { return -1; }
    void OpusDecoderHandle::reset() {}

} // namespace SFT::Audio

#endif
