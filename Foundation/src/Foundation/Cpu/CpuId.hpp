#pragma once

#include <Foundation/Types.hpp>

#include <string_view>

namespace SFT::Foundation::Cpu {


    struct CpuFeatures {

        bool sse = false;
        bool sse2 = false;
        bool sse3 = false;
        bool ssse3 = false;
        bool sse4_1 = false;
        bool sse4_2 = false;
        bool popcnt = false;
        bool avx = false;
        bool avx2 = false;
        bool fma3 = false;
        bool f16c = false;
        bool bmi1 = false;
        bool bmi2 = false;
        bool avx512f = false;
        bool avx512bw = false;
        bool avx512cd = false;
        bool avx512dq = false;
        bool avx512vl = false;


        bool hybrid = false;


        bool os_supports_avx = false;
        bool os_supports_avx512 = false;


        bool neon = false;


        bool sve = false;


        bool rvv = false;

        char vendor[13] = {};
        char brand[49] = {};

        /// Returns the current or globally available vendor view value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::string_view vendor_view() const noexcept;
        /// Returns the current or globally available brand view value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::string_view brand_view() const noexcept;
    };


    enum class SimdLevel {
        Scalar,
        AVX,
        AVX2,
        AVX512,
        NEON,
        RVV,
    };


    /// Returns the current or globally available features value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const CpuFeatures &features() noexcept;


    /// Returns the current or globally available best simd level value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SimdLevel best_simd_level() noexcept;

} // namespace SFT::Foundation::Cpu
