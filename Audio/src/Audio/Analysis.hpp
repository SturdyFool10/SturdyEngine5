#pragma once

#include <Audio/Mixer.hpp>

#include <Foundation/Foundation.hpp>

#include <span>
#include <vector>

namespace SFT::Audio {

    /// Hann-windowed FFT magnitudes of `samples` (length a power of two, at least 4): `magnitudes` receives
    /// `samples.size() / 2` linear values scaled so a full-scale sine reads about 1 at its bin.
    void compute_spectrum(std::span<const f32> samples, std::span<f32> magnitudes);

    /// Spectrum and level readouts of what the engine is playing: for visualisers, VU meters and audio-reactive gameplay.
    class SpectrumAnalyzer {
      public:
        explicit SpectrumAnalyzer(u32 fft_size = 2048);

        /// Analyses the most recent `fft_size` samples of an output.
        void analyze(const AudioEngine &engine, OutputId output = primary_output);

        [[nodiscard]] std::span<const f32> magnitudes() const noexcept { return magnitudes_; }
        [[nodiscard]] u32 fft_size() const noexcept { return size_; }
        /// Centre frequency of an FFT bin.
        [[nodiscard]] static f32 bin_frequency(u32 bin, u32 fft_size, u32 sample_rate) noexcept {
            return static_cast<f32>(bin) * static_cast<f32>(sample_rate) / static_cast<f32>(fft_size);
        }
        /// `count` logarithmically spaced bands between `low_hz` and `high_hz`, each 0..1 on a decibel scale
        /// (-80 dB to 0 dB), the shape a spectrum bar display wants.
        [[nodiscard]] std::vector<f32> bands(u32 count, u32 sample_rate, f32 low_hz = 40.0f, f32 high_hz = 16000.0f) const;
        /// Energy-weighted average frequency, a rough "brightness".
        [[nodiscard]] f32 centroid(u32 sample_rate) const;

      private:
        u32 size_;
        std::vector<f32> samples_;
        std::vector<f32> magnitudes_;
    };

} // namespace SFT::Audio
