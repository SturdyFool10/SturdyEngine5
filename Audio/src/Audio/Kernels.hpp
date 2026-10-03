#pragma once

#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <span>

namespace SFT::Audio::Kernels {

    /// The inner loops every part of the audio engine shares (mixing, metering, effects, format conversion), compiled once
    /// per instruction set and picked at startup from what the CPU reports: AVX-512F or AVX2+FMA on x86-64, NEON on ARM64 (always
    /// present), the compiler's RVV vectorisation on RISC-V builds that enable the V extension, and plain portable loops
    /// anywhere else. Call these instead of writing a per-sample loop; there is exactly one place to improve them.
    ///
    /// None of them care about alignment, none allocate, and `dst`/`src` must not overlap unless stated.

    /// `dst[i] += src[i]`
    void add(f32 *dst, const f32 *src, usize n) noexcept;
    /// `dst[i] += src[i] * gain`
    void add_gain(f32 *dst, const f32 *src, f32 gain, usize n) noexcept;
    /// `dst[i] += src[i] * g[i]` where `g` slides linearly from `from` (exclusive) to `to` (inclusive) over `n` samples:
    /// the click-free way to change a voice's gain between blocks.
    void add_ramp(f32 *dst, const f32 *src, usize n, f32 from, f32 to) noexcept;
    /// `data[i] *= gain`
    void scale(f32 *data, f32 gain, usize n) noexcept;
    /// `data[i] *= g[i]`, `g` as in `add_ramp`.
    void scale_ramp(f32 *data, usize n, f32 from, f32 to) noexcept;
    /// `dst[i] = src[i] * gain` (dst may equal src).
    void copy_gain(f32 *dst, const f32 *src, f32 gain, usize n) noexcept;
    /// Largest absolute value (0 for n == 0). NaN samples are ignored.
    [[nodiscard]] f32 peak(const f32 *data, usize n) noexcept;
    /// Sum of squares in double precision (partial sums are float lanes; the lanes are combined in double).
    [[nodiscard]] f64 sum_squares(const f32 *data, usize n) noexcept;

    // ---- layout / format conversion (portable loops the compiler vectorises) ----------------------------------------

    /// `planar[c][i]` -> `interleaved[i * channels + c]`
    void interleave(const f32 *const *planar, u32 channels, f32 *interleaved, usize frames) noexcept;
    /// `interleaved[i * channels + c]` -> `planar[c][i]`
    void deinterleave(const f32 *interleaved, u32 channels, f32 *const *planar, usize frames) noexcept;

    /// Float to integer PCM: clamps to [-1, 1] and rounds to nearest. 24-bit goes to the low three bytes of each output
    /// triple (little endian, packed); 32-bit uses the full range.
    void f32_to_i16(const f32 *src, i16 *dst, usize n) noexcept;
    void f32_to_i24(const f32 *src, u8 *dst, usize n) noexcept;
    void f32_to_i32(const f32 *src, i32 *dst, usize n) noexcept;
    void i16_to_f32(const i16 *src, f32 *dst, usize n) noexcept;
    void i24_to_f32(const u8 *src, f32 *dst, usize n) noexcept;
    void i32_to_f32(const i32 *src, f32 *dst, usize n) noexcept;
    void u8_to_f32(const u8 *src, f32 *dst, usize n) noexcept;

    /// Catmull-Rom cubic through `p1` (at t = 0) and `p2` (at t = 1), shaped by the neighbours `p0` and `p3`: the one
    /// interpolator every resampling path (varispeed playback, sample-rate matching, streams) uses.
    [[nodiscard]] constexpr f32 catmull_rom(f32 p0, f32 p1, f32 p2, f32 p3, f32 t) noexcept {
        const f32 a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
        const f32 b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
        const f32 d = -0.5f * p0 + 0.5f * p2;
        return ((a * t + b) * t + d) * t + p1;
    }

    // ---- span overloads --------------------------------------------------------------------------------------------
    // Same operations over spans: the length is the shorter of the operands, so a mismatched pair can never run past the end
    // of either. Prefer these anywhere the operands are already containers or `AudioBuffer::channel()` spans.

    inline void add(std::span<f32> dst, std::span<const f32> src) noexcept { add(dst.data(), src.data(), std::min(dst.size(), src.size())); }
    inline void add_gain(std::span<f32> dst, std::span<const f32> src, f32 gain) noexcept { add_gain(dst.data(), src.data(), gain, std::min(dst.size(), src.size())); }
    inline void add_ramp(std::span<f32> dst, std::span<const f32> src, f32 from, f32 to) noexcept { add_ramp(dst.data(), src.data(), std::min(dst.size(), src.size()), from, to); }
    inline void scale(std::span<f32> data, f32 gain) noexcept { scale(data.data(), gain, data.size()); }
    inline void scale_ramp(std::span<f32> data, f32 from, f32 to) noexcept { scale_ramp(data.data(), data.size(), from, to); }
    inline void copy_gain(std::span<f32> dst, std::span<const f32> src, f32 gain) noexcept { copy_gain(dst.data(), src.data(), gain, std::min(dst.size(), src.size())); }
    [[nodiscard]] inline f32 peak(std::span<const f32> data) noexcept { return peak(data.data(), data.size()); }
    [[nodiscard]] inline f64 sum_squares(std::span<const f32> data) noexcept { return sum_squares(data.data(), data.size()); }

    /// Which implementation family was selected ("avx512f", "avx2+fma", "neon", "rvv", "portable"), for logs and diagnostics.
    [[nodiscard]] const char *active_isa() noexcept;

} // namespace SFT::Audio::Kernels
