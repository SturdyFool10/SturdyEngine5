#include <Foundation/src/Cpu/CpuId.hpp>
#include <Foundation/src/Cpu/CpuTopology.hpp>
#include <Foundation/src/Cpu/SimdMath.hpp>
#include <Foundation/src/Math.hpp>
#include <Foundation/src/Wide.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

namespace {

    template <class T>
    bool nearly_equal(T a, T b, T tolerance) {
        return std::fabs(a - b) <= tolerance * std::max(T(1), std::fabs(a));
    }

    // Reference implementations the dispatched `Cpu::*` functions must agree with, regardless of
    // which SIMD tier actually ran.
    template <class T>
    void add_reference(T *dst, const T *a, const T *b, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i)
            dst[i] = a[i] + b[i];
    }
    template <class T>
    void mul_reference(T *dst, const T *a, const T *b, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i)
            dst[i] = a[i] * b[i];
    }
    template <class T>
    void fma_reference(T *dst, const T *a, const T *b, const T *c, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i)
            dst[i] = a[i] * b[i] + c[i];
    }
    template <class T>
    T dot_reference(const T *a, const T *b, std::size_t n) {
        T sum = T(0);
        for (std::size_t i = 0; i < n; ++i)
            sum += a[i] * b[i];
        return sum;
    }
    template <class T>
    void sqrt_reference(T *dst, const T *a, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i)
            dst[i] = std::sqrt(a[i]);
    }

    // Exercises add/mul/fma/dot/sqrt for one scalar type T (float or double) at array length n,
    // deliberately including non-vector-aligned sizes to hit every tier's scalar tail path.
    template <class T>
    bool test_simd_ops(std::size_t n, T tolerance) {
        std::vector<T> a(n), b(n), c(n);
        std::mt19937 rng(1234u + static_cast<unsigned>(n) + static_cast<unsigned>(sizeof(T)));
        std::uniform_real_distribution<T> dist(T(-10), T(10));
        for (std::size_t i = 0; i < n; ++i) {
            a[i] = dist(rng);
            b[i] = dist(rng);
            c[i] = std::abs(dist(rng)) + T(0.001); // sqrt operand: keep non-negative
        }

        std::vector<T> dst(n), expected(n);

        SFT::Foundation::Cpu::add(dst.data(), a.data(), b.data(), n);
        add_reference(expected.data(), a.data(), b.data(), n);
        for (std::size_t i = 0; i < n; ++i) {
            if (!nearly_equal(dst[i], expected[i], tolerance)) {
                std::cerr << "add mismatch at n=" << n << " i=" << i << ": " << dst[i] << " vs " << expected[i] << "\n";
                return false;
            }
        }

        SFT::Foundation::Cpu::mul(dst.data(), a.data(), b.data(), n);
        mul_reference(expected.data(), a.data(), b.data(), n);
        for (std::size_t i = 0; i < n; ++i) {
            if (!nearly_equal(dst[i], expected[i], tolerance)) {
                std::cerr << "mul mismatch at n=" << n << " i=" << i << ": " << dst[i] << " vs " << expected[i] << "\n";
                return false;
            }
        }

        SFT::Foundation::Cpu::fma(dst.data(), a.data(), b.data(), c.data(), n);
        fma_reference(expected.data(), a.data(), b.data(), c.data(), n);
        for (std::size_t i = 0; i < n; ++i) {
            if (!nearly_equal(dst[i], expected[i], tolerance * T(10))) {
                std::cerr << "fma mismatch at n=" << n << " i=" << i << ": " << dst[i] << " vs " << expected[i] << "\n";
                return false;
            }
        }

        const T dot_result = SFT::Foundation::Cpu::dot(a.data(), b.data(), n);
        const T dot_expected = dot_reference(a.data(), b.data(), n);
        if (!nearly_equal(dot_result, dot_expected, tolerance * T(100))) {
            std::cerr << "dot mismatch at n=" << n << ": " << dot_result << " vs " << dot_expected << "\n";
            return false;
        }

        SFT::Foundation::Cpu::sqrt(dst.data(), c.data(), n);
        sqrt_reference(expected.data(), c.data(), n);
        for (std::size_t i = 0; i < n; ++i) {
            if (!nearly_equal(dst[i], expected[i], tolerance)) {
                std::cerr << "sqrt mismatch at n=" << n << " i=" << i << ": " << dst[i] << " vs " << expected[i] << "\n";
                return false;
            }
        }

        return true;
    }

    // array_add/array_mul (Math.hpp) must exactly match looping the wide type's own scalar operators.
    template <class T>
    bool test_wide_array_ops(std::size_t n) {
        std::vector<T> a(n), b(n), dst(n), expected(n);
        std::mt19937 rng(99u + static_cast<unsigned>(n));
        std::uniform_real_distribution<double> dist(-10.0, 10.0);
        for (std::size_t i = 0; i < n; ++i) {
            a[i] = T(dist(rng));
            b[i] = T(dist(rng));
        }

        SFT::Foundation::array_add(dst.data(), a.data(), b.data(), n);
        for (std::size_t i = 0; i < n; ++i) {
            expected[i] = a[i] + b[i];
            if (!(dst[i] == expected[i])) {
                std::cerr << "array_add mismatch at n=" << n << " i=" << i << "\n";
                return false;
            }
        }

        SFT::Foundation::array_mul(dst.data(), a.data(), b.data(), n);
        for (std::size_t i = 0; i < n; ++i) {
            expected[i] = a[i] * b[i];
            if (!(dst[i] == expected[i])) {
                std::cerr << "array_mul mismatch at n=" << n << " i=" << i << "\n";
                return false;
            }
        }

        return true;
    }

} // namespace

int main() {
    using namespace SFT::Foundation::Cpu;

    // features() must be internally consistent regardless of what this CPU actually has: SSE2 is part
    // of the x86-64 baseline ABI, so any x86-64 build must report it. best_simd_level() must never
    // claim a tier the feature bits (and OS XSAVE check) don't back up.
    const CpuFeatures &f = features();
#if defined(__x86_64__) || defined(_M_X64)
    if (!f.sse2) {
        std::cerr << "sse2 not reported on an x86-64 build (baseline ABI requirement)\n";
        return 1;
    }
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    if (!f.neon) {
        std::cerr << "neon not reported on an Arm64 build (mandatory AArch64 baseline)\n";
        return 1;
    }
#endif
    if (f.avx512f && !f.os_supports_avx512 && best_simd_level() == SimdLevel::AVX512) {
        std::cerr << "best_simd_level() claimed AVX512 without OS XSAVE support\n";
        return 1;
    }
    if (f.avx && !f.os_supports_avx && best_simd_level() != SimdLevel::Scalar) {
        std::cerr << "best_simd_level() claimed a SIMD tier without OS AVX support\n";
        return 1;
    }

    std::cout << "vendor=" << f.vendor_view() << " brand=" << f.brand_view() << "\n";
    std::cout << "best_simd_level=" << static_cast<int>(best_simd_level()) << "\n";

    const CurrentCore core = current_core();
    std::cout << "current_core x2apic_id=" << core.x2apic_id << " type=" << static_cast<int>(core.type) << "\n";

    // Odd, non-vector-aligned sizes deliberately exercise every tier's scalar tail path.
    for (const std::size_t n : {0uz, 1uz, 3uz, 7uz, 8uz, 15uz, 16uz, 17uz, 63uz, 257uz, 4096uz}) {
        if (!test_simd_ops<float>(n, 1e-4f))
            return 1;
        if (!test_simd_ops<double>(n, 1e-9))
            return 1;
        if (!test_wide_array_ops<SFT::Foundation::f128>(n))
            return 1;
        if (!test_wide_array_ops<SFT::Foundation::f256>(n))
            return 1;
    }

    std::cout << "all Cpu:: checks passed\n";
    return 0;
}
