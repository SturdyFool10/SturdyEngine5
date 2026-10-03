#pragma once

#include <Audio/AudioBuffer.hpp>
#include <Audio/Channels.hpp>
#include <Audio/Dsp.hpp>

#include <Foundation/Foundation.hpp>

#include <functional>
#include <optional>
#include <string>
#include <memory>
#include <vector>

namespace SFT::Audio {

    /// The one pull interface everything audible implements: decoded files, streaming decoders, ring sources fed from
    /// any thread, and procedural generators all look the same to the mixer, so a gunshot sample and a synthesised
    /// engine hum share voices, buses and spatialisation.
    class DataSource {
      public:
        virtual ~DataSource() = default;

        [[nodiscard]] virtual u32 channel_count() const = 0;
        [[nodiscard]] virtual u32 sample_rate() const = 0;
        /// What the channels are: speaker roles, an ambisonic sound field, or independent signals. The default is the
        /// conventional layout for the channel count (1 mono, 2 stereo, 6 5.1, ...; anything else discrete).
        [[nodiscard]] virtual ChannelLayoutInfo channel_layout() const { return ChannelLayoutInfo::guess(channel_count()); }

        /// Fills up to `frames` frames of `out` (planar, `out.channels() >= channel_count()`), starting at frame 0 of the
        /// buffer, and returns how many were produced. Fewer than requested means the source ended; the remainder of
        /// `out` is zeroed by the caller. Must be real-time safe: no allocation, no locks, no I/O.
        virtual u32 read(AudioBuffer &out, u32 frames) = 0;

        /// True once nothing more will be produced.
        [[nodiscard]] virtual bool finished() const { return false; }

        /// Playback-rate multiplier (pitch and speed together); sources that cannot resample ignore it.
        virtual void set_rate(f32 /*rate*/) {}

        /// Advances without producing audio (virtual voices keep their place). Returns false if unsupported.
        virtual bool skip(u64 /*frames*/) { return false; }

        /// True when `set_rate` really changes pitch/speed. Sources that cannot are wrapped in a `RateConverter` by the
        /// mixer, so every source can be pitched.
        [[nodiscard]] virtual bool supports_rate() const { return false; }

        // ---- timeline: positions are in the source's own frames, `native_rate()` per second -----------------------
        /// Frames per second of `position_frames` / `length_frames` / `seek_frames` (a decoded file's rate).
        [[nodiscard]] virtual u32 native_rate() const { return sample_rate(); }
        /// True when the source can jump to any frame (decoded and streamed files can; synthesis and live feeds cannot).
        [[nodiscard]] virtual bool seekable() const { return false; }
        virtual bool seek_frames(u64 /*frame*/) { return false; }
        /// Frame of the next sample `read` will produce.
        [[nodiscard]] virtual u64 position_frames() const { return 0; }
        /// Total frames; 0 for endless or unknown.
        [[nodiscard]] virtual u64 length_frames() const { return 0; }
    };

    /// A named position in a piece of audio (a cue point authored in the file or added by code), in the file's own frames.
    struct AudioMarker {
        UString name;
        u64 frame = 0;
    };

    /// A loop region embedded in a file (WAV `smpl`, Vorbis LOOPSTART/LOOPLENGTH), in the file's own frames.
    struct LoopRegion {
        u64 start = 0;
        u64 end = 0; // exclusive
    };

    /// Immutable decoded audio shared between voices.
    struct SampleBuffer {
        u32 channels = 1;
        u32 sample_rate = 48000;
        /// Interleaved float samples (`frames * channels`), shared with whoever decoded them (the asset manager keeps
        /// the same allocation) so playing a loaded sound never copies it.
        std::shared_ptr<const std::vector<f32>> samples;
        /// Channel meaning; `layout.channels == 0` (the default) means "guess from the channel count".
        ChannelLayoutInfo layout;
        /// Cue points and loop region found in the file, if any.
        std::vector<AudioMarker> markers;
        std::optional<LoopRegion> loop;

        [[nodiscard]] u64 frames() const noexcept { return channels == 0 || !samples ? 0 : samples->size() / channels; }
        [[nodiscard]] f32 duration_seconds() const noexcept {
            return sample_rate == 0 ? 0.0f : static_cast<f32>(frames()) / static_cast<f32>(sample_rate);
        }
    };

    /// Plays a `SampleBuffer` with optional looping and cubic-interpolated resampling (pitch shift and conversion to
    /// the mixer rate in one step).
    class BufferSource final : public DataSource {
      public:
        /// `output_rate` is the mixer's rate; the buffer is converted on the fly.
        BufferSource(std::shared_ptr<const SampleBuffer> buffer, u32 output_rate, bool loop = false);

        u32 channel_count() const override { return buffer_->channels; }
        u32 sample_rate() const override { return output_rate_; }
        u32 read(AudioBuffer &out, u32 frames) override;
        bool finished() const override { return finished_; }
        void set_rate(f32 rate) override { rate_ = rate > 0.0f ? rate : 0.0f; }
        bool skip(u64 frames) override;
        bool supports_rate() const override { return true; }
        u32 native_rate() const override { return buffer_->sample_rate; }
        bool seekable() const override { return true; }
        bool seek_frames(u64 frame) override;
        u64 position_frames() const override { return position_ < 0.0 ? 0 : static_cast<u64>(position_); }
        u64 length_frames() const override { return buffer_->frames(); }

        /// What `read` would do for the next `frames` output frames, described instead of performed, so another executor (a
        /// compute backend) can do the arithmetic. Advances the source exactly as `read` does.
        struct BlockPlan {
            u32 valid_frames = 0; ///< frames of the block that carry audio (fewer than asked when a one-shot ends)
            i64 base = 0;         ///< integer source frame of the first output frame
            f32 frac = 0.0f;
            f32 step = 1.0f;
            bool loop = false;
        };
        [[nodiscard]] BlockPlan plan_block(u32 frames);
        [[nodiscard]] const std::shared_ptr<const SampleBuffer> &buffer() const noexcept { return buffer_; }
        [[nodiscard]] bool looping() const noexcept { return loop_; }

        void set_loop(bool loop) noexcept { loop_ = loop; }
        void seek_seconds(f64 seconds) noexcept;
        [[nodiscard]] f64 position_seconds() const noexcept;

      private:
        [[nodiscard]] f32 sample_at(i64 frame, u32 channel) const noexcept;

        std::shared_ptr<const SampleBuffer> buffer_;
        u32 output_rate_;
        bool loop_;
        bool finished_ = false;
        f64 position_ = 0.0; // fractional frame position in the source
        f32 rate_ = 1.0f;
    };

    /// Makes any source pitchable: resamples `inner`'s output by the rate it is given (cubic interpolation), buffering
    /// across blocks. The mixer wraps sources that do not support rates themselves, so synthesis patches, ring feeds and
    /// streamed files pitch up and down like samples do.
    class RateConverter final : public DataSource {
      public:
        /// `max_block_frames` is the largest `read` request that will be made.
        RateConverter(std::shared_ptr<DataSource> inner, u32 max_block_frames);

        u32 channel_count() const override { return inner_->channel_count(); }
        u32 sample_rate() const override { return inner_->sample_rate(); }
        u32 read(AudioBuffer &out, u32 frames) override;
        bool finished() const override { return inner_->finished() && static_cast<u64>(input_position_) + 2 >= input_length_; }
        void set_rate(f32 rate) override { rate_ = rate > 0.0f ? static_cast<f64>(rate) : 0.0; }
        bool supports_rate() const override { return true; }
        bool skip(u64 frames) override;
        u32 native_rate() const override { return inner_->native_rate(); }
        bool seekable() const override { return inner_->seekable(); }
        bool seek_frames(u64 frame) override;
        u64 position_frames() const override;
        u64 length_frames() const override { return inner_->length_frames(); }

      private:
        /// Refills the input window so frames [floor(pos)-1, floor(pos)+2] are available; false at the end of the source.
        bool refill();

        std::shared_ptr<DataSource> inner_;
        AudioBuffer input_;
        AudioBuffer scratch_; // inner reads land here first (allocated once, so reads never allocate)
        u64 input_length_ = 0;   // valid frames in `input_`
        f64 input_position_ = 0; // read position inside `input_`
        f64 rate_ = 1.0;
        bool ended_ = false;
    };

    /// A generator driven by a function; the function must be real-time safe.
    class CallbackSource final : public DataSource {
      public:
        using Fn = std::function<u32(AudioBuffer &out, u32 frames)>;
        CallbackSource(u32 channels, u32 sample_rate, Fn fn) : channels_(channels), sample_rate_(sample_rate), fn_(std::move(fn)) {}

        u32 channel_count() const override { return channels_; }
        u32 sample_rate() const override { return sample_rate_; }
        u32 read(AudioBuffer &out, u32 frames) override { return fn_ ? fn_(out, frames) : 0; }

      private:
        u32 channels_;
        u32 sample_rate_;
        Fn fn_;
    };

    /// Where a live feed starts playing and how it stays in step with the engine's clock.
    struct LiveSourceOptions {
        /// Frames to collect before the first one is played (the feed's jitter cushion). Also the fill level the drift
        /// control steers toward. 0 starts as soon as anything arrives.
        u32 prefill_frames = 0;
        /// If the backlog ever exceeds this, the oldest audio is dropped to get back to the cushion (a stalled consumer
        /// must not turn into seconds of latency). 0 = never drop.
        u32 max_latency_frames = 0;
        /// Nudge the playback rate by up to +-`max_rate_adjustment` to keep the backlog near `prefill_frames`, absorbing
        /// the unavoidable difference between the producer's clock (a microphone, a network sender) and the engine's.
        bool adaptive = true;
        f32 max_rate_adjustment = 0.002f;
    };

    /// Multi-channel audio fed from any one thread and played by the engine: a microphone or audio interface (up to 256
    /// channels), a network stream, a voice-chat decoder, audio from a video decoder. `push` interleaved frames from the
    /// producer; the mixer drains it, resampling `input_rate` to the engine's rate and steering out clock drift. Lock free
    /// (single producer, single consumer); underruns play silence and are counted rather than blocking either side.
    class LiveSource final : public DataSource {
      public:
        LiveSource(u32 channels, u32 input_rate, u32 output_rate, usize capacity_frames, const LiveSourceOptions &options = {});
        /// A mono feed at the engine's rate (the original `RingSource` constructor).
        LiveSource(u32 sample_rate, usize capacity_frames);
        ~LiveSource() override;

        /// Producer side: appends `frames` interleaved frames (`channels()` samples each) and returns how many fit.
        usize push(const f32 *interleaved, usize frames) noexcept;
        /// Producer side, for planar data (`planes[c][i]`).
        usize push_planar(const f32 *const *planes, usize frames) noexcept;
        /// Marks the stream as complete: the source finishes once it drains.
        void close() noexcept;
        /// Tells the engine what the channels are (a mic array's ambisonic order, a 5.1 feed); discrete by default for >2.
        void set_layout(const ChannelLayoutInfo &layout);

        u32 channel_count() const override { return channels_; }
        u32 sample_rate() const override { return output_rate_; }
        u32 native_rate() const override { return input_rate_; }
        ChannelLayoutInfo channel_layout() const override;
        u32 read(AudioBuffer &out, u32 frames) override;
        bool finished() const override;
        bool supports_rate() const override { return true; }
        void set_rate(f32 rate) override { rate_ = rate > 0.0f ? rate : 0.0f; }

        // ---- health
        /// Frames waiting to be played.
        [[nodiscard]] u64 buffered_frames() const noexcept;
        [[nodiscard]] f64 buffered_seconds() const noexcept;
        /// Reads that found too little data (the output was padded with silence), and frames dropped to cap latency.
        [[nodiscard]] u64 underruns() const noexcept;
        [[nodiscard]] u64 dropped_frames() const noexcept;
        /// Frames the producer could not store because the ring was full.
        [[nodiscard]] u64 overflowed_frames() const noexcept;
        [[nodiscard]] f64 rate_adjustment() const noexcept;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
        u32 channels_;
        u32 input_rate_;
        u32 output_rate_;
        f32 rate_ = 1.0f;
    };

    /// The historical name (mono, same-rate feeds).
    using RingSource = LiveSource;

} // namespace SFT::Audio
