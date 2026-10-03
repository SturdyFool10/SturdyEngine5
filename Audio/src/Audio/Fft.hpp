#pragma once

#include <Foundation/Foundation.hpp>

#include <span>
#include <vector>

namespace SFT::Audio {

    /// Radix-2 FFT on split real/imaginary arrays with precomputed twiddles (computed in double, stored as float), so the
    /// butterfly loops are contiguous and vectorise. One `Fft` is immutable after construction and may be shared by any
    /// number of threads; callers bring their own scratch memory.
    ///
    /// Used by convolution reverb, spectrograms/analysis, and the phase-vocoder time stretcher.
    class Fft {
      public:
        /// `size` must be a power of two, at least 4 (smaller or other values are rounded up to the next power of two).
        explicit Fft(u32 size);

        [[nodiscard]] u32 size() const noexcept { return size_; }
        /// Scratch floats `forward_real`/`inverse_real` need.
        [[nodiscard]] u32 real_scratch_size() const noexcept { return size_; }

        /// In-place complex transform of `size()` points. `forward` is unscaled; `inverse` divides by `size()`.
        void forward(f32 *re, f32 *im) const noexcept;
        void inverse(f32 *re, f32 *im) const noexcept;

        /// Real input of `size()` samples to `size()/2 + 1` bins (`re`/`im` hold that many). Unscaled.
        /// `scratch` has `real_scratch_size()` floats and may not alias the other arguments.
        void forward_real(const f32 *input, f32 *re, f32 *im, f32 *scratch) const noexcept;
        /// The inverse of `forward_real`: `size()/2 + 1` bins in, `size()` samples out (scaled so the round trip is exact).
        /// `re`/`im` are destroyed.
        void inverse_real(f32 *re, f32 *im, f32 *output, f32 *scratch) const noexcept;

      private:
        void transform(f32 *re, f32 *im, u32 n, bool invert) const noexcept;

        u32 size_;
        std::vector<u32> bit_reverse_;       // for `size_`
        std::vector<f32> twiddle_re_, twiddle_im_;           // per-stage runs for the full-size complex transform
        std::vector<u32> half_bit_reverse_;  // for `size_ / 2` (the real transforms)
        std::vector<f32> half_twiddle_re_, half_twiddle_im_;
        std::vector<f32> real_cos_, real_sin_; // exp(-2 pi i k / N), k = 0..N/2
    };

} // namespace SFT::Audio
