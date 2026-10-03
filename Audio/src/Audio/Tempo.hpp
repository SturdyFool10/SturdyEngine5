#pragma once

#include <Audio/Mixer.hpp>

#include <vector>

/// A tempo map: music whose speed or time signature changes (a ritardando-free but multi-section score, a game cue that shifts from 90
/// to 140 bpm). Beats are counted from the first beat; every change takes effect at its beat and a new time signature starts a new bar
/// there. Converts both ways between beats and seconds, finds the next beat or bar line, and lays down marker waypoints on the grid.
namespace SFT::Audio {

    struct TempoChange {
        f64 beat = 0.0;
        f64 bpm = 120.0;
        u32 beats_per_bar = 4;
    };

    class TempoMap {
      public:
        /// One tempo throughout, with the first beat `first_beat_seconds` into the audio.
        [[nodiscard]] static TempoMap constant(f64 bpm, u32 beats_per_bar = 4, f64 first_beat_seconds = 0.0);

        TempoMap() = default;
        explicit TempoMap(std::vector<TempoChange> changes, f64 first_beat_seconds = 0.0);
        /// Adds (or replaces, at the same beat) a change.
        void add(const TempoChange &change);
        void set_first_beat_seconds(f64 seconds);

        [[nodiscard]] bool empty() const noexcept { return changes_.empty(); }
        [[nodiscard]] std::span<const TempoChange> changes() const noexcept { return changes_; }
        [[nodiscard]] f64 first_beat_seconds() const noexcept { return first_beat_seconds_; }

        /// Seconds into the audio at `beat` (negative beats extend the first tempo backwards).
        [[nodiscard]] f64 seconds_at_beat(f64 beat) const;
        [[nodiscard]] f64 beat_at_seconds(f64 seconds) const;
        [[nodiscard]] f64 bpm_at_beat(f64 beat) const;
        [[nodiscard]] u32 beats_per_bar_at(f64 beat) const;
        /// The first beat (or bar line) strictly after `beat`, with a hair of tolerance so standing on a line counts as being at it.
        [[nodiscard]] f64 next_beat(f64 beat) const;
        [[nodiscard]] f64 next_bar(f64 beat) const;
        /// `beat` itself when it is on a beat (or bar) line, else the next one: what a quantised start waits for.
        [[nodiscard]] f64 beat_at_or_after(f64 beat) const;
        [[nodiscard]] f64 bar_at_or_after(f64 beat) const;
        /// Beat within the bar (0 = the downbeat) and the bar number (0 for the first bar).
        [[nodiscard]] f64 beat_in_bar(f64 beat) const;
        [[nodiscard]] u32 bar_at(f64 beat) const;

        /// Waypoints named "<prefix>N" on every `every_n` beats from `first_beat` up to `last_beat`, for `PlayParams::markers` (ids from `first_id`).
        [[nodiscard]] std::vector<MarkerSpec> beat_markers(f64 first_beat, f64 last_beat, u32 every_n = 1, const UString &prefix = "beat", u32 first_id = 1) const;

      private:
        [[nodiscard]] usize segment_for_beat(f64 beat) const;
        void rebuild();

        std::vector<TempoChange> changes_;
        std::vector<f64> start_seconds_; ///< seconds (relative to the first beat) at each change
        f64 first_beat_seconds_ = 0.0;
    };

} // namespace SFT::Audio
