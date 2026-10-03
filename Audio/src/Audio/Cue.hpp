#pragma once

#include <Audio/Sound.hpp>

#include <Foundation/Foundation.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace SFT::Audio {

    /// One recording a cue can choose.
    struct CueVariation {
        std::shared_ptr<const SampleBuffer> sound;
        /// Relative chance for Random selection.
        f32 weight = 1.0f;
        f32 volume = 1.0f;
    };

    enum class CueSelection : u8 {
        Random,         // weighted random
        RandomNoRepeat, // weighted random, never the same twice in a row
        RoundRobin,     // in order, wrapping
        Layered,        // all of them at once (a gunshot = crack + body + tail)
    };

    /// What to do when a cue is asked to play while at its instance limit.
    enum class CueLimit : u8 {
        RejectNew,  // ignore the request
        StopOldest, // make room by fading out the oldest instance
    };

    /// A designer-facing sound: several variations, randomised volume and pitch, instance limits and a cooldown, so a hundred
    /// footsteps do not sound like one sample played a hundred times.
    struct SoundCue {
        UString name;
        std::vector<CueVariation> variations;
        CueSelection selection = CueSelection::RandomNoRepeat;
        f32 volume = 1.0f;
        /// Random volume scale in [1 - v, 1 + v].
        f32 volume_variation = 0.0f;
        f32 pitch = 1.0f;
        /// Random pitch offset in semitones in [-p, +p].
        f32 pitch_variation_semitones = 0.0f;
        /// Most simultaneous instances (0 = unlimited) and what happens beyond it.
        u32 max_instances = 0;
        CueLimit limit = CueLimit::StopOldest;
        /// Minimum time between two starts of this cue.
        f32 cooldown_seconds = 0.0f;
        /// Base playback settings (bus, distance model, spatialisation, priority, ...); `source`, `volume` and `pitch` are
        /// filled in by the player.
        PlayParams base;
    };

    /// Plays cues through a `SoundManager`, deterministically (seeded) and with instance and cooldown bookkeeping.
    class CuePlayer {
      public:
        explicit CuePlayer(SoundManager &sounds, u64 seed = 1) : sounds_(sounds), state_(seed != 0 ? seed : 1) {}

        /// Plays `cue` at the base parameters (override position etc. through `overrides`). Returns the first instance's
        /// handle (invalid when rejected or empty). `now_seconds` drives cooldowns (use the engine clock or game time).
        SoundHandle play(const SoundCue &cue, f64 now_seconds, const PlayParams &overrides = {});

        /// Instances of `cue` still sounding.
        [[nodiscard]] u32 active_instances(const SoundCue &cue);
        void stop_all(const SoundCue &cue, f32 fade_seconds = 0.0f);

      private:
        struct State {
            std::vector<SoundHandle> instances;
            usize next = 0;
            usize last = ~usize{0};
            f64 last_start = -1e30;
        };

        [[nodiscard]] f32 random01();
        [[nodiscard]] usize pick(const SoundCue &cue, State &state);

        SoundManager &sounds_;
        u64 state_;
        std::unordered_map<const SoundCue *, State> states_;
    };

} // namespace SFT::Audio
