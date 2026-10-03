#pragma once

#include <Audio/Dsp.hpp>
#include <Audio/Source.hpp>

#include <Foundation/Foundation.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace SFT::Audio {

    using PatchNode = u32;
    inline constexpr PatchNode no_patch_node = 0xFFFFFFFFu;

    struct PatchDef; // immutable, shared between instances

    /// Describes a synthesis patch as a graph of nodes evaluated per sample: oscillators, noise, ADSR envelopes, filters,
    /// delays, LFOs and arithmetic, plus named parameters a game drives live (engine RPM, wind speed, a trigger gate).
    /// Every node input is another node, so modulation of anything by anything (FM, ring mod, filter sweeps) falls out of
    /// wiring. Nodes only reference earlier nodes, so patches are acyclic by construction and evaluate in order.
    class PatchBuilder {
      public:
        PatchBuilder();
        ~PatchBuilder();
        PatchBuilder(PatchBuilder &&) noexcept;
        PatchBuilder &operator=(PatchBuilder &&) noexcept;

        PatchNode constant(f32 value);
        /// A named control input, smoothed over ~5 ms; set it with `PatchSource::set_param`.
        PatchNode param(const UString &name, f32 default_value = 0.0f);
        /// Oscillator at `frequency` Hz (a node: constant, parameter, envelope-driven...). `pulse_width` only affects Square.
        PatchNode oscillator(Waveform waveform, PatchNode frequency, PatchNode pulse_width = no_patch_node);
        PatchNode noise(bool pink = false, u32 seed = 1);
        /// ADSR envelope gated by `gate` (on above 0.5). Times in seconds.
        PatchNode envelope(PatchNode gate, f32 attack, f32 decay, f32 sustain, f32 release);
        /// State-variable filter; `cutoff` is a node in Hz (modulate it for sweeps and wah). Output is the chosen band.
        enum class Band : u8 { Low, Band, High };
        PatchNode filter(Band band, PatchNode input, PatchNode cutoff, f32 q = 0.7071f);
        PatchNode add(PatchNode a, PatchNode b);
        PatchNode multiply(PatchNode a, PatchNode b);
        /// `a + (b - a) * t`.
        PatchNode mix(PatchNode a, PatchNode b, PatchNode t);
        /// `input * scale + offset`.
        PatchNode scale(PatchNode input, f32 scale, f32 offset = 0.0f);
        /// Soft clipping (`tanh`) after `drive` gain: warm saturation.
        PatchNode saturate(PatchNode input, f32 drive = 1.0f);
        /// Feedback delay of up to `max_seconds`; `time` is a node in seconds.
        PatchNode delay(PatchNode input, f32 max_seconds, PatchNode time, f32 feedback = 0.0f);
        /// MIDI note number to Hz: `440 * 2^((note - 69) / 12)`.
        PatchNode midi_to_hz(PatchNode note);

        /// Marks the node that is the patch's output and builds the immutable definition.
        [[nodiscard]] std::shared_ptr<const PatchDef> build(PatchNode output);

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    /// One playing instance of a patch (a mono `DataSource`). Parameters may be changed from any thread.
    class PatchSource final : public DataSource {
      public:
        PatchSource(std::shared_ptr<const PatchDef> def, u32 sample_rate);
        ~PatchSource() override;

        /// Sets a named parameter; returns false for an unknown name. Thread-safe and real-time safe.
        bool set_param(const ustr &name, f32 value) noexcept;
        [[nodiscard]] usize param_count() const noexcept;

        /// Ends the source after `seconds` of audio (0 = play forever until the owner stops the voice).
        void set_duration(f32 seconds) noexcept { duration_frames_ = seconds > 0.0f ? static_cast<u64>(seconds * static_cast<f32>(sample_rate_)) : 0; }

        u32 channel_count() const override { return 1; }
        u32 sample_rate() const override { return sample_rate_; }
        u32 read(AudioBuffer &out, u32 frames) override;
        bool finished() const override { return finished_; }

      private:
        struct Runtime;
        std::shared_ptr<const PatchDef> def_;
        std::unique_ptr<Runtime> runtime_;
        u32 sample_rate_;
        u64 duration_frames_ = 0;
        u64 produced_ = 0;
        bool finished_ = false;
    };

} // namespace SFT::Audio
