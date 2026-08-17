#include <Foundation/src/Cpu/CpuId.hpp>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #define STURDY_CPU_X86 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define STURDY_CPU_ARM64 1
#elif defined(__riscv) && (__riscv_xlen == 64)
    #define STURDY_CPU_RISCV64 1
#endif

#if defined(STURDY_CPU_X86)
    #include <cpuid.h>
    #include <immintrin.h>
    #include <cstring>
#elif defined(STURDY_CPU_RISCV64) && defined(__linux__)






    #include <sys/auxv.h>
    #ifndef AT_HWCAP
        #define AT_HWCAP 16
    #endif
#endif

namespace SFT::Foundation::Cpu {

#if defined(STURDY_CPU_X86)

    namespace {

        void read_leaf(unsigned int leaf, unsigned int subleaf, unsigned int &eax, unsigned int &ebx, unsigned int &ecx, unsigned int &edx) noexcept {
            __get_cpuid_count(leaf, subleaf, &eax, &ebx, &ecx, &edx);
        }

        [[nodiscard]] CpuFeatures detect_features() noexcept {
            CpuFeatures result{};

            unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
            read_leaf(0, 0, eax, ebx, ecx, edx);
            const unsigned int max_leaf = eax;


            std::memcpy(result.vendor + 0, &ebx, 4);
            std::memcpy(result.vendor + 4, &edx, 4);
            std::memcpy(result.vendor + 8, &ecx, 4);
            result.vendor[12] = '\0';

            if (max_leaf >= 1) {
                read_leaf(1, 0, eax, ebx, ecx, edx);
                result.sse = (edx >> 25) & 1;
                result.sse2 = (edx >> 26) & 1;
                result.sse3 = ecx & 1;
                result.ssse3 = (ecx >> 9) & 1;
                result.sse4_1 = (ecx >> 19) & 1;
                result.sse4_2 = (ecx >> 20) & 1;
                result.popcnt = (ecx >> 23) & 1;
                result.fma3 = (ecx >> 12) & 1;
                result.f16c = (ecx >> 29) & 1;
                result.avx = (ecx >> 28) & 1;

                const bool osxsave = (ecx >> 27) & 1;
                if (osxsave) {
                    const unsigned long long xcr0 = _xgetbv(0);

                    result.os_supports_avx = (xcr0 & 0x6ULL) == 0x6ULL;

                    result.os_supports_avx512 = result.os_supports_avx && (xcr0 & 0xE0ULL) == 0xE0ULL;
                }
            }

            if (max_leaf >= 7) {
                read_leaf(7, 0, eax, ebx, ecx, edx);
                result.avx2 = (ebx >> 5) & 1;
                result.bmi1 = (ebx >> 3) & 1;
                result.bmi2 = (ebx >> 8) & 1;
                result.avx512f = (ebx >> 16) & 1;
                result.avx512dq = (ebx >> 17) & 1;
                result.avx512cd = (ebx >> 28) & 1;
                result.avx512bw = (ebx >> 30) & 1;
                result.avx512vl = (ebx >> 31) & 1;
                result.hybrid = (edx >> 15) & 1;
            }

            unsigned int max_ext_leaf = 0;
            read_leaf(0x80000000, 0, max_ext_leaf, ebx, ecx, edx);
            if (max_ext_leaf >= 0x80000004) {
                unsigned int brand[12] = {};
                read_leaf(0x80000002, 0, brand[0], brand[1], brand[2], brand[3]);
                read_leaf(0x80000003, 0, brand[4], brand[5], brand[6], brand[7]);
                read_leaf(0x80000004, 0, brand[8], brand[9], brand[10], brand[11]);
                std::memcpy(result.brand, brand, sizeof(brand));
                result.brand[48] = '\0';
            }

            return result;
        }

    } // namespace

    const CpuFeatures &features() noexcept {
        static const CpuFeatures instance = detect_features();
        return instance;
    }

    SimdLevel best_simd_level() noexcept {
        const CpuFeatures &f = features();
        if (f.avx512f && f.os_supports_avx512) {
            return SimdLevel::AVX512;
        }
        if (f.avx2 && f.fma3 && f.os_supports_avx) {
            return SimdLevel::AVX2;
        }
        if (f.avx && f.os_supports_avx) {
            return SimdLevel::AVX;
        }
        return SimdLevel::Scalar;
    }

#elif defined(STURDY_CPU_ARM64)

    const CpuFeatures &features() noexcept {


        static const CpuFeatures instance = [] {
            CpuFeatures f{};
            f.neon = true;
            return f;
        }();
        return instance;
    }

    SimdLevel best_simd_level() noexcept { return SimdLevel::NEON; }

#elif defined(STURDY_CPU_RISCV64)

    namespace {

        [[nodiscard]] CpuFeatures detect_features() noexcept {
            CpuFeatures result{};
#if defined(__linux__)
            const unsigned long hwcap = getauxval(AT_HWCAP);


            result.rvv = ((hwcap >> (static_cast<unsigned>('V') - static_cast<unsigned>('A'))) & 1UL) != 0UL;
#endif
            return result;
        }

    } // namespace

    const CpuFeatures &features() noexcept {
        static const CpuFeatures instance = detect_features();
        return instance;
    }

    SimdLevel best_simd_level() noexcept { return features().rvv ? SimdLevel::RVV : SimdLevel::Scalar; }

#else

    const CpuFeatures &features() noexcept {
        static const CpuFeatures instance{};
        return instance;
    }

    SimdLevel best_simd_level() noexcept { return SimdLevel::Scalar; }

#endif

} // namespace SFT::Foundation::Cpu

namespace SFT::Foundation::Cpu {

    std::string_view CpuFeatures::vendor_view() const noexcept { return vendor; }

    std::string_view CpuFeatures::brand_view() const noexcept { return brand; }

} // namespace SFT::Foundation::Cpu

