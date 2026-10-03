#pragma once

#include <Audio/Decoder.hpp>
#include <Audio/Mixer.hpp>
#include <Audio/Stream.hpp>

#include <Foundation/Foundation.hpp>

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace SFT::Audio {

    class SoundManager;

    /// A waypoint that was just reached.
    struct MarkerHit {
        VoiceId voice = 0;
        UString name;
        u32 id = 0;
        f64 seconds = 0.0;
    };

    class SoundHandle;

    /// Code to run when things happen to a sound. All callbacks run on the game thread inside `SoundManager::update`, never
    /// on the audio thread, so they may do anything (spawn entities, change state, start other sounds).
    struct SoundCallbacks {
        std::function<void(const SoundHandle &)> on_started;
        std::function<void(const SoundHandle &)> on_finished;
        std::function<void(const SoundHandle &)> on_paused;
        /// Any marker, named or not.
        std::function<void(const SoundHandle &, const MarkerHit &)> on_marker;
        std::function<void(const SoundHandle &, f64 seconds)> on_loop;
        std::function<void(const SoundHandle &, f64 seconds)> on_jump;
    };

    /// A cheap, copyable reference to one playing sound. Every operation is a command to the audio thread, so none block;
    /// handles of finished sounds are inert. The manager (and engine) must outlive their handles.
    class SoundHandle {
      public:
        SoundHandle() = default;

        [[nodiscard]] bool valid() const noexcept { return manager_ != nullptr && id_ != 0; }
        [[nodiscard]] VoiceId voice() const noexcept { return id_; }
        /// Pending (waiting for its delayed start), playing, paused or virtualised; false once finished.
        [[nodiscard]] bool alive() const noexcept;
        [[nodiscard]] bool playing() const noexcept;
        [[nodiscard]] bool paused() const noexcept;
        [[nodiscard]] bool finished() const noexcept;
        /// Where playback is, and the total length (0 for endless sources).
        [[nodiscard]] f64 position_seconds() const noexcept;
        [[nodiscard]] f64 duration_seconds() const noexcept;
        [[nodiscard]] f32 volume() const noexcept;
        [[nodiscard]] f32 pitch() const noexcept;

        // ---- volume and pitch (pitch changes speed with it, like a turntable; 2 = an octave up)
        void set_volume(f32 volume) const;
        void set_pitch(f32 pitch) const;
        /// Pitch shift in semitones relative to normal.
        void set_pitch_semitones(f32 semitones) const;
        void fade_volume(f32 target, f32 seconds) const;
        void fade_pitch(f32 target, f32 seconds) const;
        /// Fades out then stops (0 stops immediately).
        void stop(f32 fade_seconds = 0.0f) const;
        void pause() const;
        void resume() const;
        void set_position(const glm::vec3 &position, const glm::vec3 &velocity = glm::vec3(0.0f)) const;

        // ---- timeline
        /// Jumps to a time; a few milliseconds fade in to hide the seam.
        void seek(f64 seconds) const;
        /// Jumps to a named waypoint; false if this sound has no such marker.
        bool seek_to_marker(const ustr &name) const;
        void set_loop(f64 start_seconds, f64 end_seconds, i32 count = -1) const;
        void clear_loop() const;

        // ---- waypoints
        /// Adds a waypoint; returns its id. With an action the mixer carries it out at that exact sample.
        u32 add_marker(const UString &name, f64 seconds, MarkerAction action = MarkerAction::None, f64 jump_seconds = 0.0,
                       bool seamless = false, i32 max_triggers = -1) const;
        /// When playback reaches `marker`, jump to `target_seconds` (once by default; -1 for every pass).
        bool jump_at_marker(const ustr &marker, f64 target_seconds, i32 max_triggers = 1) const;
        void stop_at_marker(const ustr &marker) const;
        void remove_marker(const ustr &name) const;
        /// Runs `callback` every time `marker` is reached.
        void on_marker(const UString &marker, std::function<void(const MarkerHit &)> callback) const;
        [[nodiscard]] bool has_marker(const ustr &name) const;
        [[nodiscard]] f64 marker_seconds(const ustr &name) const;

      private:
        friend class SoundManager;
        SoundHandle(SoundManager *manager, VoiceId id, std::shared_ptr<const VoiceStatus> status)
            : manager_(manager), id_(id), status_(std::move(status)) {}
        SoundManager *manager_ = nullptr;
        VoiceId id_ = 0;
        std::shared_ptr<const VoiceStatus> status_;
    };

    /// The game-thread facade over an `AudioEngine`: starts sounds (from files, buffers, streams or any source), keeps their
    /// named waypoints, and runs callbacks when the audio thread reports events. Call `update()` once per frame.
    class SoundManager {
      public:
        /// `pump_engine`: also free retired voices and collect finished ones (leave false when something else, such as the
        /// engine's `AudioWorld`, already calls `AudioEngine::pump`).
        explicit SoundManager(AudioEngine &engine, bool pump_engine = false);

        [[nodiscard]] AudioEngine &engine() noexcept { return engine_; }

        /// Starts a sound from `params` (source required). Markers in `params` keep their names for handle lookups.
        SoundHandle play(PlayParams params, SoundCallbacks callbacks = {});

        /// Decodes a short file once (cached by path) and plays it; the file's cue points and loop region become markers.
        /// Best for effects: the whole sound is in memory, playback is instant.
        [[nodiscard]] std::expected<SoundHandle, UString> play_file(const std::filesystem::path &path, PlayParams params = {},
                                                                        SoundCallbacks callbacks = {}, bool use_file_markers = true);
        /// Streams a long file (music, ambience) from a decode thread. Same markers; seeks never stall mixing.
        [[nodiscard]] std::expected<SoundHandle, UString> stream_file(const std::filesystem::path &path, PlayParams params = {},
                                                                          const StreamingOptions &options = {}, SoundCallbacks callbacks = {},
                                                                          bool use_file_markers = true);
        /// Plays an already decoded buffer.
        SoundHandle play_buffer(std::shared_ptr<const SampleBuffer> buffer, PlayParams params = {}, SoundCallbacks callbacks = {},
                                bool loop = false, bool use_file_markers = true);

        /// Decodes (and caches) a file without playing it.
        [[nodiscard]] std::expected<std::shared_ptr<const SampleBuffer>, UString> load(const std::filesystem::path &path);
        void clear_cache() { cache_.clear(); }

        /// Delivers audio-thread events to callbacks. Call once per frame.
        void update();

        [[nodiscard]] usize active_sounds() const noexcept { return entries_.size(); }

      private:
        friend class SoundHandle;
        struct Entry {
            std::shared_ptr<const VoiceStatus> status;
            SoundCallbacks callbacks;
            std::unordered_map<UString, u32> ids;
            std::unordered_map<u32, UString> names;
            std::unordered_map<UString, f64> seconds;
            std::unordered_map<UString, std::vector<std::function<void(const MarkerHit &)>>> named_callbacks;
            u32 next_id = 1;
        };
        Entry *find(VoiceId id);
        const Entry *find(VoiceId id) const;
        void register_marker(Entry &entry, const UString &name, u32 id, f64 seconds);

        AudioEngine &engine_;
        bool pump_engine_;
        std::unordered_map<VoiceId, Entry> entries_;
        std::unordered_map<UString, std::shared_ptr<const SampleBuffer>> cache_;
        std::vector<VoiceEvent> event_scratch_;
    };

} // namespace SFT::Audio
