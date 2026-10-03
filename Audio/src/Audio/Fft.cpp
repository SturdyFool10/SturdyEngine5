#include <Audio/Fft.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace SFT::Audio {

    namespace {

        u32 round_up_pow2(u32 n) {
            u32 p = 4;
            while (p < n && p < (1u << 30)) {
                p <<= 1;
            }
            return p;
        }

        std::vector<u32> make_bit_reverse(u32 n) {
            std::vector<u32> table(n);
            u32 bits = 0;
            while ((1u << bits) < n) {
                ++bits;
            }
            for (u32 i = 0; i < n; ++i) {
                u32 r = 0;
                for (u32 b = 0; b < bits; ++b) {
                    r |= ((i >> b) & 1u) << (bits - 1 - b);
                }
                table[i] = r;
            }
            return table;
        }

        // Twiddles for every stage, concatenated: stage with half-length h (h = 1, 2, 4, ... n/2) occupies [h - 1, 2h - 1).
        void make_twiddles(u32 n, std::vector<f32> &re, std::vector<f32> &im) {
            re.assign(n, 0.0f);
            im.assign(n, 0.0f);
            for (u32 half = 1; half < n; half <<= 1) {
                for (u32 j = 0; j < half; ++j) {
                    const f64 angle = -std::numbers::pi * static_cast<f64>(j) / static_cast<f64>(half);
                    re[half - 1 + j] = static_cast<f32>(std::cos(angle));
                    im[half - 1 + j] = static_cast<f32>(std::sin(angle));
                }
            }
        }

    } // namespace

    Fft::Fft(u32 size) : size_(round_up_pow2(size)) {
        bit_reverse_ = make_bit_reverse(size_);
        make_twiddles(size_, twiddle_re_, twiddle_im_);
        half_bit_reverse_ = make_bit_reverse(size_ / 2);
        make_twiddles(size_ / 2, half_twiddle_re_, half_twiddle_im_);
        real_cos_.resize(size_ / 2 + 1);
        real_sin_.resize(size_ / 2 + 1);
        for (u32 k = 0; k <= size_ / 2; ++k) {
            const f64 angle = -2.0 * std::numbers::pi * static_cast<f64>(k) / static_cast<f64>(size_);
            real_cos_[k] = static_cast<f32>(std::cos(angle));
            real_sin_[k] = static_cast<f32>(std::sin(angle));
        }
    }

    // In-place transform of `n` points (n == size_ or size_/2) with the matching tables.
    void Fft::transform(f32 *re, f32 *im, u32 n, bool invert) const noexcept {
        const std::vector<u32> &rev = n == size_ ? bit_reverse_ : half_bit_reverse_;
        const std::vector<f32> &wr = n == size_ ? twiddle_re_ : half_twiddle_re_;
        const std::vector<f32> &wi = n == size_ ? twiddle_im_ : half_twiddle_im_;
        for (u32 i = 0; i < n; ++i) {
            const u32 j = rev[i];
            if (i < j) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        // The inverse transform is the forward one with the imaginary twiddle sign flipped.
        const f32 sign = invert ? -1.0f : 1.0f;
        for (u32 half = 1; half < n; half <<= 1) {
            const f32 *__restrict twr = wr.data() + (half - 1);
            const f32 *__restrict twi = wi.data() + (half - 1);
            for (u32 start = 0; start < n; start += 2 * half) {
                f32 *__restrict ar = re + start;
                f32 *__restrict ai = im + start;
                f32 *__restrict br = re + start + half;
                f32 *__restrict bi = im + start + half;
                for (u32 j = 0; j < half; ++j) {
                    const f32 cr = twr[j], ci = sign * twi[j];
                    const f32 tr = br[j] * cr - bi[j] * ci;
                    const f32 ti = br[j] * ci + bi[j] * cr;
                    br[j] = ar[j] - tr;
                    bi[j] = ai[j] - ti;
                    ar[j] += tr;
                    ai[j] += ti;
                }
            }
        }
        if (invert) {
            const f32 scale = 1.0f / static_cast<f32>(n);
            for (u32 i = 0; i < n; ++i) {
                re[i] *= scale;
                im[i] *= scale;
            }
        }
    }

    void Fft::forward(f32 *re, f32 *im) const noexcept { transform(re, im, size_, false); }
    void Fft::inverse(f32 *re, f32 *im) const noexcept { transform(re, im, size_, true); }

    void Fft::forward_real(const f32 *input, f32 *re, f32 *im, f32 *scratch) const noexcept {
        const u32 half = size_ / 2;
        f32 *zr = scratch;
        f32 *zi = scratch + half;
        for (u32 n = 0; n < half; ++n) {
            zr[n] = input[2 * n];
            zi[n] = input[2 * n + 1];
        }
        transform(zr, zi, half, false);
        for (u32 k = 0; k <= half; ++k) {
            const u32 a = k % half;
            const u32 b = (half - k) % half;
            // E = (Z[k] + conj(Z[N/2-k])) / 2, O = (Z[k] - conj(Z[N/2-k])) / (2i); X[k] = E + w^k O
            const f32 er = 0.5f * (zr[a] + zr[b]);
            const f32 ei = 0.5f * (zi[a] - zi[b]);
            const f32 orr = 0.5f * (zi[a] + zi[b]);
            const f32 oi = -0.5f * (zr[a] - zr[b]);
            const f32 c = real_cos_[k], s = real_sin_[k];
            re[k] = er + (orr * c - oi * s);
            im[k] = ei + (orr * s + oi * c);
        }
    }

    void Fft::inverse_real(f32 *re, f32 *im, f32 *output, f32 *scratch) const noexcept {
        const u32 half = size_ / 2;
        f32 *zr = scratch;
        f32 *zi = scratch + half;
        // Rebuild Z[k] = E[k] + i O[k] from the half spectrum: E = (X[k] + conj(X[N/2-k])) / 2,
        // O = (X[k] - conj(X[N/2-k])) * conj(w^k) / 2.
        for (u32 k = 0; k < half; ++k) {
            const u32 m = half - k;
            const f32 er = 0.5f * (re[k] + re[m]);
            const f32 ei = 0.5f * (im[k] - im[m]);
            const f32 dr = 0.5f * (re[k] - re[m]);
            const f32 di = 0.5f * (im[k] + im[m]);
            const f32 c = real_cos_[k], s = -real_sin_[k]; // conj(w^k)
            const f32 orr = dr * c - di * s;
            const f32 oi = dr * s + di * c;
            zr[k] = er - oi;
            zi[k] = ei + orr;
        }
        transform(zr, zi, half, true);
        for (u32 n = 0; n < half; ++n) {
            output[2 * n] = zr[n];
            output[2 * n + 1] = zi[n];
        }
    }

} // namespace SFT::Audio
