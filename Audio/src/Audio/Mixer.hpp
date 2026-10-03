#pragma once

#include <Audio/Acoustics.hpp>
#include <Audio/AudioBuffer.hpp>
#include <Audio/Compute.hpp>
#include <Audio/Dsp.hpp>
#include <Audio/Effects.hpp>
#include <Audio/Source.hpp>
#include <Audio/Spatial.hpp>

#include <Foundation/Foundation.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace SFT::Audio {

    using VoiceId = u32;   // 0 is never a valid voice
    using BusId = u32;
    using OutputId = u32;
    inline constexpr BusId no_bus = 0xFFFFFFFFu;
    inline constexpr OutputId primary_output = 0;

    /// Where a source lives. `World` sources sit at a world position (give them an entity to follow and they move with
    /// it); `ListenerRelative` sources are glued to the listener's head, so a player's own voice line or a UI cue never
    /// trails behind as they move or turn. Listener-relative positions are in listener space: +X right, +Y up, -Z forward.
    enum class SourceSpace : u8 { World, ListenerRelative };

    /// How a spatial voice is placed on a speaker output.
    enum class Panner : u8 {
        Vbap,       // sharp pairs/triplets of speakers
        Ambisonic,  // encoded to third-order ambisonics and decoded, a softer, more enveloping image
    };

    // ---- outputs ---------------------------------------------------------------------------------------------------

    struct OutputDesc {
        enum class Kind : u8 {
            /// Loudspeakers in `layout` (stereo, 5.1, 7.1, 7.1.4, 9.1.6, ...).
            Speakers,
            /// Headphones: stereo through a binaural filter per voice.
            Binaural,
            /// Atmos-class object rendering: a static bed in `layout` plus up to `max_objects` positioned mono objects with
            /// ADM-style metadata, handed to a platform renderer (Windows Spatial Audio, a Dolby-capable AVR, an IAMF
            /// writer). The engine needs no licensed code for this: it only supplies beds, objects and metadata.
            Objects,
        };
        Kind kind = Kind::Speakers;
        UString name = "main";
        SpeakerLayout layout = SpeakerLayout::stereo();
        u32 max_objects = 128;
        /// Set by whatever consumes `AudioEngine::objects()` (a platform spatial renderer). While false, positioned voices
        /// are panned into the bed with VBAP, so a plain device sink still plays an Objects output correctly.
        bool objects_consumed = false;
        /// Binaural only; null selects the built-in spherical-head model. Supply a SOFA-backed filter for measured HRTFs.
        BinauralFilterFactory binaural;
        /// Ambisonic order used by voices that request `Panner::Ambisonic` (1-3).
        u32 ambisonic_order = 3;
    };

    /// Per-object metadata for an Objects output, valid for the block it was rendered in.
    struct ObjectMetadata {
        VoiceId voice = 0;
        /// Position in listener space (metres): +X right, +Y up, -Z forward.
        glm::vec3 position{0.0f};
        /// 0 = point source, 1 = fully diffuse (size/spread).
        f32 spread = 0.0f;
        f32 gain = 1.0f;
    };

    struct ObjectBlock {
        AudioBuffer samples; // one mono channel per object slot
        std::vector<ObjectMetadata> metadata;
        u32 active = 0;
    };

    // ---- waypoints, jumps, status ---------------------------------------------------------------------------------

    /// What a waypoint does when playback reaches it (always sample-accurate: the mixer splits the block at the marker).
    enum class MarkerAction : u8 {
        None,  // just report it
        Jump,  // continue from `jump_seconds`
        Stop,  // end the voice here
        Pause, // pause the voice here
        /// Fade the voice out over `jump_seconds`, then end it (the sample-accurate half of a crossfade).
        FadeStop,
    };

    /// A waypoint at a position of a voice's audio. `id` comes back in events; give each marker of a voice its own.
    struct MarkerSpec {
        u32 id = 0;
        /// Name for game-side lookup (the audio thread only uses `id`).
        UString name;
        f64 seconds = 0.0;
        MarkerAction action = MarkerAction::None;
        f64 jump_seconds = 0.0;
        /// Jumps normally get a few milliseconds of fade across the seam; set for sample-exact loops that are already seamless.
        bool seamless = false;
        /// Stops firing after this many hits (-1 = every time playback reaches it).
        i32 max_triggers = -1;
        /// Report each hit to the game thread (`VoiceEvent`).
        bool fire_event = true;
    };

    /// A loop between two points of a seekable source (music loops, engine sustains, ambience beds).
    struct LoopSpec {
        f64 start_seconds = 0.0;
        f64 end_seconds = 0.0;
        /// Loops to play before continuing past the end (-1 = forever).
        i32 count = -1;
    };

    /// Something that happened to a voice, delivered to the game thread by `AudioEngine::poll_events`.
    struct VoiceEvent {
        enum class Kind : u8 {
            Started,   // a scheduled voice began sounding
            MarkerHit, // playback crossed a marker (`marker_id`)
            Jumped,    // a Jump marker or a seek moved playback
            Looped,    // a loop region wrapped
            Paused,    // a Pause marker paused the voice
            Finished,  // the voice ended (source exhausted, Stop marker, or stopped)
        };
        VoiceId voice = 0;
        Kind kind = Kind::Finished;
        u32 marker_id = 0;
        f64 position_seconds = 0.0;
    };

    /// Live, lock-free readout of a voice: where it is and what state it is in. Shared with the audio thread, which updates
    /// it once per block, so reading it from game code never blocks anything.
    class VoiceStatus {
      public:
        enum class State : u32 { Pending, Playing, Paused, Virtual, Finished };
        [[nodiscard]] f64 position_seconds() const noexcept { return position_.load(std::memory_order_relaxed); }
        [[nodiscard]] f64 duration_seconds() const noexcept { return duration_.load(std::memory_order_relaxed); }
        [[nodiscard]] State state() const noexcept { return static_cast<State>(state_.load(std::memory_order_relaxed)); }
        [[nodiscard]] bool finished() const noexcept { return state() == State::Finished; }
        [[nodiscard]] bool playing() const noexcept { return state() == State::Playing; }
        [[nodiscard]] f32 volume() const noexcept { return volume_.load(std::memory_order_relaxed); }
        [[nodiscard]] f32 pitch() const noexcept { return pitch_.load(std::memory_order_relaxed); }
        /// Channels of the voice's source.
        [[nodiscard]] u32 channels() const noexcept { return channels_.load(std::memory_order_relaxed); }

        /// Written by the audio thread once per block (engine-internal).
        void publish(f64 position, State state, f32 volume, f32 pitch) noexcept {
            position_.store(position, std::memory_order_relaxed);
            state_.store(static_cast<u32>(state), std::memory_order_relaxed);
            volume_.store(volume, std::memory_order_relaxed);
            pitch_.store(pitch, std::memory_order_relaxed);
        }
        void set_duration(f64 seconds) noexcept { duration_.store(seconds, std::memory_order_relaxed); }
        void set_channels(u32 channels) noexcept { channels_.store(channels, std::memory_order_relaxed); }

      private:
        std::atomic<f64> position_{0.0};
        std::atomic<f64> duration_{0.0};
        std::atomic<u32> state_{0};
        std::atomic<f32> volume_{1.0f};
        std::atomic<f32> pitch_{1.0f};
        std::atomic<u32> channels_{0};
    };

    struct BusLevels {
        f32 peak = 0.0f;
        f32 rms = 0.0f;
    };

    // ---- voices ----------------------------------------------------------------------------------------------------

    struct PlayParams {
        std::shared_ptr<DataSource> source;
        /// Bus the voice plays through (and thereby the output it reaches). Default: the primary output's master.
        BusId bus = 0;
        /// Extra buses to also play through (e.g. the same sound on the TV and on a headset); each may be on a different output.
        std::vector<BusId> mirror_buses;
        f32 volume = 1.0f;
        f32 pitch = 1.0f;
        SourceSpace space = SourceSpace::World;
        /// False plays the source un-positioned (UI, music, stereo beds), regardless of `space`.
        bool spatial = true;
        glm::vec3 position{0.0f};
        glm::vec3 velocity{0.0f};
        DistanceModel distance{};
        /// 0..1 widens the source toward all speakers (a large or very close source).
        f32 spread = 0.0f;
        /// Scales audibility when more voices want to play than there are physical voices.
        f32 priority = 1.0f;
        f32 doppler_factor = 1.0f;
        Panner panner = Panner::Vbap;
        /// Send to an aux bus (reverb) and its level.
        BusId aux_bus = no_bus;
        f32 aux_send = 0.0f;
        /// Allow the acoustics provider to shape this voice (occlusion, detours, reverb send).
        bool use_acoustics = true;

        // ---- timeline (needs a seekable source: decoded or streamed files)
        /// Waypoints: reported, and optionally acted on, the instant playback reaches them.
        std::vector<MarkerSpec> markers;
        std::optional<LoopSpec> loop;
        /// Begin this far into the audio.
        f64 start_seconds = 0.0;
        /// Start after this delay (sample accurate, from when the engine receives the command).
        f32 start_delay_seconds = 0.0f;
        /// Start exactly at this engine clock time (`AudioEngine::clock_seconds`); negative = now. Overrides the delay.
        f64 start_at_seconds = -1.0;
        /// Fade across jumps and seeks to avoid clicks.
        f32 declick_seconds = 0.003f;
        /// Fade the voice in from silence over this long once it actually starts (after any delay).
        f32 fade_in_seconds = 0.0f;

        /// Effects applied to this voice alone, in order, before it is spatialised or mixed (a low-pass on a muffled voice, a
        /// noise gate on a microphone, a de-esser on dialogue). Built on the game thread; the audio thread only runs them.
        std::vector<EffectSpec> effects;
        /// What the source's channels are; by default the source says (files report their speaker mask, mics are discrete).
        /// Ambisonic sources are rotated with the listener and decoded; speaker layouts are folded onto the output by role.
        std::optional<ChannelLayoutInfo> source_layout;
    };

    /// The cue points and loop region a decoded file carries, as `PlayParams` fields (marker ids count from 1).
    void apply_file_metadata(PlayParams &params, const SampleBuffer &buffer, bool use_loop = true);

    struct ListenerState {
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 velocity{0.0f};
    };

    /// Turns a bus into an aux reverb: whatever is sent to it comes out as FDN reverb spread over the output layout.
    [[nodiscard]] std::unique_ptr<BusEffect> make_reverb_effect(f32 sample_rate, const SpeakerLayout &layout, f32 rt60, f32 size = 1.0f,
                                                                f32 damping = 0.3f);
    [[nodiscard]] std::unique_ptr<BusEffect> make_limiter_effect(f32 sample_rate, f32 ceiling = 0.98f);
    [[nodiscard]] std::unique_ptr<BusEffect> make_compressor_effect(f32 sample_rate, f32 threshold_db, f32 ratio, f32 attack_seconds,
                                                                   f32 release_seconds, f32 makeup_db = 0.0f);

    struct AudioEngineConfig {
        u32 sample_rate = 48000;
        u32 block_frames = 256;
        /// Voices actually mixed each block; the loudest (by gain x priority) win and the rest keep their place silently.
        /// Mixing is cheap and spreads over `mix_threads`, so thousands of simultaneous sounds are routine.
        u32 max_physical_voices = 512;
        /// Voices that may exist at once (playing or virtual). Memory is a few KB per voice that has played, reused afterwards.
        u32 max_voices = 8192;
        /// Commands buffered between the game and audio threads per block (plays, volume changes, positions...).
        u32 command_capacity = 65536;
        /// Extra threads that help mix voices when many are audible (0 = mix on the audio thread alone, UINT32_MAX = choose
        /// from the machine's cores). The audio thread always takes a share, so 2 means three mixers in total.
        u32 mix_threads = 0xFFFFFFFFu;
        /// Below this many audible voices a block is mixed on the audio thread alone: waking threads costs more than it saves.
        u32 parallel_voice_threshold = 96;
        /// Longest acoustic detour delay a voice can be given (seconds). Zero (the default) leaves voices without the delay
        /// line, which costs ~100 KB each; the acoustics layer raises it when it is in use.
        f32 acoustics_delay_seconds = 0.0f;
        /// Compute-mode mixing (see Compute.hpp). When set, blocks with at least `compute_min_voices` audible voices hand the
        /// voices that qualify (plain loaded samples with no per-voice effects, filters, delay or markers) to the backend and
        /// mix only the rest on the CPU. Null (the default) keeps every voice on the CPU.
        std::shared_ptr<ComputeMixBackend> compute_backend;
        u32 compute_min_voices = 512;
        /// At least one; output 0 is the primary and drives the clock.
        std::vector<OutputDesc> outputs;
        f32 speed_of_sound = 343.0f;
    };

    struct AudioStats {
        u32 voices = 0;
        u32 physical = 0;
        u32 virtualised = 0;
        u32 dropped_commands = 0;
        f32 master_peak = 0.0f;
        /// Threads taking part in mixing (the audio thread plus helpers) and the instruction set the kernels run on.
        u32 mix_threads = 1;
        const char *simd = "";
        /// Voices mixed by the compute backend in the last block (0 when none is configured or the block was too small).
        u32 compute_voices = 0;
    };

    /// The audio graph. Game-thread methods enqueue commands on a lock-free ring; `render_block` (called from whatever
    /// pulls audio: a device callback, an offline renderer, a test) applies them at block boundaries and mixes. The two
    /// sides never block each other and the audio side never allocates or frees.
    class AudioEngine {
      public:
        explicit AudioEngine(AudioEngineConfig config);
        ~AudioEngine();
        AudioEngine(const AudioEngine &) = delete;
        AudioEngine &operator=(const AudioEngine &) = delete;

        // ---- game thread -----------------------------------------------------------------------------------------
        [[nodiscard]] const AudioEngineConfig &config() const noexcept;
        /// Lets voices created from now on be delayed by up to `seconds` (propagation delay needs a delay line per voice, which
        /// is only allocated when this, or `AudioEngineConfig::acoustics_delay_seconds`, asks for one).
        void set_acoustics_delay_capacity(f32 seconds) noexcept;
        [[nodiscard]] u32 output_count() const noexcept;
        [[nodiscard]] BusId master_bus(OutputId output = primary_output) const noexcept;
        /// Adds a bus under `parent` (the output's master when `no_bus`); returns `no_bus` if the parent is unknown.
        BusId add_bus(OutputId output, const UString &name, BusId parent = no_bus, f32 volume = 1.0f);
        /// A bus that reverberates what is sent to it; returns it.
        BusId add_reverb_bus(OutputId output, const UString &name, f32 rt60, f32 size = 1.0f, f32 damping = 0.3f, f32 volume = 1.0f);
        /// Appends an effect to a bus (before the master's limiter). It is prepared for the bus's format first.
        void add_effect(BusId bus, std::unique_ptr<BusEffect> effect);
        /// The same from a description; returns false (and adds nothing) when the spec is invalid.
        bool add_effect(BusId bus, const EffectSpec &spec);
        /// Changes one parameter of the `effect_index`th effect on a bus, safely while audio is running.
        void set_effect_parameter(BusId bus, u32 effect_index, u32 parameter, f32 value);
        void set_effect_parameter(BusId bus, u32 effect_index, const ustr &parameter, f32 value);
        /// Adds an effect to a playing voice / changes one of its parameters (`effect_index` counts PlayParams::effects first).
        bool add_voice_effect(VoiceId voice, const EffectSpec &spec);
        void set_voice_effect_parameter(VoiceId voice, u32 effect_index, u32 parameter, f32 value);
        void set_bus_volume(BusId bus, f32 volume);
        void set_bus_muted(BusId bus, bool muted);

        /// Starts a voice; returns 0 when the command queue or voice table is full.
        VoiceId play(PlayParams params);
        void stop(VoiceId voice, f32 fade_seconds = 0.0f);
        void set_volume(VoiceId voice, f32 volume);
        void set_pitch(VoiceId voice, f32 pitch);
        /// World position/velocity for World voices, listener-space for ListenerRelative ones. Call every frame from the
        /// entity the sound is attached to.
        void set_position(VoiceId voice, const glm::vec3 &position, const glm::vec3 &velocity = glm::vec3(0.0f));
        void set_paused(VoiceId voice, bool paused);
        void set_acoustics(VoiceId voice, const AcousticsResult &result);
        void set_listener(const ListenerState &listener);
        void set_output_volume(OutputId output, f32 volume);

        // ---- timeline: waypoints, jumps, fades (seek/markers need a seekable source) ----------------------------
        /// Moves playback; the first few milliseconds fade in to hide the seam.
        void seek(VoiceId voice, f64 seconds);
        /// Adds a waypoint to a playing voice.
        void add_marker(VoiceId voice, const MarkerSpec &marker);
        void remove_marker(VoiceId voice, u32 marker_id);
        void clear_markers(VoiceId voice);
        /// Re-arms a marker that disarmed itself (`max_triggers` exhausted) with a fresh trigger count.
        void reset_marker(VoiceId voice, u32 marker_id, i32 triggers = -1);
        void set_loop(VoiceId voice, const std::optional<LoopSpec> &loop);
        /// Ramps volume/pitch to a target over time (block-rate; the mixer smooths within blocks).
        void fade_volume(VoiceId voice, f32 target, f32 seconds);
        void fade_pitch(VoiceId voice, f32 target, f32 seconds);
        /// Lowers `target_bus` by `reduction_db` while `trigger_bus` is louder than `threshold_db` RMS (dialogue over music).
        void add_ducking(BusId target_bus, BusId trigger_bus, f32 reduction_db, f32 threshold_db = -40.0f, f32 attack_seconds = 0.05f,
                         f32 release_seconds = 0.4f);

        /// Seconds of audio the engine has mixed (the clock `start_at_seconds` refers to); updated every block.
        [[nodiscard]] f64 clock_seconds() const noexcept;
        /// Live readout of a voice (null for an unknown or long-finished voice).
        [[nodiscard]] std::shared_ptr<const VoiceStatus> status(VoiceId voice) const;
        /// Events since the last call, in order.
        void poll_events(std::vector<VoiceEvent> &out);
        [[nodiscard]] BusLevels bus_levels(BusId bus) const noexcept;
        /// The most recent `count` mono-mixed samples (<= 8192) of an output, for oscilloscopes and spectrum displays.
        void copy_output_tap(OutputId output, f32 *out, u32 count) const noexcept;

        /// Frees what the audio thread retired and returns the voices that finished since the last call.
        std::vector<VoiceId> pump();
        [[nodiscard]] AudioStats stats() const noexcept;

        // ---- audio thread ----------------------------------------------------------------------------------------
        /// Mixes one block of `config().block_frames` frames into every output.
        void render_block();
        /// The just-rendered bed of an output (speaker feeds, binaural stereo, or an Objects output's static bed).
        [[nodiscard]] const AudioBuffer &bed(OutputId output) const noexcept;
        /// The just-rendered objects of an Objects output (empty for the other kinds).
        [[nodiscard]] const ObjectBlock &objects(OutputId output) const noexcept;

        /// Device-facing pull: fills `frames` interleaved frames of a Speakers/Binaural output. The primary output drives
        /// rendering; secondary outputs read from lock-free FIFOs the primary's pulls feed (underruns give silence).
        void pull(OutputId output, f32 *interleaved, u32 frames);
        /// For a secondary output: takes up to `max_frames` frames the primary's pulls have produced so far and returns how many
        /// there were (possibly none). Network senders and recorders use this to follow an output at its own pace.
        [[nodiscard]] u32 take_output(OutputId output, f32 *interleaved, u32 max_frames);

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace SFT::Audio
