#include <Audio/Mixer.hpp>

#include <Audio/Kernels.hpp>
#include <Audio/MixPool.hpp>
#include <Audio/Realtime.hpp>

#include <Async/SpscRingBuffer.hpp>
#include <Foundation/Log.hpp>
#include <Foundation/Iter.hpp>
#include <span>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <unordered_map>
#include <cmath>
#include <format>

namespace SFT::Audio {

    // ---- bus effects -----------------------------------------------------------------------------------------------

    namespace {

        class ReverbEffect final : public BusEffect {
          public:
            ReverbEffect(f32 sample_rate, const SpeakerLayout &layout, f32 rt60, f32 size, f32 damping)
                : sample_rate_(sample_rate), size_(size), values_{rt60, damping, 1.0f} {
                reverb_.set(sample_rate, rt60, size, damping);
                for (const Speaker &s : layout.speakers) {
                    // Wet left feeds speakers on the left, wet right the ones on the right, the centre both; LFE none.
                    if (is_lfe(s.role)) {
                        left_.push_back(0.0f);
                        right_.push_back(0.0f);
                    } else if (s.azimuth_degrees > 5.0f) {
                        left_.push_back(0.7f);
                        right_.push_back(0.0f);
                    } else if (s.azimuth_degrees < -5.0f) {
                        left_.push_back(0.0f);
                        right_.push_back(0.7f);
                    } else {
                        left_.push_back(0.5f);
                        right_.push_back(0.5f);
                    }
                }
            }

            void process(AudioBuffer &buffer) override {
                const u32 frames = buffer.frames();
                if (mono_.size() < frames) {
                    // Sized on the first block (the bus is created on the game thread with its final block size, so this
                    // path is a safety net, not the normal case).
                    mono_.assign(frames, 0.0f);
                    wet_left_.assign(frames, 0.0f);
                    wet_right_.assign(frames, 0.0f);
                }
                std::fill_n(mono_.begin(), frames, 0.0f);
                for (u32 c = 0; c < buffer.channels(); ++c) {
                    const f32 *in = buffer.data(c);
                    for (u32 i = 0; i < frames; ++i) {
                        mono_[i] += in[i];
                    }
                }
                reverb_.process(mono_.data(), wet_left_.data(), wet_right_.data(), frames);
                const f32 level = values_[2];
                buffer.clear();
                for (u32 c = 0; c < buffer.channels() && c < left_.size(); ++c) {
                    f32 *out = buffer.data(c);
                    for (u32 i = 0; i < frames; ++i) {
                        out[i] = (wet_left_[i] * left_[c] + wet_right_[i] * right_[c]) * level;
                    }
                }
            }

            // The decay and tone follow the room the listener is in (the acoustics layer retargets them as they move), without
            // touching the delay lengths, so changing them never allocates.
            [[nodiscard]] ustr name() const override { return "Reverb"_ustr; }
            [[nodiscard]] u32 tail_frames() const override { return static_cast<u32>(values_[0] * sample_rate_ * 1.25f); }
            [[nodiscard]] std::span<const ParameterInfo> parameters() const override { return kParameters; }
            void set_parameter(u32 index, f32 value) override {
                if (index < 3) {
                    values_[index] = std::clamp(value, kParameters[index].minimum, kParameters[index].maximum);
                    if (index < 2) {
                        reverb_.set(sample_rate_, values_[0], size_, values_[1]);
                    }
                }
            }
            [[nodiscard]] f32 parameter(u32 index) const override { return index < 3 ? values_[index] : 0.0f; }

            void reserve(u32 frames) {
                mono_.assign(frames, 0.0f);
                wet_left_.assign(frames, 0.0f);
                wet_right_.assign(frames, 0.0f);
            }

          private:
            static inline const std::array<ParameterInfo, 3> kParameters{{
                {"rt60", "s", 0.05f, 20.0f, 1.5f, true},
                {"damping", "", 0.0f, 0.98f, 0.3f},
                {"level", "", 0.0f, 2.0f, 1.0f},
            }};
            f32 sample_rate_;
            f32 size_;
            std::array<f32, 3> values_;
            FdnReverb reverb_;
            std::vector<f32> left_, right_;
            std::vector<f32> mono_, wet_left_, wet_right_;
        };

        class LimiterEffect final : public BusEffect {
          public:
            LimiterEffect(f32 sample_rate, f32 ceiling) { limiter_.set(sample_rate, ceiling); }
            void process(AudioBuffer &buffer) override { limiter_.process(buffer); }

          private:
            Limiter limiter_;
        };

        class CompressorEffect final : public BusEffect {
          public:
            CompressorEffect(f32 sample_rate, f32 threshold_db, f32 ratio, f32 attack, f32 release, f32 makeup_db) {
                compressor_.set(sample_rate, threshold_db, ratio, attack, release, makeup_db);
            }
            void process(AudioBuffer &buffer) override { compressor_.process(buffer); }

          private:
            Compressor compressor_;
        };

    } // namespace

    std::unique_ptr<BusEffect> make_reverb_effect(f32 sample_rate, const SpeakerLayout &layout, f32 rt60, f32 size, f32 damping) {
        return std::make_unique<ReverbEffect>(sample_rate, layout, rt60, size, damping);
    }
    std::unique_ptr<BusEffect> make_limiter_effect(f32 sample_rate, f32 ceiling) {
        return std::make_unique<LimiterEffect>(sample_rate, ceiling);
    }
    std::unique_ptr<BusEffect> make_compressor_effect(f32 sample_rate, f32 threshold_db, f32 ratio, f32 attack_seconds, f32 release_seconds,
                                                      f32 makeup_db) {
        return std::make_unique<CompressorEffect>(sample_rate, threshold_db, ratio, attack_seconds, release_seconds, makeup_db);
    }

    // ---- internals -------------------------------------------------------------------------------------------------

    namespace {

        constexpr u32 kMaxBuses = 256;
        constexpr usize kMaxEffectsPerBus = 8;
        constexpr usize kMaxEffectsPerVoice = 8;
        constexpr u32 kVoiceSlotBits = 20;
        constexpr u32 kVoiceSlotMask = (1u << kVoiceSlotBits) - 1u;
        constexpr usize kWorkerEventCapacity = 1024;
        /// Voices quieter than this (linear gain x priority) are not worth mixing: they advance silently like virtual ones.
        constexpr f32 kInaudible = 1.0e-5f;

        struct VoiceOutputState {
            std::array<f32, max_channels> previous_gains{};
            f32 previous_gain = 0.0f;
            bool initialised = false;
            std::unique_ptr<BinauralFilter> binaural;
            i32 object_slot = -1;
            /// Source channels onto this output's bed (non-spatial voices, and ambisonic sources on outputs without a decoder).
            ChannelMatrix matrix;
        };

        struct Bus {
            UString name;
            BusId parent = no_bus;
            OutputId output = 0;
            u32 depth = 0;
            f32 volume = 1.0f;
            f32 applied_volume = 1.0f;
            bool muted = false;
            std::vector<std::unique_ptr<BusEffect>> effects;
            AudioBuffer buffer;
            /// One accumulation buffer per helper mixing thread (the audio thread writes `buffer` directly).
            std::vector<AudioBuffer> partial;
            std::atomic<f32> peak{0.0f};
            std::atomic<f32> rms{0.0f};
            u32 index = 0;
            /// Something was mixed into this bus this block (set by the mixing thread 0 and by `merge_workers`).
            bool has_input = false;
            /// Frames the bus's effects keep ringing after its input stops (reverb, delay): an idle bus is skipped once they pass.
            i64 tail_remaining = 0;
        };

        /// A voice's position and velocity as the game last set them, readable without a lock or a command per update: written by the
        /// game thread under a sequence counter (odd while writing) and read, with a retry, by the audio thread.
        struct PositionSlot {
            std::atomic<u32> sequence{0};
            std::atomic<u32> owner{0};
            std::array<std::atomic<f32>, 6> values{};
        };

        struct BusInfo {
            OutputId output = 0;
            BusId parent = no_bus;
            u32 depth = 0;
        };

        struct Frame {
            f32 gain = 0.0f;          // everything except bus volumes
            glm::vec3 direction{0.0f, 0.0f, -1.0f}; // listener space, unit
            f32 distance = 0.0f;
            f32 doppler = 1.0f;
            f32 spread = 0.0f;
            f32 score = 0.0f;
        };

        struct Voice {
            VoiceId id = 0;
            u32 slot = 0;
            std::shared_ptr<DataSource> source;
            std::vector<BusId> buses;
            std::vector<VoiceOutputState> states;
            f32 volume = 1.0f, pitch = 1.0f;
            SourceSpace space = SourceSpace::World;
            bool spatial = true;
            glm::vec3 position{0.0f}, velocity{0.0f};
            DistanceModel distance{};
            f32 spread = 0.0f, priority = 1.0f, doppler_factor = 1.0f;
            Panner panner = Panner::Vbap;
            BusId aux_bus = no_bus;
            f32 aux_send = 0.0f;
            bool use_acoustics = true;

            AcousticsResult acoustics_target;
            AcousticsResult acoustics;

            bool paused = false;
            bool stopping = false;
            bool ending = false;
            f32 fade = 1.0f, fade_step = 0.0f;

            Biquad lowpass;
            f32 applied_cutoff = 20000.0f;
            bool lowpass_active = false;
            DelayLine delay;
            bool delay_active = false;
            f32 aux_previous = 0.0f;
            bool aux_initialised = false;

            AudioBuffer src;
            Frame frame;
            bool physical = false;
            /// The source as a plain loaded sample, when it is one (the compute backend can mix those).
            BufferSource *plain_buffer = nullptr;

            /// What the source's channels mean, and (for ambisonic sources) the field rotated into the listener's frame.
            ChannelLayoutInfo layout;
            bool ambisonic = false;
            /// Per-voice effects, run on `src` before mixing.
            std::vector<std::unique_ptr<AudioEffect>> effects;

            // ---- timeline
            struct Marker {
                u32 id = 0;
                u64 frame = 0;
                MarkerAction action = MarkerAction::None;
                u64 target = 0;
                bool seamless = false;
                i32 remaining = -1; // hits left (-1 = unlimited, 0 = exhausted)
                bool fire = true;
                bool armed = true;
                f32 fade_seconds = 0.0f; // FadeStop
            };
            struct Ramp {
                bool active = false;
                f32 target = 0.0f;
                f32 step = 0.0f; // per block
            };
            std::vector<Marker> markers;
            AudioBuffer segment;       // one source read lands here before being placed in `src`
            f32 start_delay_seconds = 0.0f;
            f64 start_at_seconds = -1.0;
            u64 delay_frames = 0;      // silence still to emit before the source starts
            bool started_event_due = false;
            u32 declick_frames = 0;
            u32 fade_in_remaining = 0;
            f32 start_fade_seconds = 0.0f;
            bool start_fade_applied = false;
            Ramp volume_ramp, pitch_ramp;
            std::shared_ptr<VoiceStatus> status;
            VoiceStatus::State last_state = VoiceStatus::State::Pending;
            bool finish_reported = false;
        };

        /// Clears a retired voice for reuse while keeping the memory it grew (audio buffers, marker and effect lists), so
        /// playing a sound is normally allocation-free apart from its status object.
        void recycle(Voice &v) {
            Voice fresh;
            fresh.src = std::move(v.src);
            fresh.segment = std::move(v.segment);
            fresh.markers = std::move(v.markers);
            fresh.markers.clear();
            fresh.buses = std::move(v.buses);
            fresh.buses.clear();
            fresh.states = std::move(v.states);
            fresh.states.clear();
            fresh.effects = std::move(v.effects);
            fresh.effects.clear();
            fresh.delay = std::move(v.delay);
            v = std::move(fresh);
        }

        constexpr u32 kLoopMarkerId = 0xFFFFFFF0u;

        struct DuckRule {
            BusId target = no_bus;
            BusId trigger = no_bus;
            f32 floor_gain = 0.5f;   // multiplier while ducked
            f32 threshold = 0.01f;   // linear RMS
            f32 attack = 0.2f;       // per-block smoothing coefficients
            f32 release = 0.05f;
            f32 current = 1.0f;
        };

        struct OutputState {
            OutputDesc desc;
            VbapPanner vbap;
            AmbisonicDecoder decoder;
            AudioBuffer ambi;
            std::vector<AudioBuffer> ambi_partial; // one per helper mixing thread
            bool ambi_used = false;
            ObjectBlock objects;
            /// Owner voice of each object slot (0 = free); claimed with a compare-exchange so mixing threads never collide.
            std::unique_ptr<std::atomic<VoiceId>[]> slot_owner;
            u32 slot_count = 0;
            // Secondary-output FIFO (interleaved samples), fed by the primary's pulls.
            std::unique_ptr<Async::SpscRingBuffer<f32>> fifo;
            std::vector<f32> fifo_carry;
            usize fifo_carry_position = 0;
            std::vector<f32> staged;
            // Recent mono output for visualisers.
            std::vector<f32> tap;
            std::unique_ptr<std::atomic<u32>> tap_write = std::make_unique<std::atomic<u32>>(0);
        };

        constexpr u32 kTapSize = 8192;

        enum class CommandType : u8 {
            Play, Stop, SetVolume, SetPitch, SetPosition, SetPaused, SetAcoustics, SetListener,
            AddBus, AddEffect, SetBusVolume, SetBusMuted,
            Seek, AddMarker, RemoveMarker, ClearMarkers, ResetMarker, SetLoop, FadeVolume, FadePitch, AddDuck,
            AddVoiceEffect, SetEffectParameter, SetVoiceEffectParameter
        };

        struct Command {
            CommandType type = CommandType::Stop;
            u32 id = 0;
            u32 aux = 0;
            std::array<f32, 16> f{};
            std::array<f64, 2> d{};
            void *ptr = nullptr;
        };
        static_assert(std::is_trivially_copyable_v<Command>);

        enum class RetiredKind : u8 { Voice, Effect };
        struct Retired {
            void *ptr = nullptr;
            RetiredKind kind = RetiredKind::Voice;
        };
        static_assert(std::is_trivially_copyable_v<Retired>);

        /// Everything one mixing thread needs for itself: scratch buffers, which accumulators it touched, and the events its
        /// voices raised (merged into the shared queue by the audio thread once all threads have finished).
        struct Worker {
            u32 index = 0;
            std::vector<f32> mono, scaled, tmp_left, tmp_right;
            std::array<f32, max_channels> gain_scratch{};
            std::array<f32, max_channels> ambi_scratch{};
            AudioBuffer rotated;
            std::vector<u8> bus_dirty;
            std::vector<u8> ambi_dirty;
            std::vector<VoiceEvent> events;
            u32 virtualised = 0;
        };

    } // namespace

    struct AudioEngine::Impl {
        explicit Impl(AudioEngineConfig config);

        // ---- construction helpers (game thread)
        [[nodiscard]] std::unique_ptr<Bus> make_bus(OutputId output, const UString &name, BusId parent, u32 depth, f32 volume) const;
        bool push(const Command &command);
        [[nodiscard]] std::unique_ptr<Voice> acquire_voice();
        void give_back(std::unique_ptr<Voice> voice);

        // ---- audio thread
        void apply_commands();
        void prepare_voice(Voice &voice);
        void seek_voice(Voice &voice, f64 seconds, bool declick, bool emit_event, Worker &worker);
        u32 fill_source(Voice &voice, u32 frames, Worker &worker);
        void emit(Worker &worker, VoiceId voice, VoiceEvent::Kind kind, u32 marker_id, f64 position_seconds);
        void rearm_markers(Voice &voice, u64 position);
        bool trigger_marker(Voice &voice, Voice::Marker &marker, u32 written_frames, Worker &worker);
        /// Retunes the voice's distance/occlusion low-pass for this block (air absorption plus the acoustics provider's
        /// cutoff) and reports whether it is in the signal path.
        [[nodiscard]] bool update_spatial_lowpass(Voice &voice);
        void render_voice(Voice &voice, Worker &worker);
        void render_ambisonic(Voice &voice, Worker &worker);
        void retire_finished();
        [[nodiscard]] f32 bus_chain_gain(const Bus &bus) const;
        void process_buses();
        void mix_voices();
        void merge_workers();
        // Compute-mode mixing: voices that qualify are described to the backend instead of mixed here.
        [[nodiscard]] bool plan_compute_voice(Voice &voice);
        void compute_begin();
        void compute_end();
        static void mix_entry(void *context, u32 participant);
        static void prepare_entry(void *context, u32 participant);

        [[nodiscard]] AudioBuffer &bus_target(Bus &bus, Worker &worker) const {
            if (worker.index == 0) {
                bus.has_input = true;
                return bus.buffer;
            }
            worker.bus_dirty[bus.index] = 1;
            return bus.partial[worker.index - 1];
        }
        [[nodiscard]] AudioBuffer &ambi_target(OutputId output, Worker &worker) {
            OutputState &out = outputs[output];
            if (worker.index == 0) {
                out.ambi_used = true;
                return out.ambi;
            }
            worker.ambi_dirty[output] = 1;
            return out.ambi_partial[worker.index - 1];
        }

        AudioEngineConfig cfg;
        std::vector<OutputState> outputs;

        // Audio-thread tables (fixed capacity so the audio thread never allocates).
        std::array<std::unique_ptr<Bus>, kMaxBuses> buses;
        std::vector<Bus *> bus_order;
        std::vector<std::unique_ptr<Voice>> voices;
        std::vector<Voice *> by_slot;      // voice slot -> live voice
        std::vector<Voice *> physical_list; // the voices mixed this block
        std::vector<std::pair<f32, u32>> ranking;
        ListenerState listener;
        glm::quat inverse_listener_rotation{1.0f, 0.0f, 0.0f, 0.0f};
        AmbisonicRotator rotator{max_ambisonic_order};
        glm::quat rotator_rotation{1.0f, 0.0f, 0.0f, 0.0f};
        u32 ambisonic_voices = 0;

        // Mixing threads.
        std::unique_ptr<MixPool> pool;
        std::vector<std::unique_ptr<Worker>> workers;
        std::atomic<u32> cursor{0};
        std::unique_ptr<PositionSlot[]> position_table;

        // Compute backend state (audio thread).
        struct ComputeSample {
            ComputeSampleId id = 0;
            std::shared_ptr<const SampleBuffer> keep;
            u64 last_block = 0;
        };
        struct ComputeState {
            std::unordered_map<const void *, ComputeSample> samples;
            std::vector<ComputeVoice> voices;
            std::vector<ComputeTap> taps, sorted;
            std::vector<i32> destination_lookup; // bus * max_channels + channel -> dense destination (or -1)
            std::vector<u32> destination_keys;
            std::vector<u32> counts;
            std::vector<Voice *> remaining;
            std::vector<Voice *> owners; // owners[i] is the voice behind voices[i]
            u32 signal_count = 0;
            u64 block = 0;
            bool submitted = false;
            u32 handled = 0;
        } compute;

        // Game-thread tables.
        std::vector<BusInfo> bus_info;
        /// Parameter tables of the effects added to each bus (game thread), to resolve parameter names to indices.
        std::vector<std::vector<std::span<const ParameterInfo>>> bus_effect_infos = std::vector<std::vector<std::span<const ParameterInfo>>>(kMaxBuses);
        std::vector<u32> free_slots;
        std::vector<u32> slot_generation;
        std::vector<std::shared_ptr<VoiceStatus>> status_by_slot;
        std::vector<VoiceId> status_owner;
        std::vector<std::unique_ptr<Voice>> voice_pool;

        Async::SpscRingBuffer<Command> commands;
        Async::SpscRingBuffer<Retired> retired;
        Async::SpscRingBuffer<VoiceId> finished;
        Async::SpscRingBuffer<VoiceEvent> events;
        std::vector<DuckRule> duck_rules;
        std::array<f32, kMaxBuses> duck_multiplier{};
        u64 clock_local = 0;
        std::atomic<u64> clock_frames{0};
        std::vector<Command> command_scratch;
        std::vector<Retired> retired_scratch;
        std::vector<VoiceId> finished_scratch;

        std::atomic<f32> acoustics_delay{0.0f};
        std::atomic<u32> stat_compute{0};
        std::atomic<u32> stat_voices{0}, stat_physical{0}, stat_virtual{0}, stat_dropped{0};
        std::atomic<u32> stat_peak_bits{0};
        u32 stage_position = 0;
        bool stage_empty = true;
    };

    AudioEngine::Impl::Impl(AudioEngineConfig config)
        : cfg(std::move(config)), commands(cfg.command_capacity), retired(cfg.command_capacity), finished(cfg.command_capacity),
          events(cfg.command_capacity) {
        if (cfg.outputs.empty()) {
            cfg.outputs.push_back(OutputDesc{});
        }
        cfg.block_frames = std::max(cfg.block_frames, 16u);
        cfg.max_voices = std::clamp(cfg.max_voices, 1u, kVoiceSlotMask);
        const u32 frames = cfg.block_frames;

        // Mixing threads: the audio thread plus helpers; each gets scratch space and accumulators of its own.
        pool = std::make_unique<MixPool>(MixPool::resolve_helpers(cfg.mix_threads));
        const u32 participants = pool->participants();
        for (u32 w = 0; w < participants; ++w) {
            auto worker = std::make_unique<Worker>();
            worker->index = w;
            worker->mono.assign(frames, 0.0f);
            worker->scaled.assign(frames, 0.0f);
            worker->tmp_left.assign(frames, 0.0f);
            worker->tmp_right.assign(frames, 0.0f);
            worker->rotated.resize(ambisonic_channels(max_ambisonic_order), frames);
            worker->bus_dirty.assign(kMaxBuses, 0);
            worker->ambi_dirty.assign(cfg.outputs.size(), 0);
            worker->events.reserve(kWorkerEventCapacity);
            workers.push_back(std::move(worker));
        }

        voices.reserve(cfg.max_voices);
        by_slot.assign(cfg.max_voices, nullptr);
        physical_list.reserve(std::min(cfg.max_physical_voices, cfg.max_voices));
        ranking.reserve(cfg.max_voices);
        free_slots.reserve(cfg.max_voices);
        for (u32 s = cfg.max_voices; s > 0; --s) {
            free_slots.push_back(s - 1);
        }
        slot_generation.assign(cfg.max_voices, 0);
        status_by_slot.assign(cfg.max_voices, nullptr);
        status_owner.assign(cfg.max_voices, 0);
        position_table = std::make_unique<PositionSlot[]>(cfg.max_voices);
        bus_order.reserve(kMaxBuses);
        command_scratch.reserve(cfg.command_capacity);
        retired_scratch.reserve(cfg.command_capacity);
        finished_scratch.reserve(cfg.command_capacity);
        duck_rules.reserve(32);
        duck_multiplier.fill(1.0f);

        outputs.resize(cfg.outputs.size());
        for (OutputId o = 0; o < outputs.size(); ++o) {
            OutputState &out = outputs[o];
            out.desc = cfg.outputs[o];
            if (out.desc.kind == OutputDesc::Kind::Binaural) {
                out.desc.layout = SpeakerLayout::stereo();
            }
            out.vbap.build(out.desc.layout);
            if (out.desc.kind != OutputDesc::Kind::Binaural) {
                const u32 order = std::clamp(out.desc.ambisonic_order, 1u, max_ambisonic_order);
                out.decoder.build(out.desc.layout, order);
                out.ambi.resize(ambisonic_channels(order), frames);
                for (u32 w = 1; w < participants; ++w) {
                    out.ambi_partial.emplace_back(ambisonic_channels(order), frames);
                }
            }
            if (out.desc.kind == OutputDesc::Kind::Objects) {
                out.objects.samples.resize(out.desc.max_objects, frames);
                out.objects.metadata.assign(out.desc.max_objects, ObjectMetadata{});
                out.slot_count = out.desc.max_objects;
                out.slot_owner = std::make_unique<std::atomic<VoiceId>[]>(out.slot_count);
                for (u32 i = 0; i < out.slot_count; ++i) {
                    out.slot_owner[i].store(0, std::memory_order_relaxed);
                }
            }
            if (o > 0) {
                out.fifo = std::make_unique<Async::SpscRingBuffer<f32>>(static_cast<usize>(frames) * out.desc.layout.channel_count() * 16);
                out.fifo_carry.reserve(out.fifo->capacity());
            }
            out.staged.assign(static_cast<usize>(frames) * out.desc.layout.channel_count(), 0.0f);
            out.tap.assign(kTapSize, 0.0f);

            // Master bus of this output: BusId == output index.
            auto master = make_bus(o, UString{std::format("{} master", out.desc.name)}, no_bus, 0, 1.0f);
            master->effects.reserve(kMaxEffectsPerBus);
            master->effects.push_back(make_limiter_effect(static_cast<f32>(cfg.sample_rate)));
            master->index = o;
            buses[o] = std::move(master);
            bus_info.push_back(BusInfo{o, no_bus, 0});
            bus_order.push_back(buses[o].get());
        }
    }

    std::unique_ptr<Bus> AudioEngine::Impl::make_bus(OutputId output, const UString &name, BusId parent, u32 depth, f32 volume) const {
        auto bus = std::make_unique<Bus>();
        bus->name = name;
        bus->output = output;
        bus->parent = parent;
        bus->depth = depth;
        bus->volume = bus->applied_volume = volume;
        bus->effects.reserve(kMaxEffectsPerBus);
        const u32 channels = outputs[output].desc.layout.channel_count();
        bus->buffer.resize(channels, cfg.block_frames);
        for (usize w = 1; w < workers.size(); ++w) {
            bus->partial.emplace_back(channels, cfg.block_frames);
        }
        return bus;
    }

    bool AudioEngine::Impl::push(const Command &command) {
        if (commands.try_push(command)) {
            return true;
        }
        stat_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::unique_ptr<Voice> AudioEngine::Impl::acquire_voice() {
        std::unique_ptr<Voice> voice;
        if (!voice_pool.empty()) {
            voice = std::move(voice_pool.back());
            voice_pool.pop_back();
        } else {
            voice = std::make_unique<Voice>();
            voice->markers.reserve(16);
            voice->effects.reserve(kMaxEffectsPerVoice);
        }
        return voice;
    }

    void AudioEngine::Impl::give_back(std::unique_ptr<Voice> voice) {
        // Keep a bounded number of idle voices: enough to absorb a burst, not enough to hold memory forever.
        constexpr usize kPoolLimit = 4096;
        if (voice_pool.size() < kPoolLimit) {
            recycle(*voice);
            voice_pool.push_back(std::move(voice));
        }
    }

    // ---- public API (game thread) ------------------------------------------------------------------------------------

    AudioEngine::AudioEngine(AudioEngineConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

    AudioEngine::~AudioEngine() {
        // Stop the mixing threads first: nothing below may race them.
        impl_->pool.reset();
        // Anything still in the rings or tables is owned by raw pointers in commands; drain and delete them.
        std::vector<Command> leftover;
        impl_->commands.drain_into(leftover);
        for (const Command &c : leftover) {
            if (c.type == CommandType::Play) {
                delete static_cast<Voice *>(c.ptr);
            } else if (c.type == CommandType::AddEffect || c.type == CommandType::AddVoiceEffect) {
                delete static_cast<BusEffect *>(c.ptr);
            } else if (c.type == CommandType::AddBus) {
                delete static_cast<Bus *>(c.ptr);
            }
        }
        std::vector<Retired> items;
        impl_->retired.drain_into(items);
        for (const Retired &r : items) {
            if (r.kind == RetiredKind::Voice) {
                delete static_cast<Voice *>(r.ptr);
            } else {
                delete static_cast<BusEffect *>(r.ptr);
            }
        }
    }

    const AudioEngineConfig &AudioEngine::config() const noexcept { return impl_->cfg; }
    void AudioEngine::set_acoustics_delay_capacity(f32 seconds) noexcept { impl_->acoustics_delay.store(std::max(seconds, 0.0f), std::memory_order_relaxed); }
    u32 AudioEngine::output_count() const noexcept { return static_cast<u32>(impl_->outputs.size()); }
    BusId AudioEngine::master_bus(OutputId output) const noexcept { return output < impl_->outputs.size() ? output : no_bus; }

    BusId AudioEngine::add_bus(OutputId output, const UString &name, BusId parent, f32 volume) {
        if (output >= impl_->outputs.size() || impl_->bus_info.size() >= kMaxBuses) {
            return no_bus;
        }
        if (parent == no_bus) {
            parent = output;
        }
        if (parent >= impl_->bus_info.size() || impl_->bus_info[parent].output != output) {
            return no_bus;
        }
        const BusId id = static_cast<BusId>(impl_->bus_info.size());
        const u32 depth = impl_->bus_info[parent].depth + 1;
        auto bus = impl_->make_bus(output, name, parent, depth, volume);
        Command command;
        command.type = CommandType::AddBus;
        command.id = id;
        command.ptr = bus.get();
        if (!impl_->push(command)) {
            return no_bus;
        }
        bus.release(); // now owned by the audio side
        impl_->bus_info.push_back(BusInfo{output, parent, depth});
        return id;
    }

    BusId AudioEngine::add_reverb_bus(OutputId output, const UString &name, f32 rt60, f32 size, f32 damping, f32 volume) {
        const BusId bus = add_bus(output, name, no_bus, volume);
        if (bus == no_bus) {
            return no_bus;
        }
        auto effect = std::make_unique<ReverbEffect>(static_cast<f32>(impl_->cfg.sample_rate), impl_->outputs[output].desc.layout, rt60, size, damping);
        effect->reserve(impl_->cfg.block_frames);
        add_effect(bus, std::move(effect));
        return bus;
    }

    void AudioEngine::add_effect(BusId bus, std::unique_ptr<BusEffect> effect) {
        if (bus >= impl_->bus_info.size() || !effect) {
            return;
        }
        effect->prepare(static_cast<f32>(impl_->cfg.sample_rate), impl_->outputs[impl_->bus_info[bus].output].desc.layout.channel_count(),
                        impl_->cfg.block_frames);
        Command command;
        command.type = CommandType::AddEffect;
        command.id = bus;
        command.ptr = effect.get();
        const std::span<const ParameterInfo> infos = effect->parameters();
        if (impl_->push(command)) {
            effect.release();
            impl_->bus_effect_infos[bus].push_back(infos);
        }
    }

    bool AudioEngine::add_effect(BusId bus, const EffectSpec &spec) {
        if (bus >= impl_->bus_info.size()) {
            return false;
        }
        auto effect = make_effect(spec);
        if (!effect) {
            Foundation::log_warn("audio: {}", effect.error());
            return false;
        }
        add_effect(bus, std::move(*effect));
        return true;
    }

    void AudioEngine::set_effect_parameter(BusId bus, u32 effect_index, u32 parameter, f32 value) {
        Command c;
        c.type = CommandType::SetEffectParameter;
        c.id = bus;
        c.aux = (effect_index << 16) | (parameter & 0xFFFFu);
        c.f[0] = value;
        impl_->push(c);
    }

    void AudioEngine::set_effect_parameter(BusId bus, u32 effect_index, const ustr &parameter, f32 value) {
        if (bus >= kMaxBuses || effect_index >= impl_->bus_effect_infos[bus].size()) {
            return;
        }
        // The parameter tables are immutable static data, so naming a parameter is safe on the game thread.
        const auto infos = impl_->bus_effect_infos[bus][effect_index];
        for (u32 i = 0; i < infos.size(); ++i) {
            if (infos[i].name == parameter) {
                set_effect_parameter(bus, effect_index, i, value);
                return;
            }
        }
    }

    bool AudioEngine::add_voice_effect(VoiceId voice, const EffectSpec &spec) {
        auto effect = make_effect(spec);
        if (!effect) {
            Foundation::log_warn("audio: {}", effect.error());
            return false;
        }
        const u32 slot = voice & kVoiceSlotMask;
        if (slot >= impl_->status_owner.size() || impl_->status_owner[slot] != voice) {
            return false;
        }
        // Prepared for the source's own channel count, which the voice's status records.
        const u32 channels = impl_->status_by_slot[slot]->channels();
        (*effect)->prepare(static_cast<f32>(impl_->cfg.sample_rate), channels, impl_->cfg.block_frames);
        Command c;
        c.type = CommandType::AddVoiceEffect;
        c.id = voice;
        c.ptr = effect->get();
        if (!impl_->push(c)) {
            return false;
        }
        effect->release();
        return true;
    }

    void AudioEngine::set_voice_effect_parameter(VoiceId voice, u32 effect_index, u32 parameter, f32 value) {
        Command c;
        c.type = CommandType::SetVoiceEffectParameter;
        c.id = voice;
        c.aux = (effect_index << 16) | (parameter & 0xFFFFu);
        c.f[0] = value;
        impl_->push(c);
    }

    void AudioEngine::set_bus_volume(BusId bus, f32 volume) {
        Command c;
        c.type = CommandType::SetBusVolume;
        c.id = bus;
        c.f[0] = volume;
        impl_->push(c);
    }

    void AudioEngine::set_bus_muted(BusId bus, bool muted) {
        Command c;
        c.type = CommandType::SetBusMuted;
        c.id = bus;
        c.f[0] = muted ? 1.0f : 0.0f;
        impl_->push(c);
    }

    VoiceId AudioEngine::play(PlayParams params) {
        Impl &m = *impl_;
        if (!params.source || m.bus_info.empty() || m.free_slots.empty()) {
            return 0;
        }
        if (params.bus >= m.bus_info.size()) {
            params.bus = primary_output;
        }
        auto voice = m.acquire_voice();
        // Every source can be pitched: those that cannot resample themselves are wrapped in a rate converter.
        const ChannelLayoutInfo source_layout_hint = params.source_layout ? *params.source_layout : params.source->channel_layout();
        voice->source = params.source->supports_rate() ? std::move(params.source) : std::make_shared<RateConverter>(std::move(params.source), m.cfg.block_frames);
        voice->plain_buffer = dynamic_cast<BufferSource *>(voice->source.get());
        const u32 source_channels = std::max(voice->source->channel_count(), 1u);
        // A layout must describe exactly the channels the source produces; anything else falls back to the conventional guess.
        voice->layout = source_layout_hint.channels == source_channels ? source_layout_hint : ChannelLayoutInfo::guess(source_channels);
        if (voice->layout.kind == ChannelKind::Ambisonic && (voice->layout.channels != ambisonic_channels(voice->layout.ambisonic_order) || voice->layout.channels > 16)) {
            voice->layout = ChannelLayoutInfo::discrete(source_channels);
        }
        voice->ambisonic = voice->layout.kind == ChannelKind::Ambisonic;

        voice->buses.push_back(params.bus);
        for (BusId mirror : params.mirror_buses) {
            if (mirror < m.bus_info.size() && !Foundation::iter(voice->buses).any(
                                                  [mirror](BusId bus) { return bus == mirror; })) {
                voice->buses.push_back(mirror);
            }
        }
        voice->states.resize(voice->buses.size());
        for (usize k = 0; k < voice->buses.size(); ++k) {
            const OutputState &out = m.outputs[m.bus_info[voice->buses[k]].output];
            if (out.desc.kind == OutputDesc::Kind::Binaural) {
                voice->states[k].binaural = out.desc.binaural ? out.desc.binaural() : make_spherical_head_filter(static_cast<f32>(m.cfg.sample_rate));
            }
            // Ambisonic sources reach speaker outputs through the output's decoder; everything else (and ambisonics on
            // headphones) is folded onto the bed by channel role.
            if (!voice->ambisonic || out.desc.kind == OutputDesc::Kind::Binaural) {
                voice->states[k].matrix = make_channel_matrix(voice->layout, out.desc.layout);
            }
        }
        voice->volume = params.volume;
        voice->pitch = params.pitch;
        voice->space = params.space;
        voice->spatial = params.spatial;
        voice->position = params.position;
        voice->velocity = params.velocity;
        voice->distance = params.distance;
        voice->spread = params.spread;
        voice->priority = params.priority;
        voice->doppler_factor = params.doppler_factor;
        voice->panner = params.panner;
        voice->aux_bus = (params.aux_bus < m.bus_info.size()) ? params.aux_bus : no_bus;
        voice->aux_send = params.aux_send;
        voice->use_acoustics = params.use_acoustics;
        voice->src.resize(source_channels, m.cfg.block_frames);
        const f32 delay_seconds = std::max(m.cfg.acoustics_delay_seconds, m.acoustics_delay.load(std::memory_order_relaxed));
        if (voice->use_acoustics && voice->spatial && voice->space == SourceSpace::World && delay_seconds > 0.0f &&
            voice->delay.capacity() < static_cast<u32>(static_cast<f32>(m.cfg.sample_rate) * delay_seconds)) {
            voice->delay.resize(static_cast<u32>(static_cast<f32>(m.cfg.sample_rate) * delay_seconds)); // propagation / detour delay
        }
        for (const EffectSpec &spec : params.effects) {
            if (voice->effects.size() >= kMaxEffectsPerVoice) {
                break;
            }
            auto effect = make_effect(spec, static_cast<f32>(m.cfg.sample_rate), source_channels, m.cfg.block_frames);
            if (effect) {
                voice->effects.push_back(std::move(*effect));
            } else {
                Foundation::log_warn("audio: voice effect skipped: {}", effect.error());
            }
        }

        // Identity: slot (reused after the voice is reported finished) plus a generation, so stale commands cannot hit a successor.
        const u32 slot = m.free_slots.back();
        m.free_slots.pop_back();
        u32 generation = (m.slot_generation[slot] + 1) & 0xFFFu;
        if (generation == 0) {
            generation = 1;
        }
        m.slot_generation[slot] = generation;
        voice->slot = slot;
        voice->id = (generation << kVoiceSlotBits) | slot;

        // Timeline: positions are converted to the source's own frames here, on the game thread.
        const f64 native = static_cast<f64>(std::max(voice->source->native_rate(), 1u));
        const bool seekable = voice->source->seekable();
        voice->segment.resize(voice->src.channels(), m.cfg.block_frames);
        voice->declick_frames = static_cast<u32>(std::max(params.declick_seconds, 0.0f) * static_cast<f32>(m.cfg.sample_rate));
        voice->start_delay_seconds = std::max(params.start_delay_seconds, 0.0f);
        voice->start_at_seconds = params.start_at_seconds;
        voice->start_fade_seconds = std::max(params.fade_in_seconds, 0.0f);
        voice->markers.reserve(params.markers.size() + 8);
        const auto to_frame = [native](f64 seconds) { return static_cast<u64>(std::max(0.0, std::round(seconds * native))); };
        if (seekable) {
            const u64 start_frame = to_frame(params.start_seconds);
            if (start_frame > 0) {
                voice->source->seek_frames(start_frame);
            }
            u32 auto_id = 1;
            for (const MarkerSpec &spec : params.markers) {
                Voice::Marker marker;
                marker.id = spec.id != 0 ? spec.id : auto_id++;
                marker.frame = to_frame(spec.seconds);
                marker.action = spec.action;
                marker.target = to_frame(spec.jump_seconds);
                marker.seamless = spec.seamless;
                marker.remaining = spec.max_triggers;
                marker.fire = spec.fire_event;
                marker.fade_seconds = static_cast<f32>(spec.jump_seconds);
                marker.armed = marker.frame >= start_frame && marker.remaining != 0;
                voice->markers.push_back(marker);
            }
            if (params.loop && params.loop->end_seconds > params.loop->start_seconds) {
                Voice::Marker marker;
                marker.id = kLoopMarkerId;
                marker.frame = to_frame(params.loop->end_seconds);
                marker.action = MarkerAction::Jump;
                marker.target = to_frame(params.loop->start_seconds);
                marker.seamless = true;
                marker.remaining = params.loop->count;
                marker.armed = marker.frame >= start_frame && marker.remaining != 0;
                voice->markers.push_back(marker);
            }
        }
        voice->status = std::make_shared<VoiceStatus>();
        const f64 length = static_cast<f64>(voice->source->length_frames());
        voice->status->set_duration(length > 0.0 ? length / native : 0.0);
        voice->status->set_channels(source_channels);
        voice->status->publish(params.start_seconds, VoiceStatus::State::Pending, params.volume, params.pitch);

        Command command;
        command.type = CommandType::Play;
        command.id = voice->id;
        command.ptr = voice.get();
        const VoiceId id = voice->id;
        std::shared_ptr<VoiceStatus> status = voice->status;
        if (!m.push(command)) {
            m.free_slots.push_back(slot);
            m.give_back(std::move(voice));
            return 0;
        }
        voice.release();
        m.status_by_slot[slot] = std::move(status);
        m.status_owner[slot] = id;
        return id;
    }

    void apply_file_metadata(PlayParams &params, const SampleBuffer &buffer, bool use_loop) {
        const f64 rate = static_cast<f64>(std::max(buffer.sample_rate, 1u));
        u32 id = 1;
        for (const AudioMarker &m : buffer.markers) {
            MarkerSpec spec;
            spec.id = id++;
            spec.name = m.name;
            spec.seconds = static_cast<f64>(m.frame) / rate;
            params.markers.push_back(std::move(spec));
        }
        if (use_loop && buffer.loop && !params.loop) {
            params.loop = LoopSpec{static_cast<f64>(buffer.loop->start) / rate, static_cast<f64>(buffer.loop->end) / rate, -1};
        }
        if (!params.source_layout && buffer.layout.channels == buffer.channels) {
            params.source_layout = buffer.layout;
        }
    }

    void AudioEngine::seek(VoiceId voice, f64 seconds) {
        Command c;
        c.type = CommandType::Seek;
        c.id = voice;
        c.d[0] = seconds;
        impl_->push(c);
    }

    void AudioEngine::add_marker(VoiceId voice, const MarkerSpec &marker) {
        Command c;
        c.type = CommandType::AddMarker;
        c.id = voice;
        c.aux = marker.id;
        c.d = {marker.seconds, marker.jump_seconds};
        c.f[0] = static_cast<f32>(marker.action);
        c.f[1] = marker.seamless ? 1.0f : 0.0f;
        c.f[2] = static_cast<f32>(marker.max_triggers);
        c.f[3] = marker.fire_event ? 1.0f : 0.0f;
        impl_->push(c);
    }

    void AudioEngine::remove_marker(VoiceId voice, u32 marker_id) {
        Command c;
        c.type = CommandType::RemoveMarker;
        c.id = voice;
        c.aux = marker_id;
        impl_->push(c);
    }

    void AudioEngine::clear_markers(VoiceId voice) {
        Command c;
        c.type = CommandType::ClearMarkers;
        c.id = voice;
        impl_->push(c);
    }

    void AudioEngine::reset_marker(VoiceId voice, u32 marker_id, i32 triggers) {
        Command c;
        c.type = CommandType::ResetMarker;
        c.id = voice;
        c.aux = marker_id;
        c.f[0] = static_cast<f32>(triggers);
        impl_->push(c);
    }

    void AudioEngine::set_loop(VoiceId voice, const std::optional<LoopSpec> &loop) {
        Command c;
        c.type = CommandType::SetLoop;
        c.id = voice;
        c.f[0] = loop ? 1.0f : 0.0f;
        if (loop) {
            c.d = {loop->start_seconds, loop->end_seconds};
            c.f[1] = static_cast<f32>(loop->count);
        }
        impl_->push(c);
    }

    void AudioEngine::fade_volume(VoiceId voice, f32 target, f32 seconds) {
        Command c;
        c.type = CommandType::FadeVolume;
        c.id = voice;
        c.f[0] = target;
        c.f[1] = seconds;
        impl_->push(c);
    }

    void AudioEngine::fade_pitch(VoiceId voice, f32 target, f32 seconds) {
        Command c;
        c.type = CommandType::FadePitch;
        c.id = voice;
        c.f[0] = target;
        c.f[1] = seconds;
        impl_->push(c);
    }

    void AudioEngine::add_ducking(BusId target_bus, BusId trigger_bus, f32 reduction_db, f32 threshold_db, f32 attack_seconds, f32 release_seconds) {
        Command c;
        c.type = CommandType::AddDuck;
        c.id = target_bus;
        c.aux = trigger_bus;
        const f32 block_seconds = static_cast<f32>(impl_->cfg.block_frames) / static_cast<f32>(impl_->cfg.sample_rate);
        c.f[0] = std::pow(10.0f, -std::fabs(reduction_db) / 20.0f);
        c.f[1] = std::pow(10.0f, threshold_db / 20.0f);
        c.f[2] = 1.0f - std::exp(-block_seconds / std::max(attack_seconds, 1e-3f));
        c.f[3] = 1.0f - std::exp(-block_seconds / std::max(release_seconds, 1e-3f));
        impl_->push(c);
    }

    f64 AudioEngine::clock_seconds() const noexcept {
        return static_cast<f64>(impl_->clock_frames.load(std::memory_order_relaxed)) / static_cast<f64>(impl_->cfg.sample_rate);
    }

    std::shared_ptr<const VoiceStatus> AudioEngine::status(VoiceId voice) const {
        const u32 slot = voice & kVoiceSlotMask;
        if (slot >= impl_->status_owner.size() || impl_->status_owner[slot] != voice) {
            return nullptr;
        }
        return impl_->status_by_slot[slot];
    }

    void AudioEngine::poll_events(std::vector<VoiceEvent> &out) { impl_->events.drain_into(out); }

    BusLevels AudioEngine::bus_levels(BusId bus) const noexcept {
        if (bus >= kMaxBuses || !impl_->buses[bus]) {
            return {};
        }
        return BusLevels{impl_->buses[bus]->peak.load(std::memory_order_relaxed), impl_->buses[bus]->rms.load(std::memory_order_relaxed)};
    }

    void AudioEngine::copy_output_tap(OutputId output, f32 *out, u32 count) const noexcept {
        if (output >= impl_->outputs.size() || out == nullptr) {
            return;
        }
        const OutputState &o = impl_->outputs[output];
        count = std::min(count, kTapSize);
        const u32 end = o.tap_write->load(std::memory_order_acquire);
        for (u32 i = 0; i < count; ++i) {
            out[i] = o.tap[(end - count + i) & (kTapSize - 1)];
        }
    }

    void AudioEngine::stop(VoiceId voice, f32 fade_seconds) {
        Command c;
        c.type = CommandType::Stop;
        c.id = voice;
        c.f[0] = fade_seconds;
        impl_->push(c);
    }
    void AudioEngine::set_volume(VoiceId voice, f32 volume) {
        Command c;
        c.type = CommandType::SetVolume;
        c.id = voice;
        c.f[0] = volume;
        impl_->push(c);
    }
    void AudioEngine::set_pitch(VoiceId voice, f32 pitch) {
        Command c;
        c.type = CommandType::SetPitch;
        c.id = voice;
        c.f[0] = pitch;
        impl_->push(c);
    }
    void AudioEngine::set_position(VoiceId voice, const glm::vec3 &position, const glm::vec3 &velocity) {
        // Positions change for every moving source every frame, so they bypass the command queue: the latest value sits in a
        // per-voice slot the audio thread reads each block (a sequence counter makes the 24-byte update atomic as a whole).
        const u32 slot = voice & kVoiceSlotMask;
        if (slot >= impl_->status_owner.size() || impl_->status_owner[slot] != voice) {
            return;
        }
        PositionSlot &p = impl_->position_table[slot];
        const u32 sequence = p.sequence.load(std::memory_order_relaxed);
        p.sequence.store(sequence + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        p.owner.store(voice, std::memory_order_relaxed);
        const std::array<f32, 6> v{position.x, position.y, position.z, velocity.x, velocity.y, velocity.z};
        for (usize i = 0; i < 6; ++i) {
            p.values[i].store(v[i], std::memory_order_relaxed);
        }
        p.sequence.store(sequence + 2, std::memory_order_release);
    }
    void AudioEngine::set_paused(VoiceId voice, bool paused) {
        Command c;
        c.type = CommandType::SetPaused;
        c.id = voice;
        c.f[0] = paused ? 1.0f : 0.0f;
        impl_->push(c);
    }
    void AudioEngine::set_acoustics(VoiceId voice, const AcousticsResult &r) {
        Command c;
        c.type = CommandType::SetAcoustics;
        c.id = voice;
        c.f = {r.broadband_gain, r.band_gain[0], r.band_gain[1], r.band_gain[2], r.lowpass_cutoff, r.extra_delay_seconds,
               r.apparent_direction.x, r.apparent_direction.y, r.apparent_direction.z, r.has_apparent_direction ? 1.0f : 0.0f,
               r.reverb_send, r.reverb_rt60, r.doppler, r.has_doppler ? 1.0f : 0.0f};
        impl_->push(c);
    }
    void AudioEngine::set_listener(const ListenerState &l) {
        Command c;
        c.type = CommandType::SetListener;
        c.f = {l.position.x, l.position.y, l.position.z, l.rotation.w, l.rotation.x, l.rotation.y, l.rotation.z,
               l.velocity.x, l.velocity.y, l.velocity.z};
        impl_->push(c);
    }
    void AudioEngine::set_output_volume(OutputId output, f32 volume) { set_bus_volume(master_bus(output), volume); }

    std::vector<VoiceId> AudioEngine::pump() {
        Impl &m = *impl_;
        m.retired_scratch.clear();
        m.retired.drain_into(m.retired_scratch);
        for (const Retired &r : m.retired_scratch) {
            if (r.kind == RetiredKind::Voice) {
                // Back to the pool: the next play() reuses its buffers instead of allocating.
                m.give_back(std::unique_ptr<Voice>(static_cast<Voice *>(r.ptr)));
            } else {
                delete static_cast<BusEffect *>(r.ptr);
            }
        }
        std::vector<VoiceId> done;
        m.finished.drain_into(done);
        for (VoiceId id : done) {
            const u32 slot = id & kVoiceSlotMask;
            if (slot < m.status_owner.size() && m.status_owner[slot] == id) {
                m.status_by_slot[slot].reset();
                m.status_owner[slot] = 0;
                m.free_slots.push_back(slot);
            }
        }
        return done;
    }

    AudioStats AudioEngine::stats() const noexcept {
        AudioStats s;
        s.voices = impl_->stat_voices.load(std::memory_order_relaxed);
        s.physical = impl_->stat_physical.load(std::memory_order_relaxed);
        s.virtualised = impl_->stat_virtual.load(std::memory_order_relaxed);
        s.dropped_commands = impl_->stat_dropped.load(std::memory_order_relaxed);
        s.master_peak = std::bit_cast<f32>(impl_->stat_peak_bits.load(std::memory_order_relaxed));
        s.mix_threads = static_cast<u32>(impl_->workers.size());
        s.simd = Kernels::active_isa();
        s.compute_voices = impl_->stat_compute.load(std::memory_order_relaxed);
        return s;
    }

    const AudioBuffer &AudioEngine::bed(OutputId output) const noexcept {
        static const AudioBuffer empty;
        if (output >= impl_->outputs.size() || !impl_->buses[output]) {
            return empty;
        }
        return impl_->buses[output]->buffer;
    }

    const ObjectBlock &AudioEngine::objects(OutputId output) const noexcept {
        static const ObjectBlock empty;
        return output < impl_->outputs.size() ? impl_->outputs[output].objects : empty;
    }

    // ---- audio thread -------------------------------------------------------------------------------------------------

    void AudioEngine::Impl::apply_commands() {
        command_scratch.clear();
        commands.drain_into(command_scratch);
        Worker &main_worker = *workers[0];
        const auto find_voice = [this](VoiceId id) -> Voice * {
            const u32 slot = id & kVoiceSlotMask;
            Voice *v = slot < by_slot.size() ? by_slot[slot] : nullptr;
            return v != nullptr && v->id == id ? v : nullptr;
        };
        for (const Command &c : command_scratch) {
            switch (c.type) {
                case CommandType::Play: {
                    auto *voice = static_cast<Voice *>(c.ptr);
                    if (voices.size() >= cfg.max_voices) {
                        voice->status->publish(0.0, VoiceStatus::State::Finished, 0.0f, 1.0f);
                        (void)retired.try_push(Retired{voice, RetiredKind::Voice});
                        (void)finished.try_push(voice->id);
                    } else {
                        const f64 sr = static_cast<f64>(cfg.sample_rate);
                        if (voice->start_at_seconds >= 0.0) {
                            const f64 target = voice->start_at_seconds * sr;
                            voice->delay_frames = target > static_cast<f64>(clock_local) ? static_cast<u64>(target - static_cast<f64>(clock_local)) : 0;
                        } else {
                            voice->delay_frames = static_cast<u64>(static_cast<f64>(voice->start_delay_seconds) * sr);
                        }
                        voice->started_event_due = voice->delay_frames > 0;
                        by_slot[voice->slot] = voice;
                        ambisonic_voices += voice->ambisonic ? 1 : 0;
                        voices.emplace_back(voice);
                    }
                    break;
                }
                case CommandType::AddDuck:
                    if (duck_rules.size() < duck_rules.capacity() && c.id < kMaxBuses && c.aux < kMaxBuses) {
                        DuckRule rule;
                        rule.target = c.id;
                        rule.trigger = c.aux;
                        rule.floor_gain = c.f[0];
                        rule.threshold = c.f[1];
                        rule.attack = c.f[2];
                        rule.release = c.f[3];
                        duck_rules.push_back(rule);
                    }
                    break;
                case CommandType::AddBus: {
                    auto *bus = static_cast<Bus *>(c.ptr);
                    if (c.id < kMaxBuses && !buses[c.id]) {
                        bus->index = c.id;
                        buses[c.id].reset(bus);
                        bus_order.push_back(bus);
                        std::stable_sort(bus_order.begin(), bus_order.end(), [](const Bus *a, const Bus *b) { return a->depth > b->depth; });
                    } else {
                        delete bus;
                    }
                    break;
                }
                case CommandType::AddEffect: {
                    auto *effect = static_cast<BusEffect *>(c.ptr);
                    if (c.id < kMaxBuses && buses[c.id] && buses[c.id]->effects.size() < kMaxEffectsPerBus) {
                        // The master's limiter must stay last: effects go in front of it.
                        auto &list = buses[c.id]->effects;
                        const bool master = buses[c.id]->parent == no_bus;
                        list.insert(master && !list.empty() ? list.end() - 1 : list.end(), std::unique_ptr<BusEffect>(effect));
                    } else {
                        (void)retired.try_push(Retired{effect, RetiredKind::Effect});
                    }
                    break;
                }
                case CommandType::SetEffectParameter:
                    if (c.id < kMaxBuses && buses[c.id]) {
                        auto &list = buses[c.id]->effects;
                        const u32 index = c.aux >> 16;
                        if (index < list.size()) {
                            list[index]->set_parameter(c.aux & 0xFFFFu, c.f[0]);
                        }
                    }
                    break;
                case CommandType::SetBusVolume:
                    if (c.id < kMaxBuses && buses[c.id]) buses[c.id]->volume = std::max(c.f[0], 0.0f);
                    break;
                case CommandType::SetBusMuted:
                    if (c.id < kMaxBuses && buses[c.id]) buses[c.id]->muted = c.f[0] > 0.5f;
                    break;
                case CommandType::SetListener:
                    listener.position = {c.f[0], c.f[1], c.f[2]};
                    listener.rotation = glm::normalize(glm::quat(c.f[3], c.f[4], c.f[5], c.f[6]));
                    listener.velocity = {c.f[7], c.f[8], c.f[9]};
                    break;
                default: {
                    Voice *voice = find_voice(c.id);
                    if (voice == nullptr) {
                        if (c.type == CommandType::AddVoiceEffect) {
                            (void)retired.try_push(Retired{c.ptr, RetiredKind::Effect}); // the voice is gone: just free it
                        }
                        break;
                    }
                    switch (c.type) {
                        case CommandType::Stop:
                            voice->stopping = true;
                            voice->fade_step = c.f[0] > 0.0f ? static_cast<f32>(cfg.block_frames) / (static_cast<f32>(cfg.sample_rate) * c.f[0]) : 1.0f;
                            break;
                        case CommandType::SetVolume: voice->volume = std::max(c.f[0], 0.0f); break;
                        case CommandType::SetPitch: voice->pitch = std::max(c.f[0], 0.0f); break;
                        case CommandType::SetPosition:
                            voice->position = {c.f[0], c.f[1], c.f[2]};
                            voice->velocity = {c.f[3], c.f[4], c.f[5]};
                            break;
                        case CommandType::SetPaused: voice->paused = c.f[0] > 0.5f; break;
                        case CommandType::Seek: seek_voice(*voice, c.d[0], true, true, main_worker); break;
                        case CommandType::AddVoiceEffect: {
                            auto *effect = static_cast<AudioEffect *>(c.ptr);
                            if (voice->effects.size() < kMaxEffectsPerVoice) {
                                voice->effects.emplace_back(effect);
                            } else {
                                (void)retired.try_push(Retired{effect, RetiredKind::Effect});
                            }
                            break;
                        }
                        case CommandType::SetVoiceEffectParameter: {
                            const u32 index = c.aux >> 16;
                            if (index < voice->effects.size()) {
                                voice->effects[index]->set_parameter(c.aux & 0xFFFFu, c.f[0]);
                            }
                            break;
                        }
                        case CommandType::AddMarker: {
                            if (voice->markers.size() < voice->markers.capacity()) {
                                const f64 native = static_cast<f64>(std::max(voice->source->native_rate(), 1u));
                                const u64 position = voice->source->position_frames();
                                Voice::Marker m;
                                m.id = c.aux;
                                m.frame = static_cast<u64>(std::max(0.0, std::round(c.d[0] * native)));
                                m.action = static_cast<MarkerAction>(static_cast<u8>(c.f[0]));
                                m.target = static_cast<u64>(std::max(0.0, std::round(c.d[1] * native)));
                                m.seamless = c.f[1] > 0.5f;
                                m.remaining = static_cast<i32>(c.f[2]);
                                m.fire = c.f[3] > 0.5f;
                                m.fade_seconds = static_cast<f32>(c.d[1]);
                                m.armed = m.frame >= position && m.remaining != 0;
                                voice->markers.push_back(m);
                            }
                            break;
                        }
                        case CommandType::RemoveMarker:
                            voice->markers.erase(std::remove_if(voice->markers.begin(), voice->markers.end(), [&](const Voice::Marker &m) { return m.id == c.aux; }),
                                                 voice->markers.end());
                            break;
                        case CommandType::ClearMarkers: voice->markers.clear(); break;
                        case CommandType::ResetMarker: {
                            const u64 position = voice->source->position_frames();
                            for (Voice::Marker &m : voice->markers) {
                                if (m.id == c.aux) {
                                    m.remaining = static_cast<i32>(c.f[0]);
                                    m.armed = m.remaining != 0 && m.frame >= position;
                                }
                            }
                            break;
                        }
                        case CommandType::SetLoop: {
                            voice->markers.erase(std::remove_if(voice->markers.begin(), voice->markers.end(), [](const Voice::Marker &m) { return m.id == kLoopMarkerId; }),
                                                 voice->markers.end());
                            if (c.f[0] > 0.5f && c.d[1] > c.d[0] && voice->markers.size() < voice->markers.capacity()) {
                                const f64 native = static_cast<f64>(std::max(voice->source->native_rate(), 1u));
                                Voice::Marker m;
                                m.id = kLoopMarkerId;
                                m.frame = static_cast<u64>(std::llround(c.d[1] * native));
                                m.action = MarkerAction::Jump;
                                m.target = static_cast<u64>(std::llround(c.d[0] * native));
                                m.seamless = true;
                                m.remaining = static_cast<i32>(c.f[1]);
                                m.armed = m.remaining != 0 && m.frame >= voice->source->position_frames();
                                voice->markers.push_back(m);
                            }
                            break;
                        }
                        case CommandType::FadeVolume:
                        case CommandType::FadePitch: {
                            Voice::Ramp &ramp = c.type == CommandType::FadeVolume ? voice->volume_ramp : voice->pitch_ramp;
                            f32 &value = c.type == CommandType::FadeVolume ? voice->volume : voice->pitch;
                            const f32 blocks = std::max(c.f[1] * static_cast<f32>(cfg.sample_rate) / static_cast<f32>(cfg.block_frames), 0.0f);
                            if (blocks < 1.0f) {
                                value = std::max(c.f[0], 0.0f);
                                ramp.active = false;
                            } else {
                                ramp.active = true;
                                ramp.target = std::max(c.f[0], 0.0f);
                                ramp.step = (ramp.target - value) / blocks;
                            }
                            break;
                        }
                        case CommandType::SetAcoustics: {
                            AcousticsResult &r = voice->acoustics_target;
                            r.broadband_gain = c.f[0];
                            r.band_gain = {c.f[1], c.f[2], c.f[3]};
                            r.lowpass_cutoff = c.f[4];
                            r.extra_delay_seconds = c.f[5];
                            r.apparent_direction = {c.f[6], c.f[7], c.f[8]};
                            r.has_apparent_direction = c.f[9] > 0.5f;
                            r.reverb_send = c.f[10];
                            r.reverb_rt60 = c.f[11];
                            r.doppler = c.f[12];
                            r.has_doppler = c.f[13] > 0.5f;
                            break;
                        }
                        default: break;
                    }
                }
            }
        }
    }

    f32 AudioEngine::Impl::bus_chain_gain(const Bus &bus) const {
        f32 gain = 1.0f;
        const Bus *b = &bus;
        while (b != nullptr) {
            gain *= b->muted ? 0.0f : b->volume;
            b = b->parent != no_bus && b->parent < kMaxBuses ? buses[b->parent].get() : nullptr;
        }
        return gain;
    }

    void AudioEngine::Impl::prepare_voice(Voice &v) {
        Frame &f = v.frame;
        // The latest position the game set (if it set one for this voice since it started).
        {
            PositionSlot &p = position_table[v.slot];
            for (int attempt = 0; attempt < 3; ++attempt) {
                const u32 before = p.sequence.load(std::memory_order_acquire);
                if ((before & 1u) != 0) {
                    continue; // mid-write: try again
                }
                if (p.owner.load(std::memory_order_relaxed) != v.id) {
                    break; // not set for this voice yet
                }
                std::array<f32, 6> values;
                for (usize i = 0; i < 6; ++i) {
                    values[i] = p.values[i].load(std::memory_order_relaxed);
                }
                std::atomic_thread_fence(std::memory_order_acquire);
                if (p.sequence.load(std::memory_order_relaxed) == before) {
                    v.position = {values[0], values[1], values[2]};
                    v.velocity = {values[3], values[4], values[5]};
                    break;
                }
            }
        }
        // Fades (stopping voices fade out over blocks).
        if (v.stopping) {
            v.fade = std::max(0.0f, v.fade - v.fade_step);
        }
        // A voice that was asked to fade in starts silent and ramps up from the block it begins sounding in.
        if (!v.start_fade_applied && v.delay_frames < cfg.block_frames) {
            v.start_fade_applied = true;
            if (v.start_fade_seconds > 0.0f) {
                const f32 blocks = std::max(v.start_fade_seconds * static_cast<f32>(cfg.sample_rate) / static_cast<f32>(cfg.block_frames), 1.0f);
                v.volume_ramp = Voice::Ramp{true, v.volume, v.volume / blocks};
                v.volume = 0.0f;
            }
        }
        // Volume and pitch fades requested by game code.
        for (auto [ramp, value] : {std::pair<Voice::Ramp *, f32 *>{&v.volume_ramp, &v.volume}, std::pair<Voice::Ramp *, f32 *>{&v.pitch_ramp, &v.pitch}}) {
            if (ramp->active) {
                *value += ramp->step;
                if ((ramp->step >= 0.0f && *value >= ramp->target) || (ramp->step < 0.0f && *value <= ramp->target)) {
                    *value = ramp->target;
                    ramp->active = false;
                }
            }
        }
        // Smooth acoustics toward their target so an occluder appearing does not click.
        const f32 s = 0.35f;
        v.acoustics.broadband_gain += (v.acoustics_target.broadband_gain - v.acoustics.broadband_gain) * s;
        v.acoustics.lowpass_cutoff += (v.acoustics_target.lowpass_cutoff - v.acoustics.lowpass_cutoff) * s;
        v.acoustics.extra_delay_seconds += (v.acoustics_target.extra_delay_seconds - v.acoustics.extra_delay_seconds) * s;
        v.acoustics.reverb_send += (v.acoustics_target.reverb_send - v.acoustics.reverb_send) * s;
        v.acoustics.doppler += (v.acoustics_target.doppler - v.acoustics.doppler) * s;
        v.acoustics.has_doppler = v.acoustics_target.has_doppler;
        v.acoustics.has_apparent_direction = v.acoustics_target.has_apparent_direction;
        v.acoustics.apparent_direction = v.acoustics_target.apparent_direction;

        f.gain = v.volume * v.fade;
        f.doppler = 1.0f;
        f.spread = v.spread;
        f.distance = 0.0f;
        if (v.spatial) {
            const bool world = v.space == SourceSpace::World;
            glm::vec3 relative = v.position;
            if (world) {
                relative = inverse_listener_rotation * (v.position - listener.position);
            }
            f.distance = glm::length(relative);
            f.direction = f.distance > 1e-4f ? relative / f.distance : glm::vec3(0.0f, 0.0f, -1.0f);
            f.gain *= distance_gain(v.distance, f.distance);
            if (world) {
                if (v.use_acoustics) {
                    f.gain *= v.acoustics.broadband_gain;
                    if (v.acoustics.has_apparent_direction) {
                        f.direction = glm::normalize(inverse_listener_rotation * v.acoustics.apparent_direction);
                    }
                }
                // The acoustics provider's Doppler (measured along the path the sound really takes) replaces the straight-line one.
                f.doppler = v.use_acoustics && v.acoustics.has_doppler
                                ? v.acoustics.doppler * 1.0f
                                : doppler_ratio(v.position, v.velocity, listener.position, listener.velocity, cfg.speed_of_sound, v.doppler_factor);
            }
            // A source inside its minimum distance is large around the listener: widen it.
            if (f.distance < v.distance.min_distance && v.distance.min_distance > 0.0f) {
                f.spread = std::clamp(f.spread + (1.0f - f.distance / v.distance.min_distance) * 0.5f, 0.0f, 1.0f);
            }
        }
        f.score = f.gain * v.priority;
    }

    void AudioEngine::Impl::emit(Worker &worker, VoiceId voice, VoiceEvent::Kind kind, u32 marker_id, f64 position_seconds) {
        if (worker.events.size() < kWorkerEventCapacity) {
            worker.events.push_back(VoiceEvent{voice, kind, marker_id, position_seconds});
        }
    }

    void AudioEngine::Impl::rearm_markers(Voice &v, u64 position) {
        for (Voice::Marker &m : v.markers) {
            m.armed = m.remaining != 0 && m.frame >= position;
        }
    }

    void AudioEngine::Impl::seek_voice(Voice &v, f64 seconds, bool declick, bool emit_event, Worker &worker) {
        if (!v.source->seekable()) {
            return;
        }
        const f64 native = static_cast<f64>(std::max(v.source->native_rate(), 1u));
        const u64 frame = static_cast<u64>(std::max(0.0, std::round(seconds * native)));
        v.source->seek_frames(frame);
        rearm_markers(v, frame);
        if (declick) {
            v.fade_in_remaining = v.declick_frames;
        }
        if (emit_event) {
            emit(worker, v.id, VoiceEvent::Kind::Jumped, 0, seconds);
        }
    }

    bool AudioEngine::Impl::trigger_marker(Voice &v, Voice::Marker &m, u32 written, Worker &worker) {
        const f64 native = static_cast<f64>(std::max(v.source->native_rate(), 1u));
        const f64 at_seconds = static_cast<f64>(m.frame) / native;
        const bool is_loop = m.id == kLoopMarkerId;
        m.armed = false; // re-armed by the next seek that lands before it
        if (m.remaining > 0) {
            --m.remaining;
        }
        if (m.fire && !is_loop) {
            emit(worker, v.id, VoiceEvent::Kind::MarkerHit, m.id, at_seconds);
        }
        switch (m.action) {
            case MarkerAction::None: return true;
            case MarkerAction::Jump: {
                if (!m.seamless && v.declick_frames > 0 && written > 0) {
                    // Fade out the audio just before the seam; the destination fades in.
                    const u32 d = std::min(v.declick_frames, written);
                    for (u32 c = 0; c < v.src.channels(); ++c) {
                        f32 *data = v.src.data(c) + (written - d);
                        for (u32 i = 0; i < d; ++i) {
                            data[i] *= 1.0f - static_cast<f32>(i + 1) / static_cast<f32>(d);
                        }
                    }
                }
                v.source->seek_frames(m.target);
                rearm_markers(v, m.target);
                if (!m.seamless) {
                    v.fade_in_remaining = v.declick_frames;
                }
                emit(worker, v.id, is_loop ? VoiceEvent::Kind::Looped : VoiceEvent::Kind::Jumped, m.id, static_cast<f64>(m.target) / native);
                return true;
            }
            case MarkerAction::Stop: v.ending = true; return false;
            case MarkerAction::FadeStop: {
                v.stopping = true;
                v.fade_step = m.fade_seconds > 0.0f ? static_cast<f32>(cfg.block_frames) / (static_cast<f32>(cfg.sample_rate) * m.fade_seconds) : 1.0f;
                return true;
            }
            case MarkerAction::Pause:
                v.paused = true;
                emit(worker, v.id, VoiceEvent::Kind::Paused, m.id, at_seconds);
                return false;
        }
        return true;
    }

    // Reads one block of a voice's source into `v.src`, honouring the start delay and splitting the block at every waypoint
    // so jumps, loops and stops land on the exact sample. Returns the number of frames of the block that carry audio
    // (0 when the voice is still waiting to start).
    u32 AudioEngine::Impl::fill_source(Voice &v, u32 frames, Worker &worker) {
        v.src.clear();
        u32 written = 0;
        if (v.delay_frames > 0) {
            if (v.delay_frames >= frames) {
                v.delay_frames -= frames;
                return 0;
            }
            written = static_cast<u32>(v.delay_frames);
            v.delay_frames = 0;
        }
        if (v.started_event_due) {
            v.started_event_due = false;
            emit(worker, v.id, VoiceEvent::Kind::Started, 0, static_cast<f64>(v.source->position_frames()) / static_cast<f64>(std::max(v.source->native_rate(), 1u)));
        }

        const f64 step = static_cast<f64>(v.pitch) * v.frame.doppler * static_cast<f64>(std::max(v.source->native_rate(), 1u)) / static_cast<f64>(cfg.sample_rate);
        const bool use_markers = v.source->seekable() && !v.markers.empty();
        u32 budget = 64; // a loop of zero length must not spin forever

        while (written < frames) {
            u32 chunk = frames - written;
            Voice::Marker *next = nullptr;
            if (use_markers && step > 1e-9) {
                const f64 position = static_cast<f64>(v.source->position_frames());
                for (Voice::Marker &m : v.markers) {
                    if (!m.armed || static_cast<f64>(m.frame) + 0.5 < position) {
                        continue;
                    }
                    if (next == nullptr || m.frame < next->frame) {
                        next = &m;
                    }
                }
                if (next != nullptr) {
                    const f64 until = (static_cast<f64>(next->frame) - position) / step;
                    if (until >= static_cast<f64>(chunk)) {
                        next = nullptr; // beyond this block
                    } else {
                        chunk = until <= 0.0 ? 0u : static_cast<u32>(std::ceil(until));
                    }
                }
            }

            bool ended = false;
            if (chunk > 0) {
                const u32 got = v.source->read(v.segment, chunk);
                for (u32 c = 0; c < v.src.channels(); ++c) {
                    std::copy_n(v.segment.data(c), got, v.src.data(c) + written);
                }
                if (v.fade_in_remaining > 0 && got > 0 && v.declick_frames > 0) {
                    const u32 n = std::min(v.fade_in_remaining, got);
                    for (u32 i = 0; i < n; ++i) {
                        const f32 g = 1.0f - static_cast<f32>(v.fade_in_remaining - i) / static_cast<f32>(v.declick_frames);
                        for (u32 c = 0; c < v.src.channels(); ++c) {
                            v.src.data(c)[written + i] *= g;
                        }
                    }
                    v.fade_in_remaining -= n;
                }
                written += got;
                ended = got < chunk;
            }

            if (ended) {
                // A loop marker sitting at the very end of the source takes the place of "the source ended".
                Voice::Marker *loop = nullptr;
                if (use_markers) {
                    const u64 length = v.source->length_frames();
                    for (Voice::Marker &m : v.markers) {
                        if (m.armed && m.action == MarkerAction::Jump && length > 0 && m.frame + 1 >= length) {
                            loop = &m;
                            break;
                        }
                    }
                }
                if (loop != nullptr && budget-- > 0 && trigger_marker(v, *loop, written, worker)) {
                    continue;
                }
                v.ending = true;
                break;
            }
            if (next == nullptr) {
                break;
            }
            if (budget-- == 0 || !trigger_marker(v, *next, written, worker)) {
                break;
            }
        }
        return written;
    }

    // ---- compute-mode mixing ----------------------------------------------------------------------------------------

    // Decides whether `v` can be mixed by the compute backend this block and, if so, performs everything render_voice would
    // (advancing the source, updating gain-ramp state) except the arithmetic, which becomes records for the backend. Only
    // voices whose whole signal path is "resample a loaded sample, scale by a ramp, add to bus channels" qualify; anything
    // with filters, delay lines, effects, markers, aux sends, objects, binaural or ambisonic output stays on the CPU.
    bool AudioEngine::Impl::plan_compute_voice(Voice &v) {
        ComputeState &cs = compute;
        BufferSource *source = v.plain_buffer;
        if (source == nullptr || v.ambisonic || !v.markers.empty() || !v.effects.empty() || v.aux_bus != no_bus || v.delay_frames > 0 ||
            v.started_event_due || v.fade_in_remaining > 0 || v.delay_active || v.buses.empty()) {
            return false;
        }
        const std::shared_ptr<const SampleBuffer> &buffer = source->buffer();
        if (!buffer || !buffer->samples || buffer->frames() == 0 || source->finished()) {
            return false;
        }
        const u32 frames = cfg.block_frames;
        Frame &f = v.frame;
        const bool world_spatial = v.spatial && v.space == SourceSpace::World;
        if (world_spatial && v.delay.capacity() > 0 && v.acoustics.extra_delay_seconds > 0.0005f) {
            return false;
        }
        for (usize k = 0; k < v.buses.size(); ++k) {
            const Bus *bus = buses[v.buses[k]].get();
            if (bus == nullptr) {
                continue;
            }
            const OutputState &out = outputs[bus->output];
            if (out.desc.kind == OutputDesc::Kind::Binaural || (out.desc.kind == OutputDesc::Kind::Objects && out.desc.objects_consumed) ||
                (v.spatial && v.panner == Panner::Ambisonic && out.ambi.channels() > 0)) {
                return false;
            }
        }
        const ComputeBackendInfo info = cfg.compute_backend->info();
        if (cs.voices.size() >= info.max_voices || cs.taps.size() + 4096 > info.max_taps) {
            return false;
        }
        auto found = cs.samples.find(buffer->samples.get());
        if (found == cs.samples.end()) {
            // First time this sample is seen: make it resident and play it on the CPU until the backend has it.
            ComputeSample entry;
            entry.id = cfg.compute_backend->register_sample(buffer->samples, buffer->channels);
            entry.keep = buffer;
            entry.last_block = cs.block;
            cs.samples.emplace(buffer->samples.get(), std::move(entry));
            return false;
        }
        if (!cfg.compute_backend->sample_ready(found->second.id)) {
            return false;
        }
        found->second.last_block = cs.block;

        source->set_rate(v.pitch * f.doppler);
        const BufferSource::BlockPlan plan = source->plan_block(frames);
        if (plan.valid_frames < frames && !plan.loop) {
            v.ending = true;
        }
        if (plan.valid_frames == 0) {
            return true;
        }
        const u32 channels = buffer->channels;
        ComputeVoice record{found->second.id, plan.loop ? 1u : 0u, 0, plan.valid_frames, plan.base, plan.frac, plan.step};
        record.first_signal = cs.signal_count;
        record.mix_down = v.spatial ? 1u : 0u;
        cs.signal_count += v.spatial ? 1u : channels;
        if (world_spatial) {
            // The device runs the filter itself, so hand it the CPU path's design and state.
            if (update_spatial_lowpass(v)) {
                const BiquadCoefficients c = v.lowpass.coefficients();
                const auto [z1, z2] = v.lowpass.state();
                record.filter = 1;
                record.b0 = c.b0, record.b1 = c.b1, record.b2 = c.b2, record.a1 = c.a1, record.a2 = c.a2;
                record.z1 = z1, record.z2 = z2;
            }
        }
        cs.voices.push_back(record);
        cs.owners.push_back(&v);

        auto add_tap = [&](u32 signal, const Bus &bus, u32 channel, f32 from, f32 to) {
            const u32 key = bus.index * max_channels + channel;
            i32 &dense = cs.destination_lookup[key];
            if (dense < 0) {
                dense = static_cast<i32>(cs.destination_keys.size());
                cs.destination_keys.push_back(key);
            }
            cs.taps.push_back(ComputeTap{signal, static_cast<u32>(dense), from, to});
        };
        const f32 gain = f.gain;
        for (usize k = 0; k < v.buses.size(); ++k) {
            Bus *bus = buses[v.buses[k]].get();
            if (bus == nullptr) {
                continue;
            }
            OutputState &out = outputs[bus->output];
            VoiceOutputState &state = v.states[k];
            const f32 from = state.initialised ? state.previous_gain : gain;
            if (!v.spatial) {
                for (const MatrixTap &t : state.matrix.taps) {
                    if (t.destination < bus->buffer.channels() && t.source < channels) {
                        add_tap(record.first_signal + t.source, *bus, t.destination, from * t.gain, gain * t.gain);
                    }
                }
            } else {
                std::array<f32, max_channels> &scratch = workers[0]->gain_scratch;
                out.vbap.gains(f.direction, f.spread, scratch);
                const u32 layout_channels = out.desc.layout.channel_count();
                for (u32 c = 0; c < layout_channels && c < bus->buffer.channels(); ++c) {
                    const f32 target = gain * scratch[c];
                    const f32 start = state.initialised ? state.previous_gains[c] : target;
                    if (target != 0.0f || start != 0.0f) {
                        add_tap(record.first_signal, *bus, c, start, target);
                    }
                    state.previous_gains[c] = target;
                }
            }
            state.previous_gain = gain;
            state.initialised = true;
        }
        return true;
    }

    // Plans every qualifying voice, hands the block to the backend, and removes those voices from the CPU list.
    void AudioEngine::Impl::compute_begin() {
        ComputeState &cs = compute;
        cs.handled = 0;
        cs.submitted = false;
        ++cs.block;
        if (!cfg.compute_backend || physical_list.size() < cfg.compute_min_voices) {
            return;
        }
        if (cs.destination_lookup.empty()) {
            cs.destination_lookup.assign(static_cast<usize>(kMaxBuses) * max_channels, -1);
        }
        cs.voices.clear();
        cs.owners.clear();
        cs.signal_count = 0;
        cs.taps.clear();
        cs.remaining.clear();
        cs.destination_keys.clear();
        for (Voice *v : physical_list) {
            if (plan_compute_voice(*v)) {
                ++cs.handled;
            } else {
                cs.remaining.push_back(v);
            }
        }
        // Release samples nothing has used for a while so the backend's memory follows what is actually playing.
        if ((cs.block & 1023u) == 0) {
            for (auto it = cs.samples.begin(); it != cs.samples.end();) {
                if (cs.block - it->second.last_block > 4096) {
                    cfg.compute_backend->release_sample(it->second.id);
                    it = cs.samples.erase(it);
                } else {
                    ++it;
                }
            }
        }
        physical_list.swap(cs.remaining);
        // Group taps by destination (counting sort) so the backend can reduce each destination without atomics.
        const usize destinations = cs.destination_keys.size();
        cs.counts.assign(destinations + 1, 0);
        for (const ComputeTap &t : cs.taps) {
            ++cs.counts[t.destination + 1];
        }
        for (usize d = 0; d < destinations; ++d) {
            cs.counts[d + 1] += cs.counts[d];
        }
        cs.sorted.resize(cs.taps.size());
        for (const ComputeTap &t : cs.taps) {
            cs.sorted[cs.counts[t.destination]++] = t;
        }
        cfg.compute_backend->submit(ComputeBlock{cfg.block_frames, static_cast<u32>(destinations), cs.signal_count, cs.voices, cs.sorted});
        cs.submitted = true;
    }

    // Waits for the backend and adds its result to the buses the voices were routed to.
    void AudioEngine::Impl::compute_end() {
        ComputeState &cs = compute;
        stat_compute.store(cs.handled, std::memory_order_relaxed);
        if (!cs.submitted) {
            return;
        }
        const u32 frames = cfg.block_frames;
        const ComputeResult result = cfg.compute_backend->collect();
        for (usize i = 0; i < cs.owners.size() && (i + 1) * 2 <= result.filter_state.size(); ++i) {
            if (cs.voices[i].filter != 0) {
                cs.owners[i]->lowpass.set_state(result.filter_state[i * 2], result.filter_state[i * 2 + 1]);
            }
        }
        for (usize d = 0; d < cs.destination_keys.size(); ++d) {
            const u32 key = cs.destination_keys[d];
            cs.destination_lookup[key] = -1;
            Bus *bus = buses[key / max_channels].get();
            if (bus != nullptr && (d + 1) * frames <= result.mix.size()) {
                Kernels::add(bus->buffer.data(key % max_channels), result.mix.data() + d * frames, frames);
                bus->has_input = true;
            }
        }
        cs.submitted = false;
    }

    // An ambisonic source is a sound field, not a point: it is turned with the listener's head (unless it is listener-relative)
    // and handed to the output's decoder, or folded to the headphone bed.
    void AudioEngine::Impl::render_ambisonic(Voice &v, Worker &w) {
        const u32 frames = cfg.block_frames;
        const AudioBuffer *field = &v.src;
        if (v.spatial && v.space == SourceSpace::World) {
            rotator.apply(v.src, w.rotated, frames);
            field = &w.rotated;
        }
        const f32 gain = v.frame.gain;
        for (usize k = 0; k < v.buses.size(); ++k) {
            Bus *bus = buses[v.buses[k]].get();
            if (bus == nullptr) {
                continue;
            }
            OutputState &out = outputs[bus->output];
            VoiceOutputState &state = v.states[k];
            const f32 from = state.initialised ? state.previous_gain : gain;
            if (out.desc.kind != OutputDesc::Kind::Binaural && out.ambi.channels() > 0) {
                AudioBuffer &target = ambi_target(bus->output, w);
                const u32 count = std::min(field->channels(), target.channels());
                for (u32 acn = 0; acn < count; ++acn) {
                    Kernels::add_ramp(target.data(acn), field->data(acn), frames, from, gain);
                }
            } else {
                state.matrix.apply_ramped(*field, bus_target(*bus, w), frames, from, gain);
            }
            state.previous_gain = gain;
            state.initialised = true;
        }
    }

    bool AudioEngine::Impl::update_spatial_lowpass(Voice &v) {
        f32 cutoff = air_absorption_cutoff(v.frame.distance);
        if (v.use_acoustics) {
            cutoff = std::min(cutoff, v.acoustics.lowpass_cutoff);
        }
        if (cutoff >= 19500.0f && !v.lowpass_active) {
            return false;
        }
        if (!v.lowpass_active || std::fabs(cutoff - v.applied_cutoff) > 0.05f * v.applied_cutoff) {
            v.lowpass.set(FilterType::LowPass, static_cast<f32>(cfg.sample_rate), std::min(cutoff, 20000.0f));
            v.applied_cutoff = cutoff;
            v.lowpass_active = true;
        }
        return true;
    }

    void AudioEngine::Impl::render_voice(Voice &v, Worker &w) {
        const u32 frames = cfg.block_frames;
        Frame &f = v.frame;

        v.source->set_rate(v.pitch * f.doppler);
        const u32 produced = fill_source(v, frames, w);
        if (produced == 0) {
            return;
        }
        for (auto &effect : v.effects) {
            effect->process(v.src);
        }
        if (v.ambisonic) {
            render_ambisonic(v, w);
            return;
        }

        const u32 channels = v.src.channels();
        // The mono mix is only needed to spatialise a voice or to feed an aux send.
        const bool needs_mono = v.spatial || v.aux_bus != no_bus;
        if (needs_mono) {
            f32 *mono = w.mono.data();
            std::copy_n(v.src.data(0), frames, mono);
            for (u32 c = 1; c < channels; ++c) {
                Kernels::add(mono, v.src.data(c), frames);
            }
            if (channels > 1) {
                Kernels::scale(mono, 1.0f / static_cast<f32>(channels), frames);
            }
        }
        f32 *mono = w.mono.data();

        const bool world_spatial = v.spatial && v.space == SourceSpace::World;
        if (world_spatial) {
            if (update_spatial_lowpass(v)) {
                v.lowpass.process(std::span<f32>(mono, frames));
            }
            if (v.delay.capacity() > 0 && (v.acoustics.extra_delay_seconds > 0.0005f || v.delay_active)) {
                const f32 delay = 1.0f + v.acoustics.extra_delay_seconds * static_cast<f32>(cfg.sample_rate);
                for (u32 i = 0; i < frames; ++i) {
                    v.delay.write(mono[i]);
                    mono[i] = v.delay.read(delay);
                }
                v.delay_active = true;
            }
        }

        for (usize k = 0; k < v.buses.size(); ++k) {
            Bus *bus = buses[v.buses[k]].get();
            if (bus == nullptr) {
                continue;
            }
            OutputState &out = outputs[bus->output];
            VoiceOutputState &state = v.states[k];
            const f32 gain = f.gain;

            if (!v.spatial) {
                const f32 from = state.initialised ? state.previous_gain : gain;
                state.matrix.apply_ramped(v.src, bus_target(*bus, w), frames, from, gain);
                state.previous_gain = gain;
                state.initialised = true;
                continue;
            }
            const f32 from = state.initialised ? state.previous_gain : gain;

            if (out.desc.kind == OutputDesc::Kind::Binaural) {
                // Gain-ramped mono through the per-voice binaural filter, added to the headphone bus.
                std::copy_n(mono, frames, w.scaled.data());
                Kernels::scale_ramp(w.scaled.data(), frames, from, gain);
                std::fill(w.tmp_left.begin(), w.tmp_left.end(), 0.0f);
                std::fill(w.tmp_right.begin(), w.tmp_right.end(), 0.0f);
                if (state.binaural) {
                    state.binaural->process(w.scaled.data(), w.tmp_left.data(), w.tmp_right.data(), frames, f.direction, f.distance);
                }
                AudioBuffer &target = bus_target(*bus, w);
                Kernels::add(target.data(0), w.tmp_left.data(), frames);
                Kernels::add(target.data(1), w.tmp_right.data(), frames);
            } else {
                bool as_object = false;
                if (out.desc.kind == OutputDesc::Kind::Objects && out.desc.objects_consumed) {
                    if (state.object_slot < 0) {
                        for (u32 slot = 0; slot < out.slot_count; ++slot) {
                            VoiceId expected = 0;
                            if (out.slot_owner[slot].compare_exchange_strong(expected, v.id, std::memory_order_acq_rel)) {
                                state.object_slot = static_cast<i32>(slot);
                                break;
                            }
                        }
                    }
                    as_object = state.object_slot >= 0;
                }
                if (as_object) {
                    // Objects carry the bus chain's volume themselves (they bypass the bed's bus mix).
                    const f32 chain = bus_chain_gain(*bus);
                    const f32 target = gain * chain;
                    const f32 start = state.initialised ? state.previous_gain : target;
                    const u32 slot = static_cast<u32>(state.object_slot);
                    Kernels::add_ramp(out.objects.samples.data(slot), mono, frames, start, target);
                    out.objects.metadata[slot] = ObjectMetadata{v.id, f.direction * f.distance, f.spread, target};
                    state.previous_gain = target;
                    state.initialised = true;
                    continue;
                }
                if (v.panner == Panner::Ambisonic && out.ambi.channels() > 0) {
                    const u32 order = std::clamp(out.desc.ambisonic_order, 1u, max_ambisonic_order);
                    encode_ambisonic(f.direction, order, w.ambi_scratch.data());
                    AudioBuffer &ambi = ambi_target(bus->output, w);
                    for (u32 acn = 0; acn < ambisonic_channels(order); ++acn) {
                        const f32 target = gain * w.ambi_scratch[acn];
                        const f32 start = state.initialised ? state.previous_gains[acn] : target;
                        Kernels::add_ramp(ambi.data(acn), mono, frames, start, target);
                        state.previous_gains[acn] = target;
                    }
                } else {
                    out.vbap.gains(f.direction, f.spread, w.gain_scratch);
                    const u32 layout_channels = out.desc.layout.channel_count();
                    AudioBuffer &target_buffer = bus_target(*bus, w);
                    for (u32 c = 0; c < layout_channels && c < target_buffer.channels(); ++c) {
                        const f32 target = gain * w.gain_scratch[c];
                        const f32 start = state.initialised ? state.previous_gains[c] : target;
                        if (target != 0.0f || start != 0.0f) {
                            Kernels::add_ramp(target_buffer.data(c), mono, frames, start, target);
                        }
                        state.previous_gains[c] = target;
                    }
                }
            }
            state.previous_gain = gain;
            state.initialised = true;
        }

        // Aux send (reverb): pre-pan, into channel 0 of the aux bus (the reverb sums its channels).
        if (v.aux_bus != no_bus && buses[v.aux_bus]) {
            f32 send = v.aux_send;
            if (world_spatial && v.use_acoustics) {
                send = std::max(send, v.acoustics.reverb_send * v.aux_send);
            }
            const f32 level = f.gain * send * std::sqrt(std::max(0.0f, f.distance > 0.0f ? distance_gain(v.distance, f.distance) : 1.0f));
            const f32 start = v.aux_initialised ? v.aux_previous : level;
            Kernels::add_ramp(bus_target(*buses[v.aux_bus], w).data(0), mono, frames, start, level);
            v.aux_previous = level;
            v.aux_initialised = true;
        }
    }

    void AudioEngine::Impl::process_buses() {
        const u32 frames = cfg.block_frames;
        for (Bus *bus : bus_order) {
            OutputState &out = outputs[bus->output];
            const bool master = bus->parent == no_bus;
            if (master && out.ambi_used) {
                bus->has_input = true;
                out.decoder.decode(out.ambi, bus->buffer);
            }
            if (!master) {
                // A bus nothing was mixed into (and whose effects have stopped ringing) is silent: skip it entirely, including
                // its effects and its contribution to its parent.
                if (bus->has_input) {
                    i64 tail = 0;
                    for (const auto &effect : bus->effects) {
                        tail = std::max<i64>(tail, effect->tail_frames());
                    }
                    bus->tail_remaining = tail + frames;
                } else if (bus->tail_remaining > 0) {
                    bus->tail_remaining -= frames;
                } else {
                    bus->peak.store(0.0f, std::memory_order_relaxed);
                    bus->rms.store(0.0f, std::memory_order_relaxed);
                    continue;
                }
            }
            // The master's limiter is its last effect and must run after the volume; run all but it before.
            const usize effect_count = bus->effects.size();
            const usize before_volume = master && effect_count > 0 ? effect_count - 1 : effect_count;
            for (usize e = 0; e < before_volume; ++e) {
                bus->effects[e]->process(bus->buffer);
            }
            const f32 target = bus->muted ? 0.0f : bus->volume * duck_multiplier[bus->index];
            if (bus->applied_volume != target || target != 1.0f) {
                for (u32 c = 0; c < bus->buffer.channels(); ++c) {
                    Kernels::scale_ramp(bus->buffer.data(c), frames, bus->applied_volume, target);
                }
                bus->applied_volume = target;
            }
            if (master && effect_count > 0) {
                bus->effects.back()->process(bus->buffer);
            }
            bus->peak.store(bus->buffer.peak(), std::memory_order_relaxed);
            bus->rms.store(bus->buffer.rms(), std::memory_order_relaxed);
            if (!master && buses[bus->parent]) {
                buses[bus->parent]->buffer.add(bus->buffer);
                buses[bus->parent]->has_input = true;
            }
        }
    }

    void AudioEngine::Impl::retire_finished() {
        for (usize i = 0; i < voices.size();) {
            Voice &v = *voices[i];
            const bool done = v.ending || (v.stopping && v.fade <= 0.0f) || v.source->finished();
            if (!done) {
                ++i;
                continue;
            }
            if (!v.finish_reported) {
                const f64 native = static_cast<f64>(std::max(v.source->native_rate(), 1u));
                const f64 position = static_cast<f64>(v.source->position_frames()) / native;
                v.status->publish(position, VoiceStatus::State::Finished, v.volume, v.pitch);
                workers[0]->events.push_back(VoiceEvent{v.id, VoiceEvent::Kind::Finished, 0, position});
                v.finish_reported = true;
            }
            if (!retired.try_push(Retired{voices[i].get(), RetiredKind::Voice})) {
                ++i; // the retire ring is full; try again next block
                continue;
            }
            for (usize k = 0; k < v.buses.size(); ++k) {
                if (v.states[k].object_slot >= 0 && buses[v.buses[k]]) {
                    outputs[buses[v.buses[k]]->output].slot_owner[static_cast<usize>(v.states[k].object_slot)].store(0, std::memory_order_release);
                }
            }
            if (v.ambisonic && ambisonic_voices > 0) {
                --ambisonic_voices;
            }
            by_slot[v.slot] = nullptr;
            (void)finished.try_push(v.id);
            voices[i].release(); // now owned by the retire ring
            voices[i] = std::move(voices.back());
            voices.pop_back();
        }
    }

    // ---- one block ----------------------------------------------------------------------------------------------------

    void AudioEngine::Impl::mix_entry(void *context, u32 participant) {
        auto *self = static_cast<Impl *>(context);
        Worker &worker = *self->workers[participant];
        constexpr u32 kGrain = 8;
        const u32 count = static_cast<u32>(self->physical_list.size());
        for (;;) {
            const u32 begin = self->cursor.fetch_add(kGrain, std::memory_order_relaxed);
            if (begin >= count) {
                break;
            }
            const u32 end = std::min(count, begin + kGrain);
            for (u32 i = begin; i < end; ++i) {
                self->render_voice(*self->physical_list[i], worker);
            }
        }
    }

    void AudioEngine::Impl::prepare_entry(void *context, u32) {
        auto *self = static_cast<Impl *>(context);
        constexpr u32 kGrain = 64;
        const u32 count = static_cast<u32>(self->voices.size());
        for (;;) {
            const u32 begin = self->cursor.fetch_add(kGrain, std::memory_order_relaxed);
            if (begin >= count) {
                break;
            }
            for (u32 i = begin; i < std::min(count, begin + kGrain); ++i) {
                self->prepare_voice(*self->voices[i]);
            }
        }
    }

    void AudioEngine::Impl::mix_voices() {
        cursor.store(0, std::memory_order_relaxed);
        if (physical_list.size() >= cfg.parallel_voice_threshold && pool->participants() > 1) {
            pool->run(&Impl::mix_entry, this);
        } else {
            mix_entry(this, 0);
        }
    }

    // Folds what the helper threads accumulated into the real buses and ambisonic beds, and forwards their events.
    void AudioEngine::Impl::merge_workers() {
        for (usize w = 1; w < workers.size(); ++w) {
            Worker &worker = *workers[w];
            for (Bus *bus : bus_order) {
                if (worker.bus_dirty[bus->index] != 0) {
                    AudioBuffer &partial = bus->partial[w - 1];
                    bus->buffer.add(partial);
                    partial.clear();
                    bus->has_input = true;
                    worker.bus_dirty[bus->index] = 0;
                }
            }
            for (OutputId o = 0; o < outputs.size(); ++o) {
                if (worker.ambi_dirty[o] != 0) {
                    OutputState &out = outputs[o];
                    out.ambi.add(out.ambi_partial[w - 1]);
                    out.ambi_partial[w - 1].clear();
                    out.ambi_used = true;
                    worker.ambi_dirty[o] = 0;
                }
            }
        }
        for (auto &worker : workers) {
            for (const VoiceEvent &e : worker->events) {
                (void)events.try_push(e);
            }
            worker->events.clear();
        }
    }

    void AudioEngine::render_block() {
        Impl &m = *impl_;
        // Denormals decay out of filters and reverb tails; flushing them keeps the block time flat. (Priority is the device
        // callback's business: offline rendering runs here too.)
        prepare_realtime_thread(RealtimeOptions{.flush_denormals = true, .raise_priority = false});
        const u32 frames = m.cfg.block_frames;
        m.apply_commands();
        m.inverse_listener_rotation = glm::inverse(m.listener.rotation);
        if (m.ambisonic_voices > 0 && m.listener.rotation != m.rotator_rotation) {
            m.rotator_rotation = m.listener.rotation;
            m.rotator.set_rotation(m.inverse_listener_rotation);
        }

        // Ducking: how far each bus is pulled down this block, from the trigger buses' level last block.
        m.duck_multiplier.fill(1.0f);
        for (DuckRule &rule : m.duck_rules) {
            const f32 rms = m.buses[rule.trigger] ? m.buses[rule.trigger]->rms.load(std::memory_order_relaxed) : 0.0f;
            const f32 target = rms > rule.threshold ? rule.floor_gain : 1.0f;
            rule.current += (target - rule.current) * (target < rule.current ? rule.attack : rule.release);
            m.duck_multiplier[rule.target] *= rule.current;
        }

        for (Bus *bus : m.bus_order) {
            bus->buffer.clear();
            bus->has_input = false;
        }
        for (OutputState &out : m.outputs) {
            out.ambi_used = false;
            if (out.ambi.channels() > 0) {
                out.ambi.clear();
            }
            if (out.desc.kind == OutputDesc::Kind::Objects) {
                out.objects.samples.clear();
                for (ObjectMetadata &meta : out.objects.metadata) {
                    meta = ObjectMetadata{};
                }
            }
        }

        // Per-voice gain, direction and fade maths, spread over the mixing threads when there are many voices.
        m.cursor.store(0, std::memory_order_relaxed);
        if (m.voices.size() >= std::max<usize>(m.cfg.parallel_voice_threshold * 4, 256) && m.pool->participants() > 1) {
            m.pool->run(&Impl::prepare_entry, &m);
        } else {
            Impl::prepare_entry(&m, 0);
        }
        // Rank voices by audibility; the top N are physical, the rest advance silently.
        m.ranking.clear();
        for (u32 i = 0; i < m.voices.size(); ++i) {
            m.voices[i]->physical = false;
            m.ranking.emplace_back(m.voices[i]->frame.score, i);
        }
        const usize wanted = std::min<usize>(m.cfg.max_physical_voices, m.ranking.size());
        if (wanted < m.ranking.size()) {
            std::nth_element(m.ranking.begin(), m.ranking.begin() + static_cast<std::ptrdiff_t>(wanted), m.ranking.end(),
                             [](const auto &a, const auto &b) { return a.first > b.first; });
        }
        m.physical_list.clear();
        for (usize r = 0; r < wanted; ++r) {
            Voice &v = *m.voices[m.ranking[r].second];
            // Paused voices do not mix; voices too quiet to hear are not worth the work either.
            if (!v.paused && (m.ranking[r].first >= kInaudible || v.fade_in_remaining > 0 || v.delay_frames > 0 || v.start_fade_seconds > 0.0f)) {
                v.physical = true;
                m.physical_list.push_back(&v);
            }
        }
        u32 virtualised = 0;
        for (auto &voice : m.voices) {
            Voice &v = *voice;
            if (!v.paused && !v.physical) {
                ++virtualised;
                if (v.delay_frames > 0) {
                    v.delay_frames -= std::min<u64>(v.delay_frames, frames);
                } else {
                    (void)v.source->skip(frames);
                }
            }
        }

        m.compute_begin();
        m.mix_voices();
        m.compute_end();
        m.merge_workers();
        for (auto &voice : m.voices) {
            Voice &v = *voice;
            const VoiceStatus::State state = v.paused ? VoiceStatus::State::Paused
                                             : v.delay_frames > 0 ? VoiceStatus::State::Pending
                                             : (v.physical ? VoiceStatus::State::Playing : VoiceStatus::State::Virtual);
            v.status->publish(static_cast<f64>(v.source->position_frames()) / static_cast<f64>(std::max(v.source->native_rate(), 1u)), state, v.volume, v.pitch);
        }

        m.process_buses();

        // Recent output for visualisers (mono mix of each output's master bed).
        for (OutputId o = 0; o < m.outputs.size(); ++o) {
            OutputState &out = m.outputs[o];
            const AudioBuffer &bed = m.buses[o]->buffer;
            u32 write = out.tap_write->load(std::memory_order_relaxed);
            const f32 inv = bed.channels() > 0 ? 1.0f / static_cast<f32>(bed.channels()) : 0.0f;
            for (u32 i = 0; i < frames; ++i) {
                f32 sum = 0.0f;
                for (u32 c = 0; c < bed.channels(); ++c) {
                    sum += bed.data(c)[i];
                }
                out.tap[(write + i) & (kTapSize - 1)] = sum * inv;
            }
            out.tap_write->store(write + frames, std::memory_order_release);
        }

        for (OutputState &out : m.outputs) {
            if (out.desc.kind == OutputDesc::Kind::Objects) {
                u32 active = 0;
                for (const ObjectMetadata &meta : out.objects.metadata) {
                    active += meta.voice != 0 ? 1 : 0;
                }
                out.objects.active = active;
            }
        }
        m.stat_voices.store(static_cast<u32>(m.voices.size()), std::memory_order_relaxed);
        m.stat_physical.store(static_cast<u32>(m.physical_list.size()), std::memory_order_relaxed);
        m.stat_virtual.store(virtualised, std::memory_order_relaxed);
        m.stat_peak_bits.store(std::bit_cast<u32>(m.buses[0]->buffer.peak()), std::memory_order_relaxed);

        m.clock_local += frames;
        m.clock_frames.store(m.clock_local, std::memory_order_relaxed);
        m.retire_finished();
        // Finished voices raised their events in `retire_finished`; hand those over too.
        for (const VoiceEvent &e : m.workers[0]->events) {
            (void)m.events.try_push(e);
        }
        m.workers[0]->events.clear();
    }

    u32 AudioEngine::take_output(OutputId output, f32 *interleaved, u32 max_frames) {
        Impl &m = *impl_;
        if (output == primary_output || output >= m.outputs.size() || interleaved == nullptr) {
            return 0;
        }
        OutputState &out = m.outputs[output];
        const u32 channels = out.desc.layout.channel_count();
        const usize wanted = static_cast<usize>(max_frames) * channels;
        usize sample = 0;
        while (sample < wanted) {
            if (out.fifo_carry_position >= out.fifo_carry.size()) {
                out.fifo_carry.clear();
                out.fifo_carry_position = 0;
                if (out.fifo->drain_into(out.fifo_carry) == 0) {
                    break;
                }
            }
            const usize take = std::min(out.fifo_carry.size() - out.fifo_carry_position, wanted - sample);
            std::copy_n(out.fifo_carry.begin() + static_cast<std::ptrdiff_t>(out.fifo_carry_position), take, interleaved + sample);
            out.fifo_carry_position += take;
            sample += take;
        }
        return static_cast<u32>(sample / channels); // blocks are queued whole, so this is a whole number of frames
    }

    void AudioEngine::pull(OutputId output, f32 *interleaved, u32 frames) {
        Impl &m = *impl_;
        if (output >= m.outputs.size() || interleaved == nullptr) {
            return;
        }
        OutputState &out = m.outputs[output];
        const u32 channels = out.desc.layout.channel_count();

        if (output != primary_output) {
            // Secondary: whatever the primary's pulls have produced, silence for the rest.
            const u32 got = take_output(output, interleaved, frames);
            std::fill(interleaved + static_cast<usize>(got) * channels, interleaved + static_cast<usize>(frames) * channels, 0.0f);
            return;
        }

        u32 done = 0;
        while (done < frames) {
            if (m.stage_empty || m.stage_position >= m.cfg.block_frames) {
                render_block();
                for (OutputId o = 0; o < m.outputs.size(); ++o) {
                    OutputState &s = m.outputs[o];
                    const AudioBuffer &bed = m.buses[o]->buffer;
                    const u32 ch = s.desc.layout.channel_count();
                    std::array<const f32 *, max_channels> planes{};
                    for (u32 c = 0; c < ch && c < max_channels; ++c) {
                        planes[c] = bed.data(c);
                    }
                    Kernels::interleave(planes.data(), ch, s.staged.data(), m.cfg.block_frames);
                    if (o > 0 && s.fifo && s.fifo->capacity() - s.fifo->size() >= s.staged.size()) {
                        // A whole block or nothing: a partial block would shift every later sample off its channel. A consumer
                        // that has fallen behind loses blocks rather than blocking the clock.
                        for (f32 sample : s.staged) {
                            (void)s.fifo->try_push(sample);
                        }
                    }
                }
                m.stage_position = 0;
                m.stage_empty = false;
            }
            const u32 take = std::min(frames - done, m.cfg.block_frames - m.stage_position);
            std::copy_n(out.staged.begin() + static_cast<std::ptrdiff_t>(static_cast<usize>(m.stage_position) * channels),
                        static_cast<usize>(take) * channels, interleaved + static_cast<usize>(done) * channels);
            m.stage_position += take;
            done += take;
        }
    }

} // namespace SFT::Audio
