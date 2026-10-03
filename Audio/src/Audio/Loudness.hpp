#pragma once

#include <Audio/Channels.hpp>
#include <Audio/Source.hpp>

#include <Foundation/Foundation.hpp>

#include <vector>

namespace SFT::Audio {

    /// ITU-R BS.1770-4 / EBU R128 loudness: K-weighted, gated, in LUFS, for any sample rate and channel layout (surround
    /// channels weigh +1.5 dB, the LFE does not count, unlabelled channels count as front channels).
    struct LoudnessResult {
        /// Gated programme loudness (LUFS); -infinity-ish (-70) when everything was below the absolute gate.
        f64 integrated = -70.0;
        /// Loudest 400 ms and 3 s windows seen.
        f64 momentary_max = -70.0;
        f64 short_term_max = -70.0;
        /// Loudness range (LU), EBU Tech 3342.
        f64 range = 0.0;
        /// Highest sample magnitude in dBFS.
        f64 sample_peak_db = -200.0;
        /// Highest inter-sample (4x oversampled) magnitude in dBFS; filled by `measure_loudness`, not by the streaming meter.
        f64 true_peak_db = -200.0;
    };

    /// Streaming meter: feed interleaved blocks as they arrive (a recording, a live source, a long file), read results any time.
    class LoudnessMeter {
      public:
        LoudnessMeter(u32 channels, u32 sample_rate, const ChannelLayoutInfo &layout = {});

        void process(const f32 *interleaved, usize frames);
        /// Loudness of the most recent 400 ms / 3 s (LUFS).
        [[nodiscard]] f64 momentary() const;
        [[nodiscard]] f64 short_term() const;
        [[nodiscard]] LoudnessResult result() const;
        void reset();

      private:
        struct Biquad2 {
            f64 b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
            f64 z1 = 0, z2 = 0;
            f64 step(f64 x) { const f64 y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
        };

        u32 channels_;
        u32 sample_rate_;
        std::vector<f64> weights_;
        std::vector<Biquad2> shelf_, high_pass_;
        // Mean-square energy per 100 ms step (weighted sum over channels); windows are sums of consecutive steps.
        std::vector<f64> steps_;
        u32 step_frames_;
        u32 step_fill_ = 0;
        f64 step_energy_ = 0.0;
        f64 sample_peak_ = 0.0;
    };

    /// Measures a whole buffer, including true peak.
    [[nodiscard]] LoudnessResult measure_loudness(const SampleBuffer &buffer);

} // namespace SFT::Audio
