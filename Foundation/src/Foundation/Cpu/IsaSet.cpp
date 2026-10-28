#include <Foundation/Cpu/IsaSet.hpp>
#include <Foundation/Cpu/CoreMap.hpp>

namespace SFT::Foundation::Cpu {

    IsaSet::IsaSet(const std::vector<bool> &extensions) noexcept {
        const usize count = extensions.size() < extension_count ? extensions.size() : extension_count;
        for (usize i = 0; i < count; ++i) {
            if (extensions[i]) {
                bits_.set(i);
            }
        }
    }

    IsaSet required_extensions(IsaTier tier) noexcept {
        IsaSet required;
        switch (tier) {
            case IsaTier::Scalar:
                break;
            case IsaTier::X86Sse2:
                required.add(Extension::X86_SSE2);
                break;
            case IsaTier::X86Avx:
                required.add(Extension::X86_AVX);
                required.add(Extension::X86_OSXSAVE);
                break;
            case IsaTier::X86Avx2:
                required.add(Extension::X86_AVX);
                required.add(Extension::X86_AVX2);
                required.add(Extension::X86_FMA);
                required.add(Extension::X86_OSXSAVE);
                break;
            case IsaTier::X86Avx512:
                required.add(Extension::X86_AVX);
                required.add(Extension::X86_FMA);
                required.add(Extension::X86_AVX512F);
                required.add(Extension::X86_AVX512BW);
                required.add(Extension::X86_AVX512DQ);
                required.add(Extension::X86_AVX512VL);
                required.add(Extension::X86_OSXSAVE);
                break;
            case IsaTier::ArmNeon:
                required.add(Extension::ARM_NEON);
                break;
            case IsaTier::ArmSve2:
                required.add(Extension::ARM_SVE);
                required.add(Extension::ARM_SVE2);
                break;
            case IsaTier::RiscvV:
                required.add(Extension::RISCV_V);
                break;
        }
        return required;
    }

    IsaSet current_core_isa_set() noexcept {
        const CoreCapabilities *current = CoreMap::instance().capabilities_of_current_core();
        if (!current) {
            return {};
        }
        if (current->per_thread_isa_detected) {
            return current->isa_set();
        }
#if defined(__aarch64__) || defined(_M_ARM64)
        IsaSet baseline;
        baseline.add(Extension::ARM_NEON);
        return baseline;
#else
        return {};
#endif
    }

    IsaTier best_tier(const IsaSet &available) noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        if (available.contains(required_extensions(IsaTier::X86Avx512))) {
            return IsaTier::X86Avx512;
        }
        if (available.contains(required_extensions(IsaTier::X86Avx2))) {
            return IsaTier::X86Avx2;
        }
        if (available.contains(required_extensions(IsaTier::X86Avx))) {
            return IsaTier::X86Avx;
        }
        if (available.contains(required_extensions(IsaTier::X86Sse2))) {
            return IsaTier::X86Sse2;
        }
#elif defined(__aarch64__) || defined(_M_ARM64)
        if (available.contains(required_extensions(IsaTier::ArmSve2))) {
            return IsaTier::ArmSve2;
        }
        if (available.contains(required_extensions(IsaTier::ArmNeon))) {
            return IsaTier::ArmNeon;
        }
#elif defined(__riscv) && (__riscv_xlen == 64)
        if (available.contains(required_extensions(IsaTier::RiscvV))) {
            return IsaTier::RiscvV;
        }
#else
        (void)available;
#endif
        return IsaTier::Scalar;
    }

} // namespace SFT::Foundation::Cpu
