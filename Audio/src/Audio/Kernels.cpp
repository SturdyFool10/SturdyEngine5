#include <Audio/Kernels.hpp>

#include <Foundation/Cpu/CpuId.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define SFT_AUDIO_X86 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define SFT_AUDIO_ARM64 1
#elif defined(__riscv) && (__riscv_xlen == 64)
#define SFT_AUDIO_RISCV64 1
#endif

#if defined(__clang__)
#define SFT_AUDIO_VECTORIZE _Pragma("clang loop vectorize(enable) interleave(enable)")
#else
#define SFT_AUDIO_VECTORIZE
#endif
#define SFT_ALWAYS_INLINE [[gnu::always_inline]] inline

namespace SFT::Audio::Kernels {

    namespace {

        // One portable body per kernel, written so the compiler's vectoriser handles it at whatever vector width the
        // enclosing function is compiled for. The per-ISA wrappers below only differ in their `target` attribute.
        namespace body {

            SFT_ALWAYS_INLINE void add(f32 *__restrict dst, const f32 *__restrict src, usize n) noexcept {
                SFT_AUDIO_VECTORIZE
                for (usize i = 0; i < n; ++i) {
                    dst[i] += src[i];
                }
            }

            SFT_ALWAYS_INLINE void add_gain(f32 *__restrict dst, const f32 *__restrict src, f32 gain, usize n) noexcept {
                SFT_AUDIO_VECTORIZE
                for (usize i = 0; i < n; ++i) {
                    dst[i] += src[i] * gain;
                }
            }

            SFT_ALWAYS_INLINE void add_ramp(f32 *__restrict dst, const f32 *__restrict src, usize n, f32 from, f32 to) noexcept {
                const f32 step = n > 0 ? (to - from) / static_cast<f32>(n) : 0.0f;
                // g(i) = from + step * (i + 1): written with an index so the loop has no carried dependency.
                SFT_AUDIO_VECTORIZE
                for (usize i = 0; i < n; ++i) {
                    dst[i] += src[i] * (from + step * static_cast<f32>(i + 1));
                }
            }

            SFT_ALWAYS_INLINE void scale(f32 *__restrict data, f32 gain, usize n) noexcept {
                SFT_AUDIO_VECTORIZE
                for (usize i = 0; i < n; ++i) {
                    data[i] *= gain;
                }
            }

            SFT_ALWAYS_INLINE void scale_ramp(f32 *__restrict data, usize n, f32 from, f32 to) noexcept {
                const f32 step = n > 0 ? (to - from) / static_cast<f32>(n) : 0.0f;
                SFT_AUDIO_VECTORIZE
                for (usize i = 0; i < n; ++i) {
                    data[i] *= from + step * static_cast<f32>(i + 1);
                }
            }

            SFT_ALWAYS_INLINE void copy_gain(f32 *dst, const f32 *src, f32 gain, usize n) noexcept {
                SFT_AUDIO_VECTORIZE
                for (usize i = 0; i < n; ++i) {
                    dst[i] = src[i] * gain;
                }
            }

            // Reductions use eight independent accumulators: floating-point addition is not reassociated by the
            // compiler, so the lanes are spelled out and it maps them onto vector registers.
            SFT_ALWAYS_INLINE f32 peak(const f32 *data, usize n) noexcept {
                f32 lane[8] = {};
                usize i = 0;
                for (; i + 8 <= n; i += 8) {
                    for (usize k = 0; k < 8; ++k) {
                        const f32 v = std::fabs(data[i + k]);
                        lane[k] = v > lane[k] ? v : lane[k]; // NaN compares false: ignored
                    }
                }
                f32 result = 0.0f;
                for (f32 v : lane) {
                    result = v > result ? v : result;
                }
                for (; i < n; ++i) {
                    const f32 v = std::fabs(data[i]);
                    result = v > result ? v : result;
                }
                return result;
            }

            SFT_ALWAYS_INLINE f64 sum_squares(const f32 *data, usize n) noexcept {
                f32 lane[8] = {};
                f64 total = 0.0;
                usize i = 0;
                // Chunks keep the float lanes short enough not to lose precision before they are folded into the double.
                while (i + 8 <= n) {
                    const usize chunk_end = std::min(n & ~static_cast<usize>(7), i + 2048);
                    for (; i < chunk_end; i += 8) {
                        for (usize k = 0; k < 8; ++k) {
                            lane[k] += data[i + k] * data[i + k];
                        }
                    }
                    for (f32 &v : lane) {
                        total += static_cast<f64>(v);
                        v = 0.0f;
                    }
                }
                for (; i < n; ++i) {
                    total += static_cast<f64>(data[i]) * static_cast<f64>(data[i]);
                }
                return total;
            }

        } // namespace body

        struct Table {
            void (*add)(f32 *, const f32 *, usize) noexcept = nullptr;
            void (*add_gain)(f32 *, const f32 *, f32, usize) noexcept = nullptr;
            void (*add_ramp)(f32 *, const f32 *, usize, f32, f32) noexcept = nullptr;
            void (*scale)(f32 *, f32, usize) noexcept = nullptr;
            void (*scale_ramp)(f32 *, usize, f32, f32) noexcept = nullptr;
            void (*copy_gain)(f32 *, const f32 *, f32, usize) noexcept = nullptr;
            f32 (*peak)(const f32 *, usize) noexcept = nullptr;
            f64 (*sum_squares)(const f32 *, usize) noexcept = nullptr;
            const char *isa = "portable";
        };

        // ---- baseline: whatever the build's default target vectorises to (SSE2 on x86-64, NEON on ARM64, RVV when enabled)
        void add_base(f32 *dst, const f32 *src, usize n) noexcept { body::add(dst, src, n); }
        void add_gain_base(f32 *dst, const f32 *src, f32 gain, usize n) noexcept { body::add_gain(dst, src, gain, n); }
        void add_ramp_base(f32 *dst, const f32 *src, usize n, f32 from, f32 to) noexcept { body::add_ramp(dst, src, n, from, to); }
        void scale_base(f32 *data, f32 gain, usize n) noexcept { body::scale(data, gain, n); }
        void scale_ramp_base(f32 *data, usize n, f32 from, f32 to) noexcept { body::scale_ramp(data, n, from, to); }
        void copy_gain_base(f32 *dst, const f32 *src, f32 gain, usize n) noexcept { body::copy_gain(dst, src, gain, n); }
        f32 peak_base(const f32 *data, usize n) noexcept { return body::peak(data, n); }
        f64 sum_squares_base(const f32 *data, usize n) noexcept { return body::sum_squares(data, n); }

#if defined(SFT_AUDIO_X86)
        // ---- AVX-512F: compile the shared loops for the widest supported x86 vector width.
#define SFT_AVX512 [[gnu::target("avx512f")]]
        SFT_AVX512 void add_avx512(f32 *dst, const f32 *src, usize n) noexcept { body::add(dst, src, n); }
        SFT_AVX512 void add_gain_avx512(f32 *dst, const f32 *src, f32 gain, usize n) noexcept { body::add_gain(dst, src, gain, n); }
        SFT_AVX512 void add_ramp_avx512(f32 *dst, const f32 *src, usize n, f32 from, f32 to) noexcept { body::add_ramp(dst, src, n, from, to); }
        SFT_AVX512 void scale_avx512(f32 *data, f32 gain, usize n) noexcept { body::scale(data, gain, n); }
        SFT_AVX512 void scale_ramp_avx512(f32 *data, usize n, f32 from, f32 to) noexcept { body::scale_ramp(data, n, from, to); }
        SFT_AVX512 void copy_gain_avx512(f32 *dst, const f32 *src, f32 gain, usize n) noexcept { body::copy_gain(dst, src, gain, n); }
        SFT_AVX512 f32 peak_avx512(const f32 *data, usize n) noexcept { return body::peak(data, n); }
        SFT_AVX512 f64 sum_squares_avx512(const f32 *data, usize n) noexcept { return body::sum_squares(data, n); }
#undef SFT_AVX512

        // ---- AVX2 + FMA: the same bodies, compiled for the wider target
#define SFT_AVX2 [[gnu::target("avx2,fma")]]
        SFT_AVX2 void add_avx2(f32 *dst, const f32 *src, usize n) noexcept { body::add(dst, src, n); }
        SFT_AVX2 void add_gain_avx2(f32 *dst, const f32 *src, f32 gain, usize n) noexcept { body::add_gain(dst, src, gain, n); }
        SFT_AVX2 void add_ramp_avx2(f32 *dst, const f32 *src, usize n, f32 from, f32 to) noexcept { body::add_ramp(dst, src, n, from, to); }
        SFT_AVX2 void scale_avx2(f32 *data, f32 gain, usize n) noexcept { body::scale(data, gain, n); }
        SFT_AVX2 void scale_ramp_avx2(f32 *data, usize n, f32 from, f32 to) noexcept { body::scale_ramp(data, n, from, to); }
        SFT_AVX2 void copy_gain_avx2(f32 *dst, const f32 *src, f32 gain, usize n) noexcept { body::copy_gain(dst, src, gain, n); }
        SFT_AVX2 f32 peak_avx2(const f32 *data, usize n) noexcept { return body::peak(data, n); }
        SFT_AVX2 f64 sum_squares_avx2(const f32 *data, usize n) noexcept { return body::sum_squares(data, n); }
#undef SFT_AVX2
#endif

        Table build_table() noexcept {
            Table table{add_base, add_gain_base, add_ramp_base, scale_base, scale_ramp_base, copy_gain_base, peak_base, sum_squares_base, "portable"};
#if defined(SFT_AUDIO_X86)
            const auto &cpu = Foundation::Cpu::features();
            if (cpu.avx512f && cpu.os_supports_avx512) {
                table = Table{add_avx512, add_gain_avx512, add_ramp_avx512, scale_avx512, scale_ramp_avx512, copy_gain_avx512, peak_avx512, sum_squares_avx512, "avx512f"};
            } else if (cpu.avx2 && cpu.fma3 && cpu.os_supports_avx) {
                table = Table{add_avx2, add_gain_avx2, add_ramp_avx2, scale_avx2, scale_ramp_avx2, copy_gain_avx2, peak_avx2, sum_squares_avx2, "avx2+fma"};
            } else {
                table.isa = "sse2";
            }
#elif defined(SFT_AUDIO_ARM64)
            table.isa = "neon";
#elif defined(SFT_AUDIO_RISCV64) && defined(__riscv_vector)
            table.isa = "rvv";
#endif
            return table;
        }

        const Table &table() noexcept {
            static const Table instance = build_table();
            return instance;
        }

    } // namespace

    void add(f32 *dst, const f32 *src, usize n) noexcept { table().add(dst, src, n); }
    void add_gain(f32 *dst, const f32 *src, f32 gain, usize n) noexcept { table().add_gain(dst, src, gain, n); }
    void add_ramp(f32 *dst, const f32 *src, usize n, f32 from, f32 to) noexcept {
        if (from == to) {
            table().add_gain(dst, src, to, n);
            return;
        }
        table().add_ramp(dst, src, n, from, to);
    }
    void scale(f32 *data, f32 gain, usize n) noexcept { table().scale(data, gain, n); }
    void scale_ramp(f32 *data, usize n, f32 from, f32 to) noexcept {
        if (from == to) {
            table().scale(data, to, n);
            return;
        }
        table().scale_ramp(data, n, from, to);
    }
    void copy_gain(f32 *dst, const f32 *src, f32 gain, usize n) noexcept { table().copy_gain(dst, src, gain, n); }
    f32 peak(const f32 *data, usize n) noexcept { return table().peak(data, n); }
    f64 sum_squares(const f32 *data, usize n) noexcept { return table().sum_squares(data, n); }
    const char *active_isa() noexcept { return table().isa; }

    // ---- conversion -------------------------------------------------------------------------------------------------

    void interleave(const f32 *const *planar, u32 channels, f32 *interleaved, usize frames) noexcept {
        if (channels == 1) {
            std::memcpy(interleaved, planar[0], frames * sizeof(f32));
            return;
        }
        for (u32 c = 0; c < channels; ++c) {
            const f32 *__restrict in = planar[c];
            f32 *__restrict out = interleaved + c;
            for (usize i = 0; i < frames; ++i) {
                out[i * channels] = in[i];
            }
        }
    }

    void deinterleave(const f32 *interleaved, u32 channels, f32 *const *planar, usize frames) noexcept {
        if (channels == 1) {
            std::memcpy(planar[0], interleaved, frames * sizeof(f32));
            return;
        }
        for (u32 c = 0; c < channels; ++c) {
            f32 *__restrict out = planar[c];
            const f32 *__restrict in = interleaved + c;
            for (usize i = 0; i < frames; ++i) {
                out[i] = in[i * channels];
            }
        }
    }

    void f32_to_i16(const f32 *src, i16 *dst, usize n) noexcept {
        SFT_AUDIO_VECTORIZE
        for (usize i = 0; i < n; ++i) {
            const f32 v = std::clamp(src[i], -1.0f, 1.0f) * 32767.0f;
            dst[i] = static_cast<i16>(std::nearbyint(v));
        }
    }

    void f32_to_i24(const f32 *src, u8 *dst, usize n) noexcept {
        for (usize i = 0; i < n; ++i) {
            const i32 v = static_cast<i32>(std::nearbyint(std::clamp(src[i], -1.0f, 1.0f) * 8388607.0f));
            dst[3 * i + 0] = static_cast<u8>(v & 0xFF);
            dst[3 * i + 1] = static_cast<u8>((v >> 8) & 0xFF);
            dst[3 * i + 2] = static_cast<u8>((v >> 16) & 0xFF);
        }
    }

    void f32_to_i32(const f32 *src, i32 *dst, usize n) noexcept {
        for (usize i = 0; i < n; ++i) {
            // Done in double: 2147483647 is not representable in float and would round up past the type's range.
            const f64 v = static_cast<f64>(std::clamp(src[i], -1.0f, 1.0f)) * 2147483647.0;
            dst[i] = static_cast<i32>(std::nearbyint(v));
        }
    }

    void i16_to_f32(const i16 *src, f32 *dst, usize n) noexcept {
        constexpr f32 scale = 1.0f / 32768.0f;
        SFT_AUDIO_VECTORIZE
        for (usize i = 0; i < n; ++i) {
            dst[i] = static_cast<f32>(src[i]) * scale;
        }
    }

    void i24_to_f32(const u8 *src, f32 *dst, usize n) noexcept {
        constexpr f32 scale = 1.0f / 8388608.0f;
        for (usize i = 0; i < n; ++i) {
            // Sign-extend the packed little-endian triple through the top of a 32-bit word.
            const i32 v = static_cast<i32>((static_cast<u32>(src[3 * i]) << 8) | (static_cast<u32>(src[3 * i + 1]) << 16) |
                                           (static_cast<u32>(src[3 * i + 2]) << 24)) >> 8;
            dst[i] = static_cast<f32>(v) * scale;
        }
    }

    void i32_to_f32(const i32 *src, f32 *dst, usize n) noexcept {
        constexpr f64 scale = 1.0 / 2147483648.0;
        for (usize i = 0; i < n; ++i) {
            dst[i] = static_cast<f32>(static_cast<f64>(src[i]) * scale);
        }
    }

    void u8_to_f32(const u8 *src, f32 *dst, usize n) noexcept {
        constexpr f32 scale = 1.0f / 128.0f;
        SFT_AUDIO_VECTORIZE
        for (usize i = 0; i < n; ++i) {
            dst[i] = (static_cast<f32>(src[i]) - 128.0f) * scale;
        }
    }

} // namespace SFT::Audio::Kernels
