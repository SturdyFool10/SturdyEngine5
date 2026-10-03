#include <Audio/MediaAudio.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace SFT::Audio {

    MediaAudioStream::MediaAudioStream(u32 channels, u32 sample_rate, u32 engine_rate, const MediaAudioOptions &options)
        : channels_(std::max(channels, 1u)), sample_rate_(std::max(sample_rate, 1u)), options_(options) {
        LiveSourceOptions live;
        live.prefill_frames = static_cast<u32>(options.cushion_seconds * static_cast<f32>(sample_rate_));
        live.max_latency_frames = 0; // a video decoder must never lose audio: it is throttled by `wants_data` instead
        live.adaptive = true;
        source_ = std::make_shared<LiveSource>(channels_, sample_rate_, engine_rate, static_cast<usize>(std::max(options.buffer_seconds, 0.5f) * static_cast<f32>(sample_rate_)), live);
        if (options.layout) {
            source_->set_layout(*options.layout);
        }
    }

    usize MediaAudioStream::push(f64 pts_seconds, const f32 *interleaved, usize frames) {
        if (!started_) {
            started_ = true;
            base_pts_.store(pts_seconds, std::memory_order_release);
            expected_pts_ = pts_seconds;
        }
        usize skip = 0;
        const f64 gap = pts_seconds - expected_pts_;
        if (gap > static_cast<f64>(options_.gap_tolerance_seconds)) {
            // Missing audio between packets (or a stream that starts late): keep the timeline by playing silence for it.
            usize silent = static_cast<usize>(std::llround(gap * static_cast<f64>(sample_rate_)));
            std::vector<f32> zeros(static_cast<usize>(std::min<usize>(silent, 4096)) * channels_, 0.0f);
            while (silent > 0) {
                const usize n = std::min<usize>(silent, zeros.size() / channels_);
                const usize accepted = source_->push(zeros.data(), n);
                pushed_frames_.fetch_add(accepted, std::memory_order_relaxed);
                expected_pts_ += static_cast<f64>(accepted) / static_cast<f64>(sample_rate_);
                if (accepted < n) {
                    return 0; // ring full: the caller retries this packet whole later
                }
                silent -= n;
            }
        } else if (gap < -static_cast<f64>(options_.gap_tolerance_seconds)) {
            // Overlap with what is already queued: drop the repeated frames.
            skip = std::min<usize>(frames, static_cast<usize>(std::llround(-gap * static_cast<f64>(sample_rate_))));
        }
        const usize accepted = skip < frames ? source_->push(interleaved + skip * channels_, frames - skip) : 0;
        pushed_frames_.fetch_add(accepted, std::memory_order_relaxed);
        expected_pts_ += static_cast<f64>(accepted) / static_cast<f64>(sample_rate_);
        return skip + accepted;
    }

    void MediaAudioStream::flush(f64 pts_seconds) {
        // Drop what is queued by reading it out (the ring is single-consumer, so this is the safe way to empty it).
        AudioBuffer discard(channels_, 1024);
        while (source_->read(discard, 1024) > 0) {
        }
        epoch_.fetch_add(1, std::memory_order_relaxed);
        base_pts_.store(pts_seconds, std::memory_order_release);
        pushed_frames_.store(0, std::memory_order_release);
        expected_pts_ = pts_seconds;
        started_ = true;
    }

    void MediaAudioStream::end_of_stream() { source_->close(); }

    bool MediaAudioStream::wants_data() const { return buffered_seconds() < static_cast<f64>(options_.buffer_seconds) * 0.75; }
    f64 MediaAudioStream::buffered_seconds() const { return source_->buffered_seconds(); }

    f64 MediaAudioStream::playback_pts() const {
        const u64 pushed = pushed_frames_.load(std::memory_order_acquire);
        const u64 buffered = source_->buffered_frames();
        const u64 played = pushed > buffered ? pushed - buffered : 0;
        return base_pts_.load(std::memory_order_acquire) + static_cast<f64>(played) / static_cast<f64>(sample_rate_) - latency_.load(std::memory_order_relaxed);
    }

    // ---- clock -----------------------------------------------------------------------------------------------------------------

    f64 MediaClock::wall_seconds() { return std::chrono::duration<f64>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    void MediaClock::follow(std::shared_ptr<MediaAudioStream> audio) { audio_ = std::move(audio); }

    void MediaClock::play(f64 media_time_seconds) {
        anchor_media_ = media_time_seconds;
        anchor_wall_ = wall_seconds();
        paused_ = false;
    }

    void MediaClock::pause() {
        anchor_media_ = now();
        paused_ = true;
    }

    void MediaClock::set_rate(f64 rate) {
        anchor_media_ = now();
        anchor_wall_ = wall_seconds();
        rate_ = rate > 0.0 ? rate : 1.0;
    }

    f64 MediaClock::now() const {
        if (paused_) {
            return anchor_media_;
        }
        // Audio is the master while it has something to play: the speakers are the clock that cannot drift from what is heard.
        if (audio_ && audio_->source()->buffered_frames() > 0) {
            return audio_->playback_pts();
        }
        return anchor_media_ + (wall_seconds() - anchor_wall_) * rate_;
    }

} // namespace SFT::Audio
