#pragma once

#include <Audio/AudioBuffer.hpp>

#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <vector>

/// The node library behind procedural sound, effects and contact audio. Everything is sample-rate aware, allocation
/// free once constructed, and deterministic (noise is seeded), so patches replay identically on any machine.
namespace SFT::Audio {

    // ---- filters ---------------------------------------------------------------------------------------------

    enum class FilterType : u8 { LowPass, HighPass, BandPass, Notch, AllPass, Peak, LowShelf, HighShelf };

    /// Normalised biquad coefficients (a0 == 1), as the RBJ cookbook designs them.
    struct BiquadCoefficients {
        f32 b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    };
    /// `gain_db` only matters for Peak and the shelves. `q` is resonance (0.707 = Butterworth).
    [[nodiscard]] BiquadCoefficients design_biquad(FilterType type, f32 sample_rate, f32 frequency, f32 q = 0.7071f, f32 gain_db = 0.0f) noexcept;

    /// RBJ-cookbook biquad (transposed direct form II).
    class Biquad {
      public:
        void set(FilterType type, f32 sample_rate, f32 frequency, f32 q = 0.7071f, f32 gain_db = 0.0f) noexcept;
        void set(const BiquadCoefficients &c) noexcept { b0_ = c.b0; b1_ = c.b1; b2_ = c.b2; a1_ = c.a1; a2_ = c.a2; }
        [[nodiscard]] f32 process(f32 x) noexcept {
            const f32 y = b0_ * x + z1_;
            z1_ = b1_ * x - a1_ * y + z2_;
            z2_ = b2_ * x - a2_ * y;
            return y;
        }
        void process(std::span<f32> samples) noexcept;
        void reset() noexcept { z1_ = z2_ = 0.0f; }
        /// Coefficients and state, for executors that run the filter elsewhere (the compute backend).
        [[nodiscard]] BiquadCoefficients coefficients() const noexcept { return BiquadCoefficients{b0_, b1_, b2_, a1_, a2_}; }
        [[nodiscard]] std::pair<f32, f32> state() const noexcept { return {z1_, z2_}; }
        void set_state(f32 z1, f32 z2) noexcept { z1_ = z1; z2_ = z2; }
        /// Magnitude response at `frequency` (for tests and EQ displays).
        [[nodiscard]] f32 magnitude_at(f32 frequency, f32 sample_rate) const noexcept;

      private:
        f32 b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
        f32 z1_ = 0.0f, z2_ = 0.0f;
    };

    /// Zavalishin topology-preserving state-variable filter: stable under fast modulation, three outputs at once.
    class StateVariableFilter {
      public:
        struct Output {
            f32 low, band, high;
        };
        void set(f32 sample_rate, f32 cutoff, f32 q = 0.7071f) noexcept;
        [[nodiscard]] Output process(f32 x) noexcept;
        void reset() noexcept { ic1_ = ic2_ = 0.0f; }

      private:
        f32 a1_ = 0.0f, a2_ = 0.0f, a3_ = 0.0f, k_ = 1.0f;
        f32 ic1_ = 0.0f, ic2_ = 0.0f;
    };

    // ---- sources ---------------------------------------------------------------------------------------------

    enum class Waveform : u8 { Sine, Saw, Square, Triangle };

    /// Band-limited oscillator (PolyBLEP saw and square).
    class Oscillator {
      public:
        void set(Waveform waveform, f32 sample_rate, f32 frequency, f32 pulse_width = 0.5f) noexcept;
        void set_frequency(f32 frequency) noexcept;
        void reset(f32 phase = 0.0f) noexcept { phase_ = phase; }
        [[nodiscard]] f32 next() noexcept;
        [[nodiscard]] f32 phase() const noexcept { return phase_; }

      private:
        Waveform waveform_ = Waveform::Sine;
        f32 sample_rate_ = 48000.0f;
        f32 phase_ = 0.0f;
        f32 increment_ = 0.0f;
        f32 pulse_width_ = 0.5f;
    };

    /// Seeded white and pink noise.
    class Noise {
      public:
        explicit Noise(u32 seed = 0x12345678u) noexcept : state_(seed != 0 ? seed : 1u) {}
        [[nodiscard]] f32 white() noexcept;
        /// Pink (-3 dB/octave) via Paul Kellet's economy filter.
        [[nodiscard]] f32 pink() noexcept;

      private:
        u32 state_;
        f32 b_[7]{};
    };

    // ---- control ---------------------------------------------------------------------------------------------

    /// ADSR envelope with exponential decay/release curves and sample-accurate gating.
    class Envelope {
      public:
        enum class Stage : u8 { Idle, Attack, Decay, Sustain, Release };

        void set(f32 sample_rate, f32 attack_seconds, f32 decay_seconds, f32 sustain_level, f32 release_seconds) noexcept;
        void gate_on() noexcept { stage_ = Stage::Attack; }
        void gate_off() noexcept {
            if (stage_ != Stage::Idle) {
                stage_ = Stage::Release;
            }
        }
        [[nodiscard]] f32 next() noexcept;
        [[nodiscard]] Stage stage() const noexcept { return stage_; }
        [[nodiscard]] bool active() const noexcept { return stage_ != Stage::Idle; }
        [[nodiscard]] f32 level() const noexcept { return level_; }

      private:
        Stage stage_ = Stage::Idle;
        f32 level_ = 0.0f;
        f32 sustain_ = 0.7f;
        f32 attack_coefficient_ = 0.01f, decay_coefficient_ = 0.001f, release_coefficient_ = 0.001f;
    };

    // ---- delay-based -----------------------------------------------------------------------------------------

    /// Circular delay line with linear-interpolated fractional reads.
    class DelayLine {
      public:
        DelayLine() = default;
        explicit DelayLine(u32 max_samples) { resize(max_samples); }
        void resize(u32 max_samples);
        void clear() noexcept;
        /// The sample written `delay` calls ago (`delay` >= 1, fractional allowed, clamped to the line length).
        [[nodiscard]] f32 read(f32 delay) const noexcept;
        void write(f32 x) noexcept {
            buffer_[write_index_] = x;
            write_index_ = (write_index_ + 1) & mask_;
        }
        [[nodiscard]] u32 capacity() const noexcept { return static_cast<u32>(buffer_.size()); }

      private:
        std::vector<f32> buffer_;
        u32 mask_ = 0;
        u32 write_index_ = 0;
    };

    /// Eight-line feedback delay network with a Hadamard mixing matrix and per-line damping: a dense, cheap reverb
    /// for aux buses (never per voice).
    class FdnReverb {
      public:
        static constexpr u32 lines = 8;

        /// `rt60` seconds to decay 60 dB, `size` scales the delay lengths (1 = a mid-sized room), `damping` in [0, 1)
        /// is high-frequency absorption in the loop.
        void set(f32 sample_rate, f32 rt60, f32 size = 1.0f, f32 damping = 0.3f) noexcept;
        /// Renders `frames` of wet stereo from a mono input; `out_*` are overwritten.
        void process(const f32 *mono_in, f32 *out_left, f32 *out_right, u32 frames) noexcept;
        void clear() noexcept;
        [[nodiscard]] f32 rt60() const noexcept { return rt60_; }

      private:
        std::array<DelayLine, lines> delays_;
        std::array<f32, lines> lengths_{};
        std::array<f32, lines> gains_{};
        std::array<f32, lines> damp_state_{};
        f32 sample_rate_ = 48000.0f;
        f32 rt60_ = 1.5f;
        f32 damping_ = 0.3f;
    };

    /// Karplus-Strong plucked string.
    class KarplusStrong {
      public:
        /// Excites the string at `frequency`; `decay` in (0, 1) per round trip (closer to 1 rings longer),
        /// `brightness` in [0, 1] is how much of the initial noise burst is kept unfiltered.
        void pluck(f32 sample_rate, f32 frequency, f32 decay = 0.996f, f32 brightness = 0.5f, u32 seed = 1) noexcept;
        [[nodiscard]] f32 next() noexcept;
        [[nodiscard]] bool active() const noexcept { return length_ > 0; }

      private:
        std::vector<f32> line_;
        u32 length_ = 0;
        u32 position_ = 0;
        f32 decay_ = 0.996f;
        f32 previous_ = 0.0f;
    };

    // ---- physical -----------------------------------------------------------------------------------------------

    struct Mode {
        f32 frequency = 440.0f;
        /// Seconds for the mode to decay 60 dB.
        f32 decay_seconds = 0.3f;
        f32 gain = 1.0f;
    };

    /// A bank of damped resonators struck by impulses: the core of contact sounds (tin cans, glass, wood, metal).
    class ModalBank {
      public:
        void set(f32 sample_rate, std::span<const Mode> modes);
        /// Strikes every mode with `strength` (the next sample carries the impulse).
        void excite(f32 strength) noexcept { pending_ += strength; }
        [[nodiscard]] f32 next() noexcept;
        /// True when nothing is pending and every mode has decayed to inaudible.
        [[nodiscard]] bool silent() const noexcept;
        [[nodiscard]] usize mode_count() const noexcept { return modes_.size(); }

      private:
        struct State {
            f32 a1 = 0.0f, a2 = 0.0f, input_gain = 0.0f;
            f32 y1 = 0.0f, y2 = 0.0f;
        };
        std::vector<State> modes_;
        f32 pending_ = 0.0f;
    };

    // ---- dynamics ----------------------------------------------------------------------------------------------

    /// Instant-attack peak limiter with a smooth release: the safety stage at the end of the master bus.
    class Limiter {
      public:
        void set(f32 sample_rate, f32 ceiling = 0.98f, f32 release_seconds = 0.08f) noexcept;
        /// Limits all channels together (so the stereo image does not shift).
        void process(AudioBuffer &buffer) noexcept;
        [[nodiscard]] f32 gain() const noexcept { return gain_; }

      private:
        f32 ceiling_ = 0.98f;
        f32 release_coefficient_ = 0.0005f;
        f32 gain_ = 1.0f;
    };

    /// Feed-forward compressor (peak detector, dB-domain gain computer, hard knee).
    class Compressor {
      public:
        void set(f32 sample_rate, f32 threshold_db, f32 ratio, f32 attack_seconds, f32 release_seconds, f32 makeup_db = 0.0f) noexcept;
        void process(AudioBuffer &buffer) noexcept;

      private:
        f32 threshold_db_ = -18.0f, ratio_ = 4.0f, makeup_ = 1.0f;
        f32 attack_coefficient_ = 0.01f, release_coefficient_ = 0.001f;
        f32 envelope_db_ = -120.0f;
    };

    [[nodiscard]] inline f32 db_to_linear(f32 db) noexcept;
    [[nodiscard]] inline f32 linear_to_db(f32 linear) noexcept;

} // namespace SFT::Audio

#include <cmath>

namespace SFT::Audio {
    inline f32 db_to_linear(f32 db) noexcept { return std::pow(10.0f, db / 20.0f); }
    inline f32 linear_to_db(f32 linear) noexcept { return 20.0f * std::log10(std::max(linear, 1e-9f)); }
} // namespace SFT::Audio
