#pragma once

#include <Audio/Channels.hpp>
#include <Audio/Encode.hpp>

#include <Foundation/Foundation.hpp>

#include <array>
#include <expected>
#include <memory>
#include <string>
#include <vector>

struct OpusMSEncoder;
struct OpusMSDecoder;

namespace SFT::Audio {

    /// How a multichannel signal is laid out in an Opus stream (RFC 7845 section 5.1): the mapping family, how many coded
    /// streams there are, how many of them are coupled stereo pairs, and which stream channel feeds each output channel.
    /// File muxers write this into `OpusHead`; network senders describe it in the session description.
    struct OpusStreamLayout {
        int family = 0;  ///< 0 mono/stereo, 1 labelled surround up to 8 (Vorbis order), 2 ambisonics, 255 independent channels
        int streams = 1;
        int coupled = 0;
        std::array<u8, 255> mapping{};
    };

    struct OpusEncoderSettings {
        u32 channels = 2;
        ChannelLayoutInfo layout;
        OpusApplication application = OpusApplication::Audio;
        f32 frame_ms = 20.0f;
        u32 bitrate_bps = 0;   ///< 0: automatic (or from `quality`)
        f32 quality = -1.0f;
        bool variable_bitrate = true;
        u32 complexity = 10;
    };

    /// libopus's multistream encoder behind the engine's channel conventions: mono to 255 channels, surround reordered
    /// into Vorbis order for family 1, ambisonics as family 2, everything else as independent channels. One instance per
    /// stream; not thread safe. Shared by the Ogg Opus writer and the RTP sender.
    class OpusEncoderHandle {
      public:
        [[nodiscard]] static std::expected<std::unique_ptr<OpusEncoderHandle>, UString> create(const OpusEncoderSettings &settings);
        ~OpusEncoderHandle();
        OpusEncoderHandle(const OpusEncoderHandle &) = delete;
        OpusEncoderHandle &operator=(const OpusEncoderHandle &) = delete;

        [[nodiscard]] const OpusStreamLayout &stream_layout() const noexcept { return layout_; }
        [[nodiscard]] u32 channels() const noexcept { return channels_; }
        /// Samples per channel (at 48 kHz) each `encode` consumes.
        [[nodiscard]] u32 frame_size() const noexcept { return frame_size_; }
        /// Samples the decoder must discard at the start of a stream (the encoder's look-ahead).
        [[nodiscard]] u32 pre_skip() const noexcept { return pre_skip_; }
        void set_bitrate(u32 bits_per_second);

        /// Encodes exactly `frame_size()` interleaved frames in the caller's channel order. Returns the packet size in
        /// bytes, or a negative libopus error code.
        [[nodiscard]] int encode(const f32 *interleaved, u8 *out, usize capacity);

      private:
        OpusEncoderHandle() = default;
        OpusMSEncoder *encoder_ = nullptr;
        OpusStreamLayout layout_;
        u32 channels_ = 0;
        u32 frame_size_ = 960;
        u32 pre_skip_ = 0;
        std::vector<u32> permutation_;
        std::vector<f32> reordered_;
    };

    class OpusDecoderHandle {
      public:
        [[nodiscard]] static std::expected<std::unique_ptr<OpusDecoderHandle>, UString> create(u32 channels, const OpusStreamLayout &layout, i16 gain_q8 = 0);
        ~OpusDecoderHandle();
        OpusDecoderHandle(const OpusDecoderHandle &) = delete;
        OpusDecoderHandle &operator=(const OpusDecoderHandle &) = delete;

        /// The longest packet Opus can carry: 120 ms at 48 kHz.
        static constexpr u32 max_frame_samples = 5760;

        /// Decodes one packet into `out` (room for `max_frame_samples` frames of `channels` samples) and returns the frames
        /// produced, or a negative libopus error code. A null packet asks for loss concealment of `conceal_frames` frames.
        [[nodiscard]] int decode(const u8 *packet, usize bytes, f32 *out, u32 conceal_frames = 960);
        void reset();

      private:
        OpusDecoderHandle() = default;
        OpusMSDecoder *decoder_ = nullptr;
        u32 channels_ = 0;
    };

} // namespace SFT::Audio
