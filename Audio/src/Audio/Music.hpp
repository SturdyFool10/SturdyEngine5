#pragma once

#include <Audio/Sound.hpp>
#include <Audio/Tempo.hpp>

#include <Foundation/Foundation.hpp>

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace SFT::Audio {

    /// A piece of music: how to open it (a fresh source each time it plays) plus what the player needs to cut it up
    /// musically.
    struct MusicTrack {
        UString name;
        /// Creates a new source for each playback (a streaming decoder, a buffer source...).
        std::function<std::expected<std::shared_ptr<DataSource>, UString>()> open;
        /// Tempo grid for quantised transitions; `bpm` 0 means no grid (transitions happen immediately).
        f64 bpm = 0.0;
        u32 beats_per_bar = 4;
        f64 first_beat_seconds = 0.0;
        /// A tempo map for music whose tempo or time signature changes; takes the place of `bpm`/`beats_per_bar`/`first_beat_seconds`.
        std::shared_ptr<const TempoMap> tempo;
        /// The grid this track is cut on: the tempo map, or the constant one described by the three fields above (empty when none).
        [[nodiscard]] TempoMap grid() const { return tempo ? *tempo : (bpm > 0.0 ? TempoMap::constant(bpm, beats_per_bar, first_beat_seconds) : TempoMap{}); }
        std::optional<LoopSpec> loop;
        std::vector<MarkerSpec> markers;
        f32 volume = 1.0f;
    };

    /// A track that streams `path` from disk (and loops the whole file when `loop` is set).
    [[nodiscard]] MusicTrack stream_track(const std::filesystem::path &path, u32 engine_rate, bool loop = true, f64 bpm = 0.0);
    /// A track played from a decoded buffer.
    [[nodiscard]] MusicTrack buffer_track(std::shared_ptr<const SampleBuffer> buffer, u32 engine_rate, bool loop = true, f64 bpm = 0.0);

    enum class Quantize : u8 { Immediate, NextBeat, NextBar };

    struct MusicTransition {
        /// Overlap in seconds: the old track fades out while the new one fades in.
        f32 crossfade_seconds = 1.0f;
        /// Wait for the current track's next beat/bar (needs its `bpm`) before switching.
        Quantize quantize = Quantize::Immediate;
    };

    /// Plays music for a game: crossfades, beat/bar-quantised switches, gapless queues, stingers. Everything that must be
    /// sample accurate (the fade-out starting on the beat, the next track starting the instant the last ends) is scheduled
    /// into the mixer rather than done from the frame loop.
    class MusicPlayer {
      public:
        MusicPlayer(SoundManager &sounds, BusId bus = 0) : sounds_(sounds), bus_(bus) {}

        /// Starts `track`, replacing whatever is playing according to `transition`.
        bool play(const MusicTrack &track, const MusicTransition &transition = {});
        /// Plays `track` when the current one ends. With a crossfade the two overlap; with 0 the join is gapless.
        /// (A looping current track never ends: use `play` with a transition instead.)
        void queue(const MusicTrack &track, f32 crossfade_seconds = 0.0f);
        void stop(f32 fade_seconds = 1.0f);
        void pause();
        void resume();
        /// Master music volume, optionally faded.
        void set_volume(f32 volume, f32 fade_seconds = 0.0f);
        /// Plays a one-shot musical hit on the next beat/bar of the current track.
        SoundHandle play_stinger(std::shared_ptr<const SampleBuffer> stinger, Quantize quantize = Quantize::NextBeat, f32 volume = 1.0f);

        /// Call once per frame.
        void update();

        [[nodiscard]] SoundHandle current() const { return current_; }
        [[nodiscard]] bool playing() const { return current_.alive(); }
        [[nodiscard]] const UString &current_name() const { return current_name_; }
        /// Beats since the current track's first beat (0 without a tempo grid).
        [[nodiscard]] f64 beat_position() const;
        /// Seconds until the current track's next grid point.
        [[nodiscard]] f64 seconds_to_next(Quantize quantize) const;

      private:
        bool start(const MusicTrack &track, f32 delay_seconds, f64 start_at_clock, f32 fade_in_seconds, SoundHandle &out);

        SoundManager &sounds_;
        BusId bus_;
        f32 volume_ = 1.0f;
        SoundHandle current_;
        MusicTrack current_track_;
        UString current_name_;
        // A queued track and, once scheduled into the mixer, the handle that will take over.
        std::optional<MusicTrack> queued_;
        f32 queued_crossfade_ = 0.0f;
        SoundHandle scheduled_;
        std::optional<MusicTrack> scheduled_track_;
    };

} // namespace SFT::Audio
