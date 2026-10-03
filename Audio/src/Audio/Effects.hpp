#pragma once

#include <Audio/AudioBuffer.hpp>
#include <Audio/Dsp.hpp>
#include <Audio/Source.hpp>

#include <Foundation/Foundation.hpp>

#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

/// Audio effects and filters. Every effect works the same way wherever it runs: on a mixer bus, on a single voice, or
/// offline on a decoded buffer (`apply_effects`), so a low-pass you tune in the editor is the one that ships.
///
/// Effects expose their controls as numbered, named, ranged parameters (`parameters()`), which is what a UI, an automation
/// curve, a save file, or a scripting/FFI layer needs, and the same values can be set by name through `EffectSpec`.
namespace SFT::Audio {

    struct ParameterInfo {
        UString name;
        UString unit;       ///< "Hz", "dB", "ms", "" ...
        f32 minimum = 0.0f;
        f32 maximum = 1.0f;
        f32 default_value = 0.0f;
        /// Meaningful sweeps are logarithmic (frequencies); UIs should map sliders accordingly.
        bool logarithmic = false;
        /// For enumerated parameters, the names of the values 0, 1, 2, ... (empty otherwise).
        std::span<const UString> choices{};
    };

    /// A processing stage. `process` runs in place on `buffer.frames()` frames of every channel, in real time: no
    /// allocation, locks or I/O. Everything that allocates happens in `prepare`, which runs on the game thread before the
    /// effect goes live.
    class AudioEffect {
      public:
        virtual ~AudioEffect() = default;

        virtual void process(AudioBuffer &buffer) = 0;
        /// Sets up for a signal of `channels` channels at `sample_rate`, in blocks of at most `max_frames`.
        virtual void prepare(f32 /*sample_rate*/, u32 /*channels*/, u32 /*max_frames*/) {}
        /// Forgets all history (filter state, delay lines, envelopes).
        virtual void reset() {}
        /// Frames the output lags the input by (look-ahead limiters and gates, convolution at small partitions).
        [[nodiscard]] virtual u32 latency_frames() const { return 0; }
        /// Frames the effect keeps sounding after its input goes silent (delay and reverb tails). Offline processing renders
        /// this many extra frames so tails are not cut off.
        [[nodiscard]] virtual u32 tail_frames() const { return 0; }
        [[nodiscard]] virtual ustr name() const { return "effect"_ustr; }

        [[nodiscard]] virtual std::span<const ParameterInfo> parameters() const { return {}; }
        /// Out-of-range values are clamped; unknown indices are ignored.
        virtual void set_parameter(u32 /*index*/, f32 /*value*/) {}
        [[nodiscard]] virtual f32 parameter(u32 /*index*/) const { return 0.0f; }

        /// Index of a parameter by name, or `parameter_count()` when there is none.
        [[nodiscard]] u32 find_parameter(const ustr &name) const;
        [[nodiscard]] u32 parameter_count() const { return static_cast<u32>(parameters().size()); }
        /// `set_parameter` by name; false when the name is unknown.
        bool set_parameter(const ustr &name, f32 value);
    };

    /// The mixer's historical name for an effect on a bus.
    using BusEffect = AudioEffect;

    // ---- the catalogue ---------------------------------------------------------------------------------------------------

    enum class EffectKind : u8 {
        Filter,       ///< low/high/band-pass, notch, all-pass, peak, shelves; Butterworth slopes up to 48 dB/oct for low/high-pass
        Equalizer,    ///< eight parametric bands
        DcBlocker,    ///< removes DC offset and sub-audible rumble
        NoiseGate,    ///< silences a signal below a threshold (hysteresis, hold, look-ahead, side-chain filter)
        Expander,     ///< gentle downward expansion: a softer gate
        Compressor,   ///< feed-forward, soft knee, peak or RMS detection, look-ahead, parallel mix
        Limiter,      ///< look-ahead brick-wall limiter
        DeEsser,      ///< dynamic cut of harsh sibilance
        HumFilter,    ///< notch comb at 50/60 Hz and harmonics
        Delay,        ///< echo with feedback, damping and ping-pong
        Modulation,   ///< chorus, flanger, phaser, tremolo, vibrato
        Distortion,   ///< soft/hard clip, wavefold, bit crusher
        StereoWidth,  ///< mid/side width and balance of the first two channels
        GainPan,      ///< gain, constant-power pan, polarity, mono sum
        NoiseReduction, ///< spectral noise reduction (see Denoise.hpp for profile learning)
    };

    /// An effect described by data: its kind plus parameter values by name (unlisted ones keep their defaults).
    struct EffectSpec {
        EffectKind kind = EffectKind::Filter;
        std::vector<std::pair<UString, f32>> values;

        EffectSpec() = default;
        explicit EffectSpec(EffectKind k) : kind(k) {}
        EffectSpec(EffectKind k, std::initializer_list<std::pair<UString, f32>> list) : kind(k) {
            for (const auto &[name, value] : list) {
                values.emplace_back(name, value);
            }
        }
        EffectSpec &set(const ustr &name, f32 value) {
            values.emplace_back(name, value);
            return *this;
        }
    };

    /// Builds an effect from a spec (not yet prepared). Fails on an unknown parameter name so typos are loud.
    [[nodiscard]] std::expected<std::unique_ptr<AudioEffect>, UString> make_effect(const EffectSpec &spec);
    /// Builds, prepares and returns an effect ready for `process`.
    [[nodiscard]] std::expected<std::unique_ptr<AudioEffect>, UString> make_effect(const EffectSpec &spec, f32 sample_rate, u32 channels, u32 max_frames);
    /// Display name of a kind ("Noise gate").
    [[nodiscard]] ustr effect_kind_name(EffectKind kind) noexcept;
    /// Identifier form of the name for data files and foreign callers: "noise_reduction", "dc_blocker", "gain_pan".
    [[nodiscard]] std::string_view effect_kind_token(EffectKind kind);
    /// Every kind with its parameters, for building a UI or documentation. Parameter metadata of an instance is the same.
    [[nodiscard]] std::vector<EffectKind> all_effect_kinds();
    [[nodiscard]] std::span<const ParameterInfo> effect_parameters(EffectKind kind);

    // ---- named constructors for the most common cases ---------------------------------------------------------------------------

    [[nodiscard]] std::unique_ptr<AudioEffect> make_low_pass(f32 cutoff_hz, u32 order = 2, f32 q = 0.7071f);
    [[nodiscard]] std::unique_ptr<AudioEffect> make_high_pass(f32 cutoff_hz, u32 order = 2, f32 q = 0.7071f);
    [[nodiscard]] std::unique_ptr<AudioEffect> make_band_pass(f32 center_hz, f32 q = 1.0f);
    [[nodiscard]] std::unique_ptr<AudioEffect> make_noise_gate(f32 threshold_db, f32 attack_ms = 1.0f, f32 hold_ms = 50.0f, f32 release_ms = 150.0f);
    [[nodiscard]] std::unique_ptr<AudioEffect> make_compressor(f32 threshold_db, f32 ratio, f32 attack_ms = 10.0f, f32 release_ms = 100.0f,
                                                              f32 makeup_db = 0.0f);
    [[nodiscard]] std::unique_ptr<AudioEffect> make_limiter(f32 ceiling_db = -0.3f, f32 release_ms = 80.0f, f32 lookahead_ms = 1.5f);
    [[nodiscard]] std::unique_ptr<AudioEffect> make_hum_filter(f32 base_hz = 50.0f, u32 harmonics = 4);

    /// Convolution reverb / cabinet / speaker simulation: partitioned FFT convolution with `impulse` (any channel count and
    /// sample rate; it is resampled to the engine rate on `prepare`). Latency is one partition (the block size).
    [[nodiscard]] std::unique_ptr<AudioEffect> make_convolution(std::shared_ptr<const SampleBuffer> impulse, f32 wet = 1.0f, f32 dry = 0.0f,
                                                                f32 gain_db = 0.0f, f32 predelay_ms = 0.0f);

    // ---- offline use -------------------------------------------------------------------------------------------------------

    /// Runs `effects` in series over `buffer`'s audio (interleaved samples, `block_frames` at a time) and returns the
    /// result. Look-ahead latency is removed so the output lines up with the input, and `include_tail` extends the result by
    /// the longest tail so delay/reverb decay is kept. Effects are prepared here; pass fresh ones (state carries over).
    [[nodiscard]] std::expected<std::shared_ptr<SampleBuffer>, UString> apply_effects(
        const SampleBuffer &buffer, std::span<const std::unique_ptr<AudioEffect>> effects, bool include_tail = true, u32 block_frames = 512);

} // namespace SFT::Audio
