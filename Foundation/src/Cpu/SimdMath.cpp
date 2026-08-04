#include <Foundation/src/Cpu/SimdMath.hpp>
#include <Foundation/src/Cpu/CpuId.hpp>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #define STURDY_CPU_X86 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define STURDY_CPU_ARM64 1
#elif defined(__riscv) && (__riscv_xlen == 64)
    #define STURDY_CPU_RISCV64 1
#endif

#if defined(STURDY_CPU_X86)
    #include <immintrin.h>
#elif defined(STURDY_CPU_ARM64)
    #include <arm_neon.h>
#elif defined(STURDY_CPU_RISCV64)
    #include <riscv_vector.h>
#endif

#include <cmath>

namespace SFT::Foundation::Cpu {

    namespace {

        // --- scalar (always available, every architecture) -------------------------------------

        void add_scalar(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept {
            for (usize i = 0; i < n; ++i)
                dst[i] = a[i] + b[i];
        }
        void add_scalar(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept {
            for (usize i = 0; i < n; ++i)
                dst[i] = a[i] + b[i];
        }
        void mul_scalar(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept {
            for (usize i = 0; i < n; ++i)
                dst[i] = a[i] * b[i];
        }
        void mul_scalar(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept {
            for (usize i = 0; i < n; ++i)
                dst[i] = a[i] * b[i];
        }
        void fma_scalar(f32 *dst, const f32 *a, const f32 *b, const f32 *c, usize n) noexcept {
            for (usize i = 0; i < n; ++i)
                dst[i] = a[i] * b[i] + c[i];
        }
        void fma_scalar(f64 *dst, const f64 *a, const f64 *b, const f64 *c, usize n) noexcept {
            for (usize i = 0; i < n; ++i)
                dst[i] = a[i] * b[i] + c[i];
        }
        f32 dot_scalar(const f32 *a, const f32 *b, usize n) noexcept {
            f32 sum = 0.0f;
            for (usize i = 0; i < n; ++i)
                sum += a[i] * b[i];
            return sum;
        }
        f64 dot_scalar(const f64 *a, const f64 *b, usize n) noexcept {
            f64 sum = 0.0;
            for (usize i = 0; i < n; ++i)
                sum += a[i] * b[i];
            return sum;
        }
        void sqrt_scalar(f32 *dst, const f32 *a, usize n) noexcept {
            for (usize i = 0; i < n; ++i)
                dst[i] = std::sqrt(a[i]);
        }
        void sqrt_scalar(f64 *dst, const f64 *a, usize n) noexcept {
            for (usize i = 0; i < n; ++i)
                dst[i] = std::sqrt(a[i]);
        }

#if defined(STURDY_CPU_X86)

        // --- AVX (256-bit, no guaranteed hardware FMA -- mul+add stays two instructions) -------

        [[gnu::target("avx")]] void add_avx(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept {
            usize i = 0;
            for (; i + 8 <= n; i += 8)
                _mm256_storeu_ps(dst + i, _mm256_add_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
            for (; i < n; ++i)
                dst[i] = a[i] + b[i];
        }
        [[gnu::target("avx")]] void add_avx(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept {
            usize i = 0;
            for (; i + 4 <= n; i += 4)
                _mm256_storeu_pd(dst + i, _mm256_add_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i)));
            for (; i < n; ++i)
                dst[i] = a[i] + b[i];
        }
        [[gnu::target("avx")]] void mul_avx(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept {
            usize i = 0;
            for (; i + 8 <= n; i += 8)
                _mm256_storeu_ps(dst + i, _mm256_mul_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
            for (; i < n; ++i)
                dst[i] = a[i] * b[i];
        }
        [[gnu::target("avx")]] void mul_avx(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept {
            usize i = 0;
            for (; i + 4 <= n; i += 4)
                _mm256_storeu_pd(dst + i, _mm256_mul_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i)));
            for (; i < n; ++i)
                dst[i] = a[i] * b[i];
        }
        [[gnu::target("avx")]] void fma_avx(f32 *dst, const f32 *a, const f32 *b, const f32 *c, usize n) noexcept {
            usize i = 0;
            for (; i + 8 <= n; i += 8) {
                const __m256 product = _mm256_mul_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
                _mm256_storeu_ps(dst + i, _mm256_add_ps(product, _mm256_loadu_ps(c + i)));
            }
            for (; i < n; ++i)
                dst[i] = a[i] * b[i] + c[i];
        }
        [[gnu::target("avx")]] void fma_avx(f64 *dst, const f64 *a, const f64 *b, const f64 *c, usize n) noexcept {
            usize i = 0;
            for (; i + 4 <= n; i += 4) {
                const __m256d product = _mm256_mul_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i));
                _mm256_storeu_pd(dst + i, _mm256_add_pd(product, _mm256_loadu_pd(c + i)));
            }
            for (; i < n; ++i)
                dst[i] = a[i] * b[i] + c[i];
        }
        [[gnu::target("avx")]] f32 dot_avx(const f32 *a, const f32 *b, usize n) noexcept {
            usize i = 0;
            __m256 acc = _mm256_setzero_ps();
            for (; i + 8 <= n; i += 8)
                acc = _mm256_add_ps(acc, _mm256_mul_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
            const __m128 sum128a = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
            const __m128 sum128b = _mm_hadd_ps(sum128a, sum128a);
            const __m128 sum128c = _mm_hadd_ps(sum128b, sum128b);
            f32 sum = _mm_cvtss_f32(sum128c);
            for (; i < n; ++i)
                sum += a[i] * b[i];
            return sum;
        }
        [[gnu::target("avx")]] f64 dot_avx(const f64 *a, const f64 *b, usize n) noexcept {
            usize i = 0;
            __m256d acc = _mm256_setzero_pd();
            for (; i + 4 <= n; i += 4)
                acc = _mm256_add_pd(acc, _mm256_mul_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i)));
            const __m128d sum128a = _mm_add_pd(_mm256_castpd256_pd128(acc), _mm256_extractf128_pd(acc, 1));
            const __m128d sum128b = _mm_hadd_pd(sum128a, sum128a);
            f64 sum = _mm_cvtsd_f64(sum128b);
            for (; i < n; ++i)
                sum += a[i] * b[i];
            return sum;
        }
        [[gnu::target("avx")]] void sqrt_avx(f32 *dst, const f32 *a, usize n) noexcept {
            usize i = 0;
            for (; i + 8 <= n; i += 8)
                _mm256_storeu_ps(dst + i, _mm256_sqrt_ps(_mm256_loadu_ps(a + i)));
            for (; i < n; ++i)
                dst[i] = std::sqrt(a[i]);
        }
        [[gnu::target("avx")]] void sqrt_avx(f64 *dst, const f64 *a, usize n) noexcept {
            usize i = 0;
            for (; i + 4 <= n; i += 4)
                _mm256_storeu_pd(dst + i, _mm256_sqrt_pd(_mm256_loadu_pd(a + i)));
            for (; i < n; ++i)
                dst[i] = std::sqrt(a[i]);
        }

        // --- AVX2 + FMA3 (256-bit, hardware fused multiply-add) --------------------------------
        // add/mul/sqrt gain nothing from AVX2 over AVX (no new float instructions for them), so those
        // tiers just reuse the AVX bodies; only fma/dot benefit from the hardware FMA instruction.

        [[gnu::target("avx2,fma")]] void add_avx2(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept { add_avx(dst, a, b, n); }
        [[gnu::target("avx2,fma")]] void add_avx2(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept { add_avx(dst, a, b, n); }
        [[gnu::target("avx2,fma")]] void mul_avx2(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept { mul_avx(dst, a, b, n); }
        [[gnu::target("avx2,fma")]] void mul_avx2(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept { mul_avx(dst, a, b, n); }
        [[gnu::target("avx2,fma")]] void sqrt_avx2(f32 *dst, const f32 *a, usize n) noexcept { sqrt_avx(dst, a, n); }
        [[gnu::target("avx2,fma")]] void sqrt_avx2(f64 *dst, const f64 *a, usize n) noexcept { sqrt_avx(dst, a, n); }

        [[gnu::target("avx2,fma")]] void fma_avx2(f32 *dst, const f32 *a, const f32 *b, const f32 *c, usize n) noexcept {
            usize i = 0;
            for (; i + 8 <= n; i += 8)
                _mm256_storeu_ps(dst + i, _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), _mm256_loadu_ps(c + i)));
            for (; i < n; ++i)
                dst[i] = a[i] * b[i] + c[i];
        }
        [[gnu::target("avx2,fma")]] void fma_avx2(f64 *dst, const f64 *a, const f64 *b, const f64 *c, usize n) noexcept {
            usize i = 0;
            for (; i + 4 <= n; i += 4)
                _mm256_storeu_pd(dst + i, _mm256_fmadd_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i), _mm256_loadu_pd(c + i)));
            for (; i < n; ++i)
                dst[i] = a[i] * b[i] + c[i];
        }
        [[gnu::target("avx2,fma")]] f32 dot_avx2(const f32 *a, const f32 *b, usize n) noexcept {
            usize i = 0;
            __m256 acc = _mm256_setzero_ps();
            for (; i + 8 <= n; i += 8)
                acc = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), acc);
            const __m128 sum128a = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
            const __m128 sum128b = _mm_hadd_ps(sum128a, sum128a);
            const __m128 sum128c = _mm_hadd_ps(sum128b, sum128b);
            f32 sum = _mm_cvtss_f32(sum128c);
            for (; i < n; ++i)
                sum += a[i] * b[i];
            return sum;
        }
        [[gnu::target("avx2,fma")]] f64 dot_avx2(const f64 *a, const f64 *b, usize n) noexcept {
            usize i = 0;
            __m256d acc = _mm256_setzero_pd();
            for (; i + 4 <= n; i += 4)
                acc = _mm256_fmadd_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i), acc);
            const __m128d sum128a = _mm_add_pd(_mm256_castpd256_pd128(acc), _mm256_extractf128_pd(acc, 1));
            const __m128d sum128b = _mm_hadd_pd(sum128a, sum128a);
            f64 sum = _mm_cvtsd_f64(sum128b);
            for (; i < n; ++i)
                sum += a[i] * b[i];
            return sum;
        }

        // --- AVX-512F (512-bit, hardware fused multiply-add, no separate FMA bit needed) -------

        [[gnu::target("avx512f")]] void add_avx512(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept {
            usize i = 0;
            for (; i + 16 <= n; i += 16)
                _mm512_storeu_ps(dst + i, _mm512_add_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i)));
            for (; i < n; ++i)
                dst[i] = a[i] + b[i];
        }
        [[gnu::target("avx512f")]] void add_avx512(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept {
            usize i = 0;
            for (; i + 8 <= n; i += 8)
                _mm512_storeu_pd(dst + i, _mm512_add_pd(_mm512_loadu_pd(a + i), _mm512_loadu_pd(b + i)));
            for (; i < n; ++i)
                dst[i] = a[i] + b[i];
        }
        [[gnu::target("avx512f")]] void mul_avx512(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept {
            usize i = 0;
            for (; i + 16 <= n; i += 16)
                _mm512_storeu_ps(dst + i, _mm512_mul_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i)));
            for (; i < n; ++i)
                dst[i] = a[i] * b[i];
        }
        [[gnu::target("avx512f")]] void mul_avx512(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept {
            usize i = 0;
            for (; i + 8 <= n; i += 8)
                _mm512_storeu_pd(dst + i, _mm512_mul_pd(_mm512_loadu_pd(a + i), _mm512_loadu_pd(b + i)));
            for (; i < n; ++i)
                dst[i] = a[i] * b[i];
        }
        [[gnu::target("avx512f")]] void fma_avx512(f32 *dst, const f32 *a, const f32 *b, const f32 *c, usize n) noexcept {
            usize i = 0;
            for (; i + 16 <= n; i += 16)
                _mm512_storeu_ps(dst + i, _mm512_fmadd_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i), _mm512_loadu_ps(c + i)));
            for (; i < n; ++i)
                dst[i] = a[i] * b[i] + c[i];
        }
        [[gnu::target("avx512f")]] void fma_avx512(f64 *dst, const f64 *a, const f64 *b, const f64 *c, usize n) noexcept {
            usize i = 0;
            for (; i + 8 <= n; i += 8)
                _mm512_storeu_pd(dst + i, _mm512_fmadd_pd(_mm512_loadu_pd(a + i), _mm512_loadu_pd(b + i), _mm512_loadu_pd(c + i)));
            for (; i < n; ++i)
                dst[i] = a[i] * b[i] + c[i];
        }
        [[gnu::target("avx512f")]] f32 dot_avx512(const f32 *a, const f32 *b, usize n) noexcept {
            usize i = 0;
            __m512 acc = _mm512_setzero_ps();
            for (; i + 16 <= n; i += 16)
                acc = _mm512_fmadd_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i), acc);
            f32 sum = _mm512_reduce_add_ps(acc);
            for (; i < n; ++i)
                sum += a[i] * b[i];
            return sum;
        }
        [[gnu::target("avx512f")]] f64 dot_avx512(const f64 *a, const f64 *b, usize n) noexcept {
            usize i = 0;
            __m512d acc = _mm512_setzero_pd();
            for (; i + 8 <= n; i += 8)
                acc = _mm512_fmadd_pd(_mm512_loadu_pd(a + i), _mm512_loadu_pd(b + i), acc);
            f64 sum = _mm512_reduce_add_pd(acc);
            for (; i < n; ++i)
                sum += a[i] * b[i];
            return sum;
        }
        [[gnu::target("avx512f")]] void sqrt_avx512(f32 *dst, const f32 *a, usize n) noexcept {
            usize i = 0;
            for (; i + 16 <= n; i += 16)
                _mm512_storeu_ps(dst + i, _mm512_sqrt_ps(_mm512_loadu_ps(a + i)));
            for (; i < n; ++i)
                dst[i] = std::sqrt(a[i]);
        }
        [[gnu::target("avx512f")]] void sqrt_avx512(f64 *dst, const f64 *a, usize n) noexcept {
            usize i = 0;
            for (; i + 8 <= n; i += 8)
                _mm512_storeu_pd(dst + i, _mm512_sqrt_pd(_mm512_loadu_pd(a + i)));
            for (; i < n; ++i)
                dst[i] = std::sqrt(a[i]);
        }

#elif defined(STURDY_CPU_ARM64)

        // --- NEON (128-bit, mandatory AArch64 baseline -- no detection, always safe to call) ----

        void add_neon(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept {
            usize i = 0;
            for (; i + 4 <= n; i += 4)
                vst1q_f32(dst + i, vaddq_f32(vld1q_f32(a + i), vld1q_f32(b + i)));
            for (; i < n; ++i)
                dst[i] = a[i] + b[i];
        }
        void add_neon(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept {
            usize i = 0;
            for (; i + 2 <= n; i += 2)
                vst1q_f64(dst + i, vaddq_f64(vld1q_f64(a + i), vld1q_f64(b + i)));
            for (; i < n; ++i)
                dst[i] = a[i] + b[i];
        }
        void mul_neon(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept {
            usize i = 0;
            for (; i + 4 <= n; i += 4)
                vst1q_f32(dst + i, vmulq_f32(vld1q_f32(a + i), vld1q_f32(b + i)));
            for (; i < n; ++i)
                dst[i] = a[i] * b[i];
        }
        void mul_neon(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept {
            usize i = 0;
            for (; i + 2 <= n; i += 2)
                vst1q_f64(dst + i, vmulq_f64(vld1q_f64(a + i), vld1q_f64(b + i)));
            for (; i < n; ++i)
                dst[i] = a[i] * b[i];
        }
        void fma_neon(f32 *dst, const f32 *a, const f32 *b, const f32 *c, usize n) noexcept {
            usize i = 0;
            for (; i + 4 <= n; i += 4)
                vst1q_f32(dst + i, vfmaq_f32(vld1q_f32(c + i), vld1q_f32(a + i), vld1q_f32(b + i)));
            for (; i < n; ++i)
                dst[i] = a[i] * b[i] + c[i];
        }
        void fma_neon(f64 *dst, const f64 *a, const f64 *b, const f64 *c, usize n) noexcept {
            usize i = 0;
            for (; i + 2 <= n; i += 2)
                vst1q_f64(dst + i, vfmaq_f64(vld1q_f64(c + i), vld1q_f64(a + i), vld1q_f64(b + i)));
            for (; i < n; ++i)
                dst[i] = a[i] * b[i] + c[i];
        }
        f32 dot_neon(const f32 *a, const f32 *b, usize n) noexcept {
            usize i = 0;
            float32x4_t acc = vdupq_n_f32(0.0f);
            for (; i + 4 <= n; i += 4)
                acc = vfmaq_f32(acc, vld1q_f32(a + i), vld1q_f32(b + i));
            f32 sum = vaddvq_f32(acc);
            for (; i < n; ++i)
                sum += a[i] * b[i];
            return sum;
        }
        f64 dot_neon(const f64 *a, const f64 *b, usize n) noexcept {
            usize i = 0;
            float64x2_t acc = vdupq_n_f64(0.0);
            for (; i + 2 <= n; i += 2)
                acc = vfmaq_f64(acc, vld1q_f64(a + i), vld1q_f64(b + i));
            f64 sum = vaddvq_f64(acc);
            for (; i < n; ++i)
                sum += a[i] * b[i];
            return sum;
        }
        void sqrt_neon(f32 *dst, const f32 *a, usize n) noexcept {
            usize i = 0;
            for (; i + 4 <= n; i += 4)
                vst1q_f32(dst + i, vsqrtq_f32(vld1q_f32(a + i)));
            for (; i < n; ++i)
                dst[i] = std::sqrt(a[i]);
        }
        void sqrt_neon(f64 *dst, const f64 *a, usize n) noexcept {
            usize i = 0;
            for (; i + 2 <= n; i += 2)
                vst1q_f64(dst + i, vsqrtq_f64(vld1q_f64(a + i)));
            for (; i < n; ++i)
                dst[i] = std::sqrt(a[i]);
        }

#elif defined(STURDY_CPU_RISCV64)

        // --- RVV (scalable vector length, gated on Cpu::features().rvv -- see CpuId.cpp) --------
        // Unverified in this environment: no riscv64 sysroot/hardware was available to build or run
        // this against (see the header/detection comment in CpuId.cpp for the same caveat).

        void add_rvv(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept {
            for (usize i = 0; i < n;) {
                const usize vl = __riscv_vsetvl_e32m1(n - i);
                __riscv_vse32_v_f32m1(dst + i, __riscv_vfadd_vv_f32m1(__riscv_vle32_v_f32m1(a + i, vl), __riscv_vle32_v_f32m1(b + i, vl), vl), vl);
                i += vl;
            }
        }
        void add_rvv(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept {
            for (usize i = 0; i < n;) {
                const usize vl = __riscv_vsetvl_e64m1(n - i);
                __riscv_vse64_v_f64m1(dst + i, __riscv_vfadd_vv_f64m1(__riscv_vle64_v_f64m1(a + i, vl), __riscv_vle64_v_f64m1(b + i, vl), vl), vl);
                i += vl;
            }
        }
        void mul_rvv(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept {
            for (usize i = 0; i < n;) {
                const usize vl = __riscv_vsetvl_e32m1(n - i);
                __riscv_vse32_v_f32m1(dst + i, __riscv_vfmul_vv_f32m1(__riscv_vle32_v_f32m1(a + i, vl), __riscv_vle32_v_f32m1(b + i, vl), vl), vl);
                i += vl;
            }
        }
        void mul_rvv(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept {
            for (usize i = 0; i < n;) {
                const usize vl = __riscv_vsetvl_e64m1(n - i);
                __riscv_vse64_v_f64m1(dst + i, __riscv_vfmul_vv_f64m1(__riscv_vle64_v_f64m1(a + i, vl), __riscv_vle64_v_f64m1(b + i, vl), vl), vl);
                i += vl;
            }
        }
        void fma_rvv(f32 *dst, const f32 *a, const f32 *b, const f32 *c, usize n) noexcept {
            for (usize i = 0; i < n;) {
                const usize vl = __riscv_vsetvl_e32m1(n - i);
                const vfloat32m1_t va = __riscv_vle32_v_f32m1(a + i, vl);
                const vfloat32m1_t vb = __riscv_vle32_v_f32m1(b + i, vl);
                const vfloat32m1_t vc = __riscv_vle32_v_f32m1(c + i, vl);
                __riscv_vse32_v_f32m1(dst + i, __riscv_vfmacc_vv_f32m1(vc, va, vb, vl), vl);
                i += vl;
            }
        }
        void fma_rvv(f64 *dst, const f64 *a, const f64 *b, const f64 *c, usize n) noexcept {
            for (usize i = 0; i < n;) {
                const usize vl = __riscv_vsetvl_e64m1(n - i);
                const vfloat64m1_t va = __riscv_vle64_v_f64m1(a + i, vl);
                const vfloat64m1_t vb = __riscv_vle64_v_f64m1(b + i, vl);
                const vfloat64m1_t vc = __riscv_vle64_v_f64m1(c + i, vl);
                __riscv_vse64_v_f64m1(dst + i, __riscv_vfmacc_vv_f64m1(vc, va, vb, vl), vl);
                i += vl;
            }
        }
        f32 dot_rvv(const f32 *a, const f32 *b, usize n) noexcept {
            vfloat32m1_t acc = __riscv_vfmv_v_f_f32m1(0.0f, __riscv_vsetvlmax_e32m1());
            for (usize i = 0; i < n;) {
                const usize vl = __riscv_vsetvl_e32m1(n - i);
                acc = __riscv_vfmacc_vv_f32m1(acc, __riscv_vle32_v_f32m1(a + i, vl), __riscv_vle32_v_f32m1(b + i, vl), vl);
                i += vl;
            }
            const vfloat32m1_t zero = __riscv_vfmv_v_f_f32m1(0.0f, 1);
            const vfloat32m1_t sum = __riscv_vfredusum_vs_f32m1_f32m1(acc, zero, __riscv_vsetvlmax_e32m1());
            return __riscv_vfmv_f_s_f32m1_f32(sum);
        }
        f64 dot_rvv(const f64 *a, const f64 *b, usize n) noexcept {
            vfloat64m1_t acc = __riscv_vfmv_v_f_f64m1(0.0, __riscv_vsetvlmax_e64m1());
            for (usize i = 0; i < n;) {
                const usize vl = __riscv_vsetvl_e64m1(n - i);
                acc = __riscv_vfmacc_vv_f64m1(acc, __riscv_vle64_v_f64m1(a + i, vl), __riscv_vle64_v_f64m1(b + i, vl), vl);
                i += vl;
            }
            const vfloat64m1_t zero = __riscv_vfmv_v_f_f64m1(0.0, 1);
            const vfloat64m1_t sum = __riscv_vfredusum_vs_f64m1_f64m1(acc, zero, __riscv_vsetvlmax_e64m1());
            return __riscv_vfmv_f_s_f64m1_f64(sum);
        }
        void sqrt_rvv(f32 *dst, const f32 *a, usize n) noexcept {
            for (usize i = 0; i < n;) {
                const usize vl = __riscv_vsetvl_e32m1(n - i);
                __riscv_vse32_v_f32m1(dst + i, __riscv_vfsqrt_v_f32m1(__riscv_vle32_v_f32m1(a + i, vl), vl), vl);
                i += vl;
            }
        }
        void sqrt_rvv(f64 *dst, const f64 *a, usize n) noexcept {
            for (usize i = 0; i < n;) {
                const usize vl = __riscv_vsetvl_e64m1(n - i);
                __riscv_vse64_v_f64m1(dst + i, __riscv_vfsqrt_v_f64m1(__riscv_vle64_v_f64m1(a + i, vl), vl), vl);
                i += vl;
            }
        }

#endif

        using AddFn32 = void (*)(f32 *, const f32 *, const f32 *, usize);
        using AddFn64 = void (*)(f64 *, const f64 *, const f64 *, usize);
        using MulFn32 = void (*)(f32 *, const f32 *, const f32 *, usize);
        using MulFn64 = void (*)(f64 *, const f64 *, const f64 *, usize);
        using FmaFn32 = void (*)(f32 *, const f32 *, const f32 *, const f32 *, usize);
        using FmaFn64 = void (*)(f64 *, const f64 *, const f64 *, const f64 *, usize);
        using DotFn32 = f32 (*)(const f32 *, const f32 *, usize);
        using DotFn64 = f64 (*)(const f64 *, const f64 *, usize);
        using SqrtFn32 = void (*)(f32 *, const f32 *, usize);
        using SqrtFn64 = void (*)(f64 *, const f64 *, usize);

        struct DispatchTable {
            AddFn32 add32 = add_scalar;
            AddFn64 add64 = add_scalar;
            MulFn32 mul32 = mul_scalar;
            MulFn64 mul64 = mul_scalar;
            FmaFn32 fma32 = fma_scalar;
            FmaFn64 fma64 = fma_scalar;
            DotFn32 dot32 = dot_scalar;
            DotFn64 dot64 = dot_scalar;
            SqrtFn32 sqrt32 = sqrt_scalar;
            SqrtFn64 sqrt64 = sqrt_scalar;
        };

        [[nodiscard]] DispatchTable build_dispatch_table() noexcept {
#if defined(STURDY_CPU_X86)
            switch (best_simd_level()) {
                case SimdLevel::AVX512:
                    return DispatchTable{
                        .add32 = add_avx512, .add64 = add_avx512, .mul32 = mul_avx512, .mul64 = mul_avx512,
                        .fma32 = fma_avx512, .fma64 = fma_avx512, .dot32 = dot_avx512, .dot64 = dot_avx512,
                        .sqrt32 = sqrt_avx512, .sqrt64 = sqrt_avx512};
                case SimdLevel::AVX2:
                    return DispatchTable{
                        .add32 = add_avx2, .add64 = add_avx2, .mul32 = mul_avx2, .mul64 = mul_avx2,
                        .fma32 = fma_avx2, .fma64 = fma_avx2, .dot32 = dot_avx2, .dot64 = dot_avx2,
                        .sqrt32 = sqrt_avx2, .sqrt64 = sqrt_avx2};
                case SimdLevel::AVX:
                    return DispatchTable{
                        .add32 = add_avx, .add64 = add_avx, .mul32 = mul_avx, .mul64 = mul_avx,
                        .fma32 = fma_avx, .fma64 = fma_avx, .dot32 = dot_avx, .dot64 = dot_avx,
                        .sqrt32 = sqrt_avx, .sqrt64 = sqrt_avx};
                case SimdLevel::Scalar:
                case SimdLevel::NEON:
                case SimdLevel::RVV:
                    break;
            }
#elif defined(STURDY_CPU_ARM64)
            if (best_simd_level() == SimdLevel::NEON) {
                return DispatchTable{
                    .add32 = add_neon, .add64 = add_neon, .mul32 = mul_neon, .mul64 = mul_neon,
                    .fma32 = fma_neon, .fma64 = fma_neon, .dot32 = dot_neon, .dot64 = dot_neon,
                    .sqrt32 = sqrt_neon, .sqrt64 = sqrt_neon};
            }
#elif defined(STURDY_CPU_RISCV64)
            if (best_simd_level() == SimdLevel::RVV) {
                return DispatchTable{
                    .add32 = add_rvv, .add64 = add_rvv, .mul32 = mul_rvv, .mul64 = mul_rvv,
                    .fma32 = fma_rvv, .fma64 = fma_rvv, .dot32 = dot_rvv, .dot64 = dot_rvv,
                    .sqrt32 = sqrt_rvv, .sqrt64 = sqrt_rvv};
            }
#endif
            return DispatchTable{};
        }

        [[nodiscard]] const DispatchTable &dispatch_table() noexcept {
            static const DispatchTable instance = build_dispatch_table();
            return instance;
        }

    } // namespace

    void add(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept { dispatch_table().add32(dst, a, b, n); }
    void add(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept { dispatch_table().add64(dst, a, b, n); }
    void mul(f32 *dst, const f32 *a, const f32 *b, usize n) noexcept { dispatch_table().mul32(dst, a, b, n); }
    void mul(f64 *dst, const f64 *a, const f64 *b, usize n) noexcept { dispatch_table().mul64(dst, a, b, n); }
    void fma(f32 *dst, const f32 *a, const f32 *b, const f32 *c, usize n) noexcept { dispatch_table().fma32(dst, a, b, c, n); }
    void fma(f64 *dst, const f64 *a, const f64 *b, const f64 *c, usize n) noexcept { dispatch_table().fma64(dst, a, b, c, n); }
    f32 dot(const f32 *a, const f32 *b, usize n) noexcept { return dispatch_table().dot32(a, b, n); }
    f64 dot(const f64 *a, const f64 *b, usize n) noexcept { return dispatch_table().dot64(a, b, n); }
    void sqrt(f32 *dst, const f32 *a, usize n) noexcept { dispatch_table().sqrt32(dst, a, n); }
    void sqrt(f64 *dst, const f64 *a, usize n) noexcept { dispatch_table().sqrt64(dst, a, n); }

} // namespace SFT::Foundation::Cpu
