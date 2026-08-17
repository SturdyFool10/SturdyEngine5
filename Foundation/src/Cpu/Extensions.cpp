#include <Foundation/src/Cpu/Extensions.hpp>

namespace SFT::Foundation::Cpu {

    /// Returns a human-readable name for the supplied extension value.
    ///
    /// @param extension `extension` value used by the operation.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    std::string_view extension_name(Extension extension) noexcept {
        switch (extension) {
            case Extension::X86_FPU: return "FPU";
            case Extension::X86_VME: return "VME";
            case Extension::X86_DE: return "DE";
            case Extension::X86_PSE: return "PSE";
            case Extension::X86_TSC: return "TSC";
            case Extension::X86_MSR: return "MSR";
            case Extension::X86_PAE: return "PAE";
            case Extension::X86_MCE: return "MCE";
            case Extension::X86_CX8: return "CX8";
            case Extension::X86_APIC: return "APIC";
            case Extension::X86_SEP: return "SEP";
            case Extension::X86_MTRR: return "MTRR";
            case Extension::X86_PGE: return "PGE";
            case Extension::X86_MCA: return "MCA";
            case Extension::X86_CMOV: return "CMOV";
            case Extension::X86_PAT: return "PAT";
            case Extension::X86_PSE36: return "PSE36";
            case Extension::X86_PSN: return "PSN";
            case Extension::X86_CLFSH: return "CLFSH";
            case Extension::X86_DS: return "DS";
            case Extension::X86_ACPI: return "ACPI";
            case Extension::X86_MMX: return "MMX";
            case Extension::X86_FXSR: return "FXSR";
            case Extension::X86_SSE: return "SSE";
            case Extension::X86_SSE2: return "SSE2";
            case Extension::X86_SS: return "SS";
            case Extension::X86_HTT: return "HTT";
            case Extension::X86_TM: return "TM";
            case Extension::X86_PBE: return "PBE";

            case Extension::X86_SSE3: return "SSE3";
            case Extension::X86_PCLMULQDQ: return "PCLMULQDQ";
            case Extension::X86_DTES64: return "DTES64";
            case Extension::X86_MONITOR: return "MONITOR";
            case Extension::X86_DS_CPL: return "DS_CPL";
            case Extension::X86_VMX: return "VMX";
            case Extension::X86_SMX: return "SMX";
            case Extension::X86_EIST: return "EIST";
            case Extension::X86_TM2: return "TM2";
            case Extension::X86_SSSE3: return "SSSE3";
            case Extension::X86_CNXT_ID: return "CNXT_ID";
            case Extension::X86_FMA: return "FMA";
            case Extension::X86_CX16: return "CX16";
            case Extension::X86_XTPR: return "XTPR";
            case Extension::X86_PDCM: return "PDCM";
            case Extension::X86_PCID: return "PCID";
            case Extension::X86_DCA: return "DCA";
            case Extension::X86_SSE4_1: return "SSE4_1";
            case Extension::X86_SSE4_2: return "SSE4_2";
            case Extension::X86_X2APIC: return "X2APIC";
            case Extension::X86_MOVBE: return "MOVBE";
            case Extension::X86_POPCNT: return "POPCNT";
            case Extension::X86_TSC_DEADLINE: return "TSC_DEADLINE";
            case Extension::X86_AES: return "AES";
            case Extension::X86_XSAVE: return "XSAVE";
            case Extension::X86_OSXSAVE: return "OSXSAVE";
            case Extension::X86_AVX: return "AVX";
            case Extension::X86_F16C: return "F16C";
            case Extension::X86_RDRAND: return "RDRAND";
            case Extension::X86_HYPERVISOR: return "HYPERVISOR";

            case Extension::X86_FSGSBASE: return "FSGSBASE";
            case Extension::X86_TSC_ADJUST: return "TSC_ADJUST";
            case Extension::X86_SGX: return "SGX";
            case Extension::X86_BMI1: return "BMI1";
            case Extension::X86_HLE: return "HLE";
            case Extension::X86_AVX2: return "AVX2";
            case Extension::X86_SMEP: return "SMEP";
            case Extension::X86_BMI2: return "BMI2";
            case Extension::X86_ERMS: return "ERMS";
            case Extension::X86_INVPCID: return "INVPCID";
            case Extension::X86_RTM: return "RTM";
            case Extension::X86_MPX: return "MPX";
            case Extension::X86_AVX512F: return "AVX512F";
            case Extension::X86_AVX512DQ: return "AVX512DQ";
            case Extension::X86_RDSEED: return "RDSEED";
            case Extension::X86_ADX: return "ADX";
            case Extension::X86_SMAP: return "SMAP";
            case Extension::X86_AVX512IFMA: return "AVX512IFMA";
            case Extension::X86_CLFLUSHOPT: return "CLFLUSHOPT";
            case Extension::X86_CLWB: return "CLWB";
            case Extension::X86_PT: return "PT";
            case Extension::X86_AVX512PF: return "AVX512PF";
            case Extension::X86_AVX512ER: return "AVX512ER";
            case Extension::X86_AVX512CD: return "AVX512CD";
            case Extension::X86_SHA: return "SHA";
            case Extension::X86_AVX512BW: return "AVX512BW";
            case Extension::X86_AVX512VL: return "AVX512VL";

            case Extension::X86_PREFETCHWT1: return "PREFETCHWT1";
            case Extension::X86_AVX512VBMI: return "AVX512VBMI";
            case Extension::X86_UMIP: return "UMIP";
            case Extension::X86_PKU: return "PKU";
            case Extension::X86_WAITPKG: return "WAITPKG";
            case Extension::X86_AVX512VBMI2: return "AVX512VBMI2";
            case Extension::X86_GFNI: return "GFNI";
            case Extension::X86_VAES: return "VAES";
            case Extension::X86_VPCLMULQDQ: return "VPCLMULQDQ";
            case Extension::X86_AVX512VNNI: return "AVX512VNNI";
            case Extension::X86_AVX512BITALG: return "AVX512BITALG";
            case Extension::X86_TME: return "TME";
            case Extension::X86_AVX512VPOPCNTDQ: return "AVX512VPOPCNTDQ";
            case Extension::X86_LA57: return "LA57";
            case Extension::X86_RDPID: return "RDPID";
            case Extension::X86_CLDEMOTE: return "CLDEMOTE";
            case Extension::X86_MOVDIRI: return "MOVDIRI";
            case Extension::X86_MOVDIR64B: return "MOVDIR64B";
            case Extension::X86_ENQCMD: return "ENQCMD";
            case Extension::X86_SGX_LC: return "SGX_LC";
            case Extension::X86_PKS: return "PKS";

            case Extension::X86_AVX512_4VNNIW: return "AVX512_4VNNIW";
            case Extension::X86_AVX512_4FMAPS: return "AVX512_4FMAPS";
            case Extension::X86_FSRM: return "FSRM";
            case Extension::X86_UINTR: return "UINTR";
            case Extension::X86_AVX512VP2INTERSECT: return "AVX512VP2INTERSECT";
            case Extension::X86_MD_CLEAR: return "MD_CLEAR";
            case Extension::X86_SERIALIZE: return "SERIALIZE";
            case Extension::X86_HYBRID: return "HYBRID";
            case Extension::X86_TSXLDTRK: return "TSXLDTRK";
            case Extension::X86_PCONFIG: return "PCONFIG";
            case Extension::X86_CET_IBT: return "CET_IBT";
            case Extension::X86_AMX_BF16: return "AMX_BF16";
            case Extension::X86_AVX512FP16: return "AVX512FP16";
            case Extension::X86_AMX_TILE: return "AMX_TILE";
            case Extension::X86_AMX_INT8: return "AMX_INT8";
            case Extension::X86_IBRS_IBPB: return "IBRS_IBPB";
            case Extension::X86_STIBP: return "STIBP";
            case Extension::X86_L1D_FLUSH: return "L1D_FLUSH";
            case Extension::X86_SSBD: return "SSBD";

            case Extension::X86_LAHF_LM: return "LAHF_LM";
            case Extension::X86_CMP_LEGACY: return "CMP_LEGACY";
            case Extension::X86_SVM: return "SVM";
            case Extension::X86_EXTAPIC: return "EXTAPIC";
            case Extension::X86_CR8_LEGACY: return "CR8_LEGACY";
            case Extension::X86_ABM: return "ABM";
            case Extension::X86_SSE4A: return "SSE4A";
            case Extension::X86_MISALIGNSSE: return "MISALIGNSSE";
            case Extension::X86_3DNOWPREFETCH: return "3DNOWPREFETCH";
            case Extension::X86_OSVW: return "OSVW";
            case Extension::X86_IBS: return "IBS";
            case Extension::X86_XOP: return "XOP";
            case Extension::X86_SKINIT: return "SKINIT";
            case Extension::X86_WDT: return "WDT";
            case Extension::X86_LWP: return "LWP";
            case Extension::X86_FMA4: return "FMA4";
            case Extension::X86_TCE: return "TCE";
            case Extension::X86_NODEID_MSR: return "NODEID_MSR";
            case Extension::X86_TBM: return "TBM";
            case Extension::X86_TOPOEXT: return "TOPOEXT";
            case Extension::X86_PERFCTR_CORE: return "PERFCTR_CORE";
            case Extension::X86_PERFCTR_NB: return "PERFCTR_NB";
            case Extension::X86_DBX: return "DBX";
            case Extension::X86_PERFTSC: return "PERFTSC";
            case Extension::X86_PCX_L2I: return "PCX_L2I";
            case Extension::X86_MONITORX: return "MONITORX";

            case Extension::X86_SYSCALL: return "SYSCALL";
            case Extension::X86_NX: return "NX";
            case Extension::X86_MMXEXT: return "MMXEXT";
            case Extension::X86_FXSR_OPT: return "FXSR_OPT";
            case Extension::X86_PDPE1GB: return "PDPE1GB";
            case Extension::X86_RDTSCP: return "RDTSCP";
            case Extension::X86_LM: return "LM";
            case Extension::X86_3DNOWEXT: return "3DNOWEXT";
            case Extension::X86_3DNOW: return "3DNOW";

            case Extension::ARM_NEON: return "NEON";
            case Extension::ARM_SVE: return "SVE";
            case Extension::ARM_SVE2: return "SVE2";
            case Extension::ARM_CRC32: return "CRC32";
            case Extension::ARM_AES: return "AES";
            case Extension::ARM_SHA1: return "SHA1";
            case Extension::ARM_SHA2: return "SHA2";
            case Extension::ARM_SHA3: return "SHA3";
            case Extension::ARM_SM3: return "SM3";
            case Extension::ARM_SM4: return "SM4";
            case Extension::ARM_DOTPROD: return "DOTPROD";
            case Extension::ARM_FP16: return "FP16";
            case Extension::ARM_BF16: return "BF16";
            case Extension::ARM_I8MM: return "I8MM";
            case Extension::ARM_RDM: return "RDM";
            case Extension::ARM_JSCVT: return "JSCVT";
            case Extension::ARM_FCMA: return "FCMA";
            case Extension::ARM_LSE: return "LSE";
            case Extension::ARM_RCPC: return "RCPC";
            case Extension::ARM_ATOMICS: return "ATOMICS";

            case Extension::RISCV_I: return "RV_I";
            case Extension::RISCV_M: return "RV_M";
            case Extension::RISCV_A: return "RV_A";
            case Extension::RISCV_F: return "RV_F";
            case Extension::RISCV_D: return "RV_D";
            case Extension::RISCV_C: return "RV_C";
            case Extension::RISCV_V: return "RV_V";
            case Extension::RISCV_B: return "RV_B";
            case Extension::RISCV_ZBA: return "RV_ZBA";
            case Extension::RISCV_ZBB: return "RV_ZBB";
            case Extension::RISCV_ZBC: return "RV_ZBC";
            case Extension::RISCV_ZBS: return "RV_ZBS";
            case Extension::RISCV_ZFH: return "RV_ZFH";

            case Extension::Count: return "<invalid>";
        }
        return "<unknown>";
    }

} // namespace SFT::Foundation::Cpu
