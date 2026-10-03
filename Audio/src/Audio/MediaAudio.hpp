#pragma once

#include <Audio/Source.hpp>

#include <Foundation/Foundation.hpp>

#include <atomic>
#include <memory>

namespace SFT::Audio {

    struct MediaAudioOptions {
        /// Ring size. This is the hard bound on memory: a movie's soundtrack is never held whole, only this much decoded audio.
        f32 buffer_seconds = 2.0f;
        /// How much audio is queued before playback starts, and the level the decoder is asked to keep (`wants_data`).
        f32 cushion_seconds = 0.15f;
        /// Gaps in the timeline longer than this are filled with silence (shorter ones are ignored); overlaps are trimmed.
        f32 gap_tolerance_seconds = 0.010f;
        std::optional<ChannelLayoutInfo> layout;
    };

    /// The audio half of video (or any timestamped media) playback. A demuxer/decoder thread `push`es decoded audio with its
    /// presentation timestamp; the engine plays `source()`; and `playback_pts()` says which instant of the media the speakers
    /// are playing right now, which is the clock the video side steers by. Memory is bounded by the ring however long the
    /// media is, so a soundtrack bigger than RAM streams the same way a short clip does.
    class MediaAudioStream {
      public:
        MediaAudioStream(u32 channels, u32 sample_rate, u32 engine_rate, const MediaAudioOptions &options = {});

        /// Decoder thread: audio whose first frame is presented at `pts_seconds`. Silence is inserted for a timeline gap and
        /// overlapping frames are dropped, so the stream stays aligned with the video. Returns frames accepted (fewer than
        /// offered means the ring is full: wait for `wants_data` and retry the rest).
        usize push(f64 pts_seconds, const f32 *interleaved, usize frames);
        /// A seek or discontinuity: forget everything queued and restart the timeline at `pts_seconds`.
        void flush(f64 pts_seconds);
        void end_of_stream();

        /// True while the queue holds less than the cushion: the decoder should produce more.
        [[nodiscard]] bool wants_data() const;
        [[nodiscard]] f64 buffered_seconds() const;
        [[nodiscard]] std::shared_ptr<LiveSource> source() const { return source_; }

        /// The media time the listener is hearing now: what has been played out of the queue, minus the output device's own
        /// latency (`set_output_latency`). Holds still while starved. Thread safe.
        [[nodiscard]] f64 playback_pts() const;
        void set_output_latency(f64 seconds) { latency_.store(seconds, std::memory_order_relaxed); }

      private:
        std::shared_ptr<LiveSource> source_;
        u32 channels_;
        u32 sample_rate_;
        MediaAudioOptions options_;
        // Timeline bookkeeping (decoder thread writes, any thread reads).
        std::atomic<f64> base_pts_{0.0};        // media time of the first frame pushed since the last flush
        std::atomic<u64> pushed_frames_{0};     // frames queued since the last flush, silence included
        std::atomic<u64> epoch_{0};
        f64 expected_pts_ = 0.0;                // decoder-thread only: where the next push should start
        bool started_ = false;
        std::atomic<f64> latency_{0.0};
    };

    /// A clock for video to steer by: the audio's `playback_pts()` while audio is flowing, otherwise the system clock advancing
    /// from the last known media time (paused, or the stream has no audio). `now()` is what to compare frame timestamps with.
    class MediaClock {
      public:
        /// Follows `audio` while it plays; pass null for a video-only clock.
        void follow(std::shared_ptr<MediaAudioStream> audio);
        void play(f64 media_time_seconds);
        void pause();
        void set_rate(f64 rate); ///< 1 = normal speed (the audio side must be given matching content)
        [[nodiscard]] f64 now() const;
        [[nodiscard]] bool paused() const noexcept { return paused_; }
        /// Seconds until a frame presented at `frame_pts` should be shown (negative: it is already late).
        [[nodiscard]] f64 until(f64 frame_pts) const { return (frame_pts - now()) / rate_; }
        /// True when a frame is so late (beyond `tolerance_seconds`) that it should be dropped instead of shown.
        [[nodiscard]] bool should_drop(f64 frame_pts, f64 tolerance_seconds = 0.040) const { return now() - frame_pts > tolerance_seconds; }

      private:
        std::shared_ptr<MediaAudioStream> audio_;
        bool paused_ = true;
        f64 rate_ = 1.0;
        f64 anchor_media_ = 0.0;
        f64 anchor_wall_ = 0.0;
        [[nodiscard]] static f64 wall_seconds();
    };

} // namespace SFT::Audio
