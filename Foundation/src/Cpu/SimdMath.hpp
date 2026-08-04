#pragma once

#include <Foundation/src/Types.hpp>

namespace SFT::Foundation::Cpu {

    // Element-wise / reduction float math, dispatched at runtime to the widest SIMD tier
    // `best_simd_level()` (CpuId.hpp) says is safe on this process — AVX-512/AVX2+FMA/AVX on x86,
    // NEON on Arm64, RVV on RISC-V, or a plain scalar loop everywhere else — without the caller having
    // to know or branch on which. "As fast as the CPU will let us": exactly the CPU's best tier, no
    // less, and never a tier the OS hasn't enabled register state for (x86) or that isn't actually
    // present (RISC-V's V extension is optional).
    //
    // Dispatch is resolved once, lazily, into a small function-pointer table on first call (see
    // SimdMath.cpp) — every call after that is one indirect call plus the loop itself, not a per-call
    // feature check.

    void add(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept;               // dst[i] = a[i] + b[i]
    void add(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept;
    void mul(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept;               // dst[i] = a[i] * b[i]
    void mul(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept;
    void fma(f32 *dst, const f32 *a, const f32 *b, const f32 *c, usize n) noexcept; // dst[i] = a[i]*b[i] + c[i]
    void fma(f64 *dst, const f64 *a, const f64 *b, const f64 *c, usize n) noexcept;
    [[nodiscard]] f32 dot(const f32 *a, const f32 *b, usize n) noexcept;            // sum(a[i] * b[i])
    [[nodiscard]] f64 dot(const f64 *a, const f64 *b, usize n) noexcept;
    void sqrt(f32 *dst, const f32 *a, usize n) noexcept;                            // dst[i] = sqrt(a[i])
    void sqrt(f64 *dst, const f64 *a, usize n) noexcept;

} // namespace SFT::Foundation::Cpu
