#pragma once

#include <string_view>

namespace SFT::Foundation::Cpu {

    // Every ISA extension `CoreMap` (CoreMap.hpp) can record per logical core, across every
    // architecture this project builds for. One flat enum with architecture-tagged regions (same
    // convention as `SimdLevel` in CpuId.hpp) rather than per-architecture enums, so a single
    // `std::vector<bool>` indexed by this enum can represent "everything this core supports" uniformly.
    //
    // Detection depth varies honestly by architecture — see CoreMap.cpp:
    //   x86: every bit here is read directly from `cpuid` (leaves 1, 7 subleaf 0, 0x80000001) on
    //     whichever logical core is pinned when it's read — genuinely per-core, genuinely exhaustive
    //     for what those three leaves expose.
    //   Arm64: only `ARM_NEON` is ever `true` (mandatory AArch64 baseline, no detection needed); every
    //     other Arm64 entry exists to name the extension but is always `false` here (see CpuId.hpp's
    //     `CpuFeatures::sve` comment for why — no verified, OS-independent way to detect them).
    //   RISC-V: only the base letters directly exposed by Linux's HWCAP (`RISCV_I/M/A/F/D/C/V`) are
    //     ever detected (see CpuId.cpp); the Z*-prefixed sub-extensions are named but always `false`.
    enum class Extension {
        // --- x86: leaf 1 EDX ---------------------------------------------------------------------
        X86_FPU, X86_VME, X86_DE, X86_PSE, X86_TSC, X86_MSR, X86_PAE, X86_MCE, X86_CX8, X86_APIC,
        X86_SEP, X86_MTRR, X86_PGE, X86_MCA, X86_CMOV, X86_PAT, X86_PSE36, X86_PSN, X86_CLFSH,
        X86_DS, X86_ACPI, X86_MMX, X86_FXSR, X86_SSE, X86_SSE2, X86_SS, X86_HTT, X86_TM, X86_PBE,

        // --- x86: leaf 1 ECX ----------------------------------------------------------------------
        X86_SSE3, X86_PCLMULQDQ, X86_DTES64, X86_MONITOR, X86_DS_CPL, X86_VMX, X86_SMX, X86_EIST,
        X86_TM2, X86_SSSE3, X86_CNXT_ID, X86_FMA, X86_CX16, X86_XTPR, X86_PDCM, X86_PCID, X86_DCA,
        X86_SSE4_1, X86_SSE4_2, X86_X2APIC, X86_MOVBE, X86_POPCNT, X86_TSC_DEADLINE, X86_AES,
        X86_XSAVE, X86_OSXSAVE, X86_AVX, X86_F16C, X86_RDRAND, X86_HYPERVISOR,

        // --- x86: leaf 7 subleaf 0 EBX --------------------------------------------------------------
        X86_FSGSBASE, X86_TSC_ADJUST, X86_SGX, X86_BMI1, X86_HLE, X86_AVX2, X86_SMEP, X86_BMI2,
        X86_ERMS, X86_INVPCID, X86_RTM, X86_MPX, X86_AVX512F, X86_AVX512DQ, X86_RDSEED, X86_ADX,
        X86_SMAP, X86_AVX512IFMA, X86_CLFLUSHOPT, X86_CLWB, X86_PT, X86_AVX512PF, X86_AVX512ER,
        X86_AVX512CD, X86_SHA, X86_AVX512BW, X86_AVX512VL,

        // --- x86: leaf 7 subleaf 0 ECX --------------------------------------------------------------
        X86_PREFETCHWT1, X86_AVX512VBMI, X86_UMIP, X86_PKU, X86_WAITPKG, X86_AVX512VBMI2, X86_GFNI,
        X86_VAES, X86_VPCLMULQDQ, X86_AVX512VNNI, X86_AVX512BITALG, X86_TME, X86_AVX512VPOPCNTDQ,
        X86_LA57, X86_RDPID, X86_CLDEMOTE, X86_MOVDIRI, X86_MOVDIR64B, X86_ENQCMD, X86_SGX_LC, X86_PKS,

        // --- x86: leaf 7 subleaf 0 EDX --------------------------------------------------------------
        X86_AVX512_4VNNIW, X86_AVX512_4FMAPS, X86_FSRM, X86_UINTR, X86_AVX512VP2INTERSECT,
        X86_MD_CLEAR, X86_SERIALIZE, X86_HYBRID, X86_TSXLDTRK, X86_PCONFIG, X86_CET_IBT,
        X86_AMX_BF16, X86_AVX512FP16, X86_AMX_TILE, X86_AMX_INT8, X86_IBRS_IBPB, X86_STIBP,
        X86_L1D_FLUSH, X86_SSBD,

        // --- x86: extended leaf 0x80000001 ECX (mostly AMD) --------------------------------------
        X86_LAHF_LM, X86_CMP_LEGACY, X86_SVM, X86_EXTAPIC, X86_CR8_LEGACY, X86_ABM, X86_SSE4A,
        X86_MISALIGNSSE, X86_3DNOWPREFETCH, X86_OSVW, X86_IBS, X86_XOP, X86_SKINIT, X86_WDT,
        X86_LWP, X86_FMA4, X86_TCE, X86_NODEID_MSR, X86_TBM, X86_TOPOEXT, X86_PERFCTR_CORE,
        X86_PERFCTR_NB, X86_DBX, X86_PERFTSC, X86_PCX_L2I, X86_MONITORX,

        // --- x86: extended leaf 0x80000001 EDX ---------------------------------------------------
        X86_SYSCALL, X86_NX, X86_MMXEXT, X86_FXSR_OPT, X86_PDPE1GB, X86_RDTSCP, X86_LM,
        X86_3DNOWEXT, X86_3DNOW,

        // --- Arm64 ---------------------------------------------------------------------------------
        ARM_NEON, ARM_SVE, ARM_SVE2, ARM_CRC32, ARM_AES, ARM_SHA1, ARM_SHA2, ARM_SHA3, ARM_SM3,
        ARM_SM4, ARM_DOTPROD, ARM_FP16, ARM_BF16, ARM_I8MM, ARM_RDM, ARM_JSCVT, ARM_FCMA, ARM_LSE,
        ARM_RCPC, ARM_ATOMICS,

        // --- RISC-V --------------------------------------------------------------------------------
        RISCV_I, RISCV_M, RISCV_A, RISCV_F, RISCV_D, RISCV_C, RISCV_V, RISCV_B, RISCV_ZBA,
        RISCV_ZBB, RISCV_ZBC, RISCV_ZBS, RISCV_ZFH,

        Count, // sentinel: not a real extension, sized `vector<bool>`s use this as their length
    };

    // Stable, human-readable name (e.g. "AVX512F", "NEON", "RVV_V") for logging/diagnostics.
    [[nodiscard]] std::string_view extension_name(Extension extension) noexcept;

} // namespace SFT::Foundation::Cpu
