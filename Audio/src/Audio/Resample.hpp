#pragma once

#include <Audio/Source.hpp>

#include <Foundation/Foundation.hpp>

#include <memory>
#include <span>
#include <vector>

namespace SFT::Audio {

    /// Windowed-sinc (Kaiser) sample-rate conversion. Offline conversion, live capture/network feeds, and drift correction
    /// all use this one implementation.
    enum class ResampleQuality : u8 {
        Fast,    ///< 8 taps per side, about -60 dB alias rejection: voices, previews
        Medium,  ///< 16 taps, about -80 dB
        High,    ///< 32 taps, about -100 dB: the default for converting assets
        Best,    ///< 64 taps, about -120 dB: mastering
    };

    /// A streaming converter for interleaved audio. Feed it blocks of any size; it keeps the filter history between calls,
    /// so splitting a stream into blocks gives exactly the output of converting it whole. The ratio can be nudged while
    /// running (`set_ratio_adjustment`), which is how network and video playback absorb clock drift without clicks.
    class Resampler {
      public:
        Resampler(u32 channels, u32 input_rate, u32 output_rate, ResampleQuality quality = ResampleQuality::High);

        /// Converts `input` (interleaved, `input_frames` frames) and appends the result to `output` (interleaved).
        /// Returns the number of frames appended.
        usize process(const f32 *input, usize input_frames, std::vector<f32> &output);
        /// Appends the last frames that were waiting for look-ahead (feeds silence). Call once at the end of a stream.
        usize flush(std::vector<f32> &output);
        void reset();

        /// Multiplies the nominal output rate by `adjustment` (1.0 = nominal; 1.0005 = produce 0.05% more output per input).
        /// Keep it within a few percent: the filter is designed for the nominal ratio.
        void set_ratio_adjustment(f64 adjustment) noexcept { adjustment_ = adjustment > 0.0 ? adjustment : 1.0; }

        [[nodiscard]] u32 channels() const noexcept { return channels_; }
        /// Frames of delay between input and output (half the filter length, in input frames).
        [[nodiscard]] f64 latency_frames() const noexcept { return static_cast<f64>(half_width_); }
        /// Output frames `input_frames` of input produce (approximate; exact over a long stream).
        [[nodiscard]] f64 expected_output(usize input_frames) const noexcept { return static_cast<f64>(input_frames) / step_ / adjustment_; }

      private:
        void trim();

        u32 channels_;
        f64 step_;            // input frames per output frame at nominal ratio
        f64 adjustment_ = 1.0;
        u32 half_width_;      // filter half length in input frames (already scaled for downsampling)
        // Exact polyphase mode: when the two rates reduce to a small fraction (44100 -> 48000 is 147:160) every output lands on one of
        // `den_` fixed phases, so the kernel for each phase is computed once and no interpolation between table entries is needed.
        // Tracking the position as an integer plus a phase also removes the drift floating-point accumulation would add.
        u32 num_ = 1, den_ = 1;
        bool exact_ = false;
        std::vector<f32> phase_table_; // den_ x (2 * half_width_)
        u64 exact_base_ = 0;
        u32 exact_phase_ = 0;
        bool exact_active_ = false;
        u32 table_resolution_;
        std::vector<f32> table_; // kernel samples, `table_resolution_` per input frame, from -half_width_ to +half_width_
        std::vector<f32> history_; // interleaved input still needed, starting at frame `base`
        f64 position_ = 0.0;      // next output position, in frames relative to history_[0]
    };

    /// Converts a whole interleaved signal. Returns the input unchanged (copied) when the rates match.
    [[nodiscard]] std::vector<f32> resample(std::span<const f32> interleaved, u32 channels, u32 input_rate, u32 output_rate,
                                            ResampleQuality quality = ResampleQuality::High);

    /// A `SampleBuffer` at another rate (markers and loop points are rescaled to the new frame positions).
    [[nodiscard]] std::shared_ptr<SampleBuffer> resample(const SampleBuffer &buffer, u32 output_rate, ResampleQuality quality = ResampleQuality::High);

} // namespace SFT::Audio
