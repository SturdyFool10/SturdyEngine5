#pragma once

#include <Async/Scheduler.hpp>
#include <Audio/Acoustics.hpp>
#include <Audio/DeviceSink.hpp>
#include <Audio/Mixer.hpp>
#include <Audio/Sound.hpp>
#include <AudioGpu/GpuComputeMixer.hpp>
#include <Audio/Source.hpp>

#include <Ecs/Entity.hpp>
#include <Ecs/Event.hpp>
#include <Ecs/Resource.hpp>
#include <Engine/Asset.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SFT::Engine {

    class AssetManager;

    /// A compute-mixing backend that runs on `device` (normally `Engine::rhi_device()`), for
    /// `AudioEngineConfig::compute_backend`: with it, voices that are plain loaded samples are mixed on the GPU once a block
    /// has `compute_min_voices` of them, which is what makes tens of thousands of simultaneous sounds affordable. Returns an
    /// error (and no backend) when the device cannot run the kernels; leave `compute_backend` empty then. The backend borrows
    /// the device: disable audio (or drop the backend) before the graphics backend is switched or torn down.
    [[nodiscard]] std::expected<std::shared_ptr<Audio::ComputeMixBackend>, UString> create_gpu_audio_mixer(RHI::RhiDevice *device, const AudioGpu::GpuMixerConfig &config = {});

    /// Marks the entity (normally the camera) whose position and orientation define what is heard. With several, the one
    /// with the highest `priority` wins. The entity needs a `WorldTransform`.
    struct AudioListener {
        f32 priority = 0.0f;
        bool enabled = true;
    };

    /// How a sound is anchored. Choose per source:
    ///  - `World`: fixed at `AudioSource::offset` interpreted as a world position (a radio in a room, an explosion).
    ///  - `Entity`: follows the owning entity's `WorldTransform` plus `offset` in its local space (footsteps, a speaking
    ///    NPC, an engine). Parent it to any entity and it moves, turns and gets Doppler with it.
    ///  - `Listener`: glued to the listener's head at `offset` in listener space (+X right, +Y up, -Z forward): the
    ///    player's own voice lines, UI cues and music beds never trail behind as the player moves.
    enum class AudioAttachment : u8 { World, Entity, Listener };

    /// Something that happened to an `AudioSource` (a waypoint reached, a loop wrapped, a jump, the end), sent through the ECS
    /// event stream so any system can react: open a door on a "door_slam" marker, spawn a step decal on "footfall".
    struct AudioSourceEvent {
        enum class Kind : u8 { Marker, Loop, Jump, Finished };
        Ecs::Entity entity{};
        Kind kind = Kind::Marker;
        /// The marker's name (empty for unnamed markers and for non-marker events).
        UString name;
        u32 marker_id = 0;
        f64 seconds = 0.0;
    };

    /// A sound emitter. Give it a decoded `sound` (see `sound_buffer`) or any `stream` (procedural synthesis, a
    /// streaming decoder, voice chat); it starts on its first update and keeps following its anchor until it ends.
    /// Needs a `WorldTransform` on the same entity (unused for `World` and `Listener` anchoring).
    struct AudioSource {
        std::shared_ptr<const Audio::SampleBuffer> sound;
        /// A source instance; takes precedence over `sound` and plays once (set a new one to replay).
        std::shared_ptr<Audio::DataSource> stream;
        bool loop = false;
        bool play_on_spawn = true;

        AudioAttachment attachment = AudioAttachment::Entity;
        glm::vec3 offset{0.0f};

        f32 volume = 1.0f;
        f32 pitch = 1.0f;
        Audio::BusId bus = 0;
        /// False plays un-positioned (music, UI): attachment is then irrelevant.
        bool spatial = true;
        Audio::DistanceModel distance{};
        f32 spread = 0.0f;
        f32 priority = 1.0f;
        f32 doppler_factor = 1.0f;
        Audio::Panner panner = Audio::Panner::Vbap;
        Audio::BusId aux_bus = Audio::no_bus;
        f32 aux_send = 0.0f;
        /// Let the acoustics provider (if one is set) occlude/reverberate this source.
        bool use_acoustics = true;
        /// Per-source control of the raytraced effects: which take part (bits of `Audio::AcousticEffect`; clear one to exempt
        /// this source, e.g. no muffling for a narrator) and how strongly (multiplying the global settings).
        u32 acoustic_effects = Audio::all_acoustic_effects;
        f32 muffling_scale = 1.0f;
        f32 reverb_scale = 1.0f;
        f32 delay_scale = 1.0f;
        f32 doppler_scale = 1.0f;
        /// Physical size of the source (metres): how much of it must be hidden before it counts as occluded.
        f32 acoustic_radius = 0.25f;

        // Timeline (decoded and streamed files): waypoints, a loop region and a start offset.
        std::vector<Audio::MarkerSpec> markers;
        std::optional<Audio::LoopSpec> loop_region;
        f64 start_seconds = 0.0;
        /// Also add the cue points and loop region embedded in `sound`'s file.
        bool use_file_markers = true;

        // Runtime state (read-only for gameplay).
        Audio::VoiceId voice = 0;
        /// Control the playing sound from gameplay code: seek, pitch, volume fades, markers, callbacks.
        Audio::SoundHandle handle;
        bool finished = false;
        f32 applied_volume = -1.0f;
        f32 applied_pitch = -1.0f;
        bool has_previous_position = false;
        glm::vec3 previous_position{0.0f};
    };

    /// Everything audio the engine owns: the mixer, the device sink, the optional acoustics provider. Resource of the
    /// ECS world; systems keep it in step with the entities.
    class AudioWorld {
      public:
        /// Creates the mixer; `open_device` also starts playback on the default (and any configured) devices.
        /// Returns an error string on failure, empty on success.
        UString enable(Audio::AudioEngineConfig config, bool open_device = true, const Audio::DeviceSinkConfig &devices = {});
        void disable();
        [[nodiscard]] bool enabled() const noexcept { return engine_ != nullptr; }
        [[nodiscard]] Audio::AudioEngine *engine() noexcept { return engine_.get(); }
        [[nodiscard]] const Audio::AudioEngine *engine() const noexcept { return engine_.get(); }

        /// Optional raytraced (or any other) acoustics; null (the default) means plain distance attenuation.
        /// `max_delay_seconds` sizes the per-voice delay line propagation delay needs (voices created afterwards get it).
        void set_acoustics(std::shared_ptr<Audio::AcousticsProvider> provider, f32 max_delay_seconds = 0.5f) {
            finish_acoustics_job();
            acoustics_ = std::move(provider);
            if (engine_ && acoustics_) {
                engine_->set_acoustics_delay_capacity(max_delay_seconds);
            }
            delay_capacity_ = max_delay_seconds;
        }
        /// An aux reverb bus (`AudioEngine::add_reverb_bus`) whose decay follows the room the listener is in, as measured by the
        /// acoustics provider. Pass `Audio::no_bus` to stop.
        void set_reverb_bus(Audio::BusId bus) noexcept { reverb_bus_ = bus; }
        [[nodiscard]] bool has_acoustics() const noexcept { return acoustics_ != nullptr; }
        /// Evaluates acoustics on a scheduler worker (the default) so tracing rays never stretches a frame; results are applied at the
        /// next `end_frame` after the job finishes, so they trail by a frame or two (the mixer smooths them anyway). Off evaluates inline,
        /// deterministically (tests, offline rendering).
        void set_acoustics_async(bool enabled) {
            finish_acoustics_job();
            acoustics_async_ = enabled;
        }
        /// Runs `edit(provider)` with no evaluation in flight: the only safe way to change a provider's settings or scene while
        /// the engine is running (`RaycastAcoustics::set_settings`, `set_scene`).
        template <class Fn>
        void edit_acoustics(Fn &&edit) {
            finish_acoustics_job();
            if (acoustics_) {
                std::forward<Fn>(edit)(*acoustics_);
            }
        }
        ~AudioWorld() { finish_acoustics_job(); }
        AudioWorld() = default;
        AudioWorld(AudioWorld &&) = default;
        AudioWorld &operator=(AudioWorld &&) = default;
        /// Seconds between acoustics evaluations of the voices (default 0.05 = 20 Hz), and how many voices per pass.
        void set_acoustics_rate(f32 interval_seconds, u32 voices_per_pass) noexcept {
            acoustics_interval_ = interval_seconds;
            acoustics_budget_ = voices_per_pass;
        }

        // ---- driven by the engine's systems ----
        struct TrackedVoice {
            Audio::VoiceId voice = 0;
            glm::vec3 position{0.0f};
            f32 radius = 0.25f;
            glm::vec3 velocity{0.0f};
            u32 effects = Audio::all_acoustic_effects;
            f32 muffling_scale = 1.0f, reverb_scale = 1.0f, delay_scale = 1.0f, doppler_scale = 1.0f;
        };
        void begin_frame();
        void offer_listener(f32 priority, const glm::mat4 &transform);
        /// Pushes the chosen listener, collects finished voices and runs acoustics when due.
        void end_frame(f32 delta_seconds);
        void track(const TrackedVoice &voice) { tracked_.push_back(voice); }
        [[nodiscard]] bool voice_finished(Audio::VoiceId voice) const { return finished_.contains(voice); }
        [[nodiscard]] const Audio::ListenerState &listener() const noexcept { return listener_; }
        /// The game-thread sound facade (null until `enable`): file loading, named waypoints, callbacks, cues and music
        /// all build on it.
        [[nodiscard]] Audio::SoundManager *sounds() noexcept { return sounds_.get(); }
        /// Events gathered from sources this frame, for the ECS system that forwards them.
        void push_event(AudioSourceEvent event) { source_events_.push_back(std::move(event)); }
        [[nodiscard]] std::vector<AudioSourceEvent> take_events() { return std::exchange(source_events_, {}); }

      private:
        /// An evaluation running on a worker, and what it will hand back.
        struct PendingAcoustics {
            std::vector<Audio::VoiceId> voices;
            std::vector<Audio::AcousticsResult> results;
            Audio::RoomEstimate room;
            bool has_room = false;
        };
        /// Waits for a running evaluation and applies its results.
        void finish_acoustics_job();
        /// Applies results of a finished evaluation if there is one.
        void poll_acoustics_job();
        void apply_acoustics(const PendingAcoustics &done);

        std::unique_ptr<Audio::AudioEngine> engine_;
        std::unique_ptr<Audio::SoundManager> sounds_;
        std::vector<AudioSourceEvent> source_events_;
        std::unique_ptr<Audio::DeviceSink> sink_;
        std::shared_ptr<Audio::AcousticsProvider> acoustics_;
        Audio::ListenerState listener_;
        bool has_listener_ = false;
        bool listener_ready_ = false;
        f32 best_priority_ = -1e30f;
        glm::mat4 best_transform_{1.0f};
        std::unordered_set<Audio::VoiceId> finished_;
        std::vector<TrackedVoice> tracked_;
        f32 delay_capacity_ = 0.5f;
        Audio::BusId reverb_bus_ = Audio::no_bus;
        f32 reverb_rt60_ = 0.0f, reverb_damping_ = 0.3f;
        f32 acoustics_interval_ = 0.05f;
        u32 acoustics_budget_ = 16;
        f32 acoustics_clock_ = 0.0f;
        bool acoustics_async_ = true;
        std::shared_ptr<PendingAcoustics> acoustics_pending_;
        std::optional<Async::TaskHandle<void>> acoustics_task_;
        /// What each voice was last evaluated with, so unchanged voices are skipped and the rest are scheduled by staleness.
        struct EvaluationState {
            glm::vec3 position{0.0f};
            glm::vec3 listener{0.0f};
            u32 age = 0;
            bool evaluated = false;
        };
        std::unordered_map<Audio::VoiceId, EvaluationState> evaluated_;
    };

    /// Wraps a loaded sound asset for playback without copying its samples. Empty on a wrong or stale asset.
    [[nodiscard]] std::shared_ptr<const Audio::SampleBuffer> sound_buffer(const AssetManager &assets, Asset sound);

    /// Per-update work of the audio systems, exposed for the engine wiring and for tests.
    void update_audio_source(AudioWorld &world, Ecs::Entity entity, AudioSource &source, const glm::mat4 &transform, f32 delta_seconds);

} // namespace SFT::Engine

SFT_ECS_COMPONENT(SFT::Engine::AudioListener, "sturdy.engine.audio_listener");
SFT_ECS_EVENT(SFT::Engine::AudioSourceEvent, "sturdy.engine.audio_source_event");
SFT_ECS_COMPONENT(SFT::Engine::AudioSource, "sturdy.engine.audio_source");
SFT_ECS_RESOURCE(SFT::Engine::AudioWorld, "sturdy.engine.audio_world");
