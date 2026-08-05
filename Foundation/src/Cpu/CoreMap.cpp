#include <Foundation/src/Cpu/CoreMap.hpp>
#include <Foundation/src/Cpu/CpuId.hpp>

#include <algorithm>
#include <thread>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #define STURDY_CPU_X86 1
#endif

#if defined(STURDY_CPU_X86)
    #include <cpuid.h>
#endif

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#elif defined(__APPLE__)
    #include <mach/mach.h>
    #include <mach/thread_policy.h>
    #include <pthread.h>
#elif defined(__linux__)
    #include <pthread.h>
    #include <sched.h>
    #include <unistd.h>
#endif

namespace SFT::Foundation::Cpu {

    bool operator==(const CoreCapabilities &a, const CoreCapabilities &b) noexcept {
        return a.type == b.type && a.l1d_bytes == b.l1d_bytes && a.l1i_bytes == b.l1i_bytes &&
               a.l2_bytes == b.l2_bytes && a.l3_bytes == b.l3_bytes && a.extensions == b.extensions;
    }

    namespace {

#if defined(STURDY_CPU_X86)

        void read_leaf(unsigned int leaf, unsigned int subleaf, unsigned int &eax, unsigned int &ebx, unsigned int &ecx, unsigned int &edx) noexcept {
            __get_cpuid_count(leaf, subleaf, &eax, &ebx, &ecx, &edx);
        }

        [[nodiscard]] bool has_bit(unsigned int reg, unsigned int position) noexcept {
            return ((reg >> position) & 1u) != 0u;
        }

        void set_bit(std::vector<bool> &extensions, Extension extension, bool value) noexcept {
            extensions[static_cast<usize>(extension)] = value;
        }

        [[nodiscard]] u32 read_x2apic_id() noexcept {
            // Same leaf-preference order as CpuTopology.cpp's read_x2apic_id() -- duplicated rather than
            // shared across translation units, matching this module's existing per-file convention (see
            // e.g. read_leaf above, independently defined in CpuId.cpp/CpuTopology.cpp too).
            unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
            read_leaf(0, 0, eax, ebx, ecx, edx);
            const unsigned int max_leaf = eax;
            if (max_leaf >= 0x1f) {
                read_leaf(0x1f, 0, eax, ebx, ecx, edx);
                return edx;
            }
            if (max_leaf >= 0xb) {
                read_leaf(0xb, 0, eax, ebx, ecx, edx);
                return edx;
            }
            read_leaf(1, 0, eax, ebx, ecx, edx);
            return ebx >> 24;
        }

        [[nodiscard]] CoreType read_core_type() noexcept {
            // Duplicated from CpuTopology.cpp's read_core_type() (anonymous-namespace, not shared across
            // TUs) -- GenuineIntel-only leaf 0x1A decode, see that file's header comment for why every
            // other vendor (including hybrid AMD designs) reports Unknown here.
            if (!Cpu::features().hybrid || Cpu::features().vendor_view() != "GenuineIntel") {
                return CoreType::Unknown;
            }
            unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
            read_leaf(0x1a, 0, eax, ebx, ecx, edx);
            const unsigned int core_type = eax >> 24;
            if (core_type == 0x20) {
                return CoreType::Efficiency; // Intel Atom-derived core
            }
            if (core_type == 0x40) {
                return CoreType::Performance; // Intel Core-derived core
            }
            return CoreType::Unknown;
        }

        // Every bit this module knows a name for, across leaf 1 (EDX/ECX), leaf 7 subleaf 0
        // (EBX/ECX/EDX), and extended leaf 0x80000001 (ECX/EDX). Bit positions cross-checked against
        // the Linux kernel's arch/x86/include/asm/cpufeatures.h (the authoritative, battle-tested
        // mapping) rather than trusting hand-recalled SDM tables or compiler cpuid.h macros -- the
        // latter's bit_PKU/bit_UMIP pairing in this toolchain's own <cpuid.h> turned out to disagree
        // with the kernel source on which bit is which, which is exactly the kind of transcription
        // error this cross-check exists to catch.
        void decode_extensions(std::vector<bool> &ext) noexcept {
            unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
            read_leaf(0, 0, eax, ebx, ecx, edx);
            const unsigned int max_leaf = eax;

            if (max_leaf >= 1) {
                read_leaf(1, 0, eax, ebx, ecx, edx);

                // EDX
                set_bit(ext, Extension::X86_FPU, has_bit(edx, 0));
                set_bit(ext, Extension::X86_VME, has_bit(edx, 1));
                set_bit(ext, Extension::X86_DE, has_bit(edx, 2));
                set_bit(ext, Extension::X86_PSE, has_bit(edx, 3));
                set_bit(ext, Extension::X86_TSC, has_bit(edx, 4));
                set_bit(ext, Extension::X86_MSR, has_bit(edx, 5));
                set_bit(ext, Extension::X86_PAE, has_bit(edx, 6));
                set_bit(ext, Extension::X86_MCE, has_bit(edx, 7));
                set_bit(ext, Extension::X86_CX8, has_bit(edx, 8));
                set_bit(ext, Extension::X86_APIC, has_bit(edx, 9));
                set_bit(ext, Extension::X86_SEP, has_bit(edx, 11));
                set_bit(ext, Extension::X86_MTRR, has_bit(edx, 12));
                set_bit(ext, Extension::X86_PGE, has_bit(edx, 13));
                set_bit(ext, Extension::X86_MCA, has_bit(edx, 14));
                set_bit(ext, Extension::X86_CMOV, has_bit(edx, 15));
                set_bit(ext, Extension::X86_PAT, has_bit(edx, 16));
                set_bit(ext, Extension::X86_PSE36, has_bit(edx, 17));
                set_bit(ext, Extension::X86_PSN, has_bit(edx, 18));
                set_bit(ext, Extension::X86_CLFSH, has_bit(edx, 19));
                set_bit(ext, Extension::X86_DS, has_bit(edx, 21));
                set_bit(ext, Extension::X86_ACPI, has_bit(edx, 22));
                set_bit(ext, Extension::X86_MMX, has_bit(edx, 23));
                set_bit(ext, Extension::X86_FXSR, has_bit(edx, 24));
                set_bit(ext, Extension::X86_SSE, has_bit(edx, 25));
                set_bit(ext, Extension::X86_SSE2, has_bit(edx, 26));
                set_bit(ext, Extension::X86_SS, has_bit(edx, 27));
                set_bit(ext, Extension::X86_HTT, has_bit(edx, 28));
                set_bit(ext, Extension::X86_TM, has_bit(edx, 29));
                set_bit(ext, Extension::X86_PBE, has_bit(edx, 31));

                // ECX
                set_bit(ext, Extension::X86_SSE3, has_bit(ecx, 0));
                set_bit(ext, Extension::X86_PCLMULQDQ, has_bit(ecx, 1));
                set_bit(ext, Extension::X86_DTES64, has_bit(ecx, 2));
                set_bit(ext, Extension::X86_MONITOR, has_bit(ecx, 3));
                set_bit(ext, Extension::X86_DS_CPL, has_bit(ecx, 4));
                set_bit(ext, Extension::X86_VMX, has_bit(ecx, 5));
                set_bit(ext, Extension::X86_SMX, has_bit(ecx, 6));
                set_bit(ext, Extension::X86_EIST, has_bit(ecx, 7));
                set_bit(ext, Extension::X86_TM2, has_bit(ecx, 8));
                set_bit(ext, Extension::X86_SSSE3, has_bit(ecx, 9));
                set_bit(ext, Extension::X86_CNXT_ID, has_bit(ecx, 10));
                set_bit(ext, Extension::X86_FMA, has_bit(ecx, 12));
                set_bit(ext, Extension::X86_CX16, has_bit(ecx, 13));
                set_bit(ext, Extension::X86_XTPR, has_bit(ecx, 14));
                set_bit(ext, Extension::X86_PDCM, has_bit(ecx, 15));
                set_bit(ext, Extension::X86_PCID, has_bit(ecx, 17));
                set_bit(ext, Extension::X86_DCA, has_bit(ecx, 18));
                set_bit(ext, Extension::X86_SSE4_1, has_bit(ecx, 19));
                set_bit(ext, Extension::X86_SSE4_2, has_bit(ecx, 20));
                set_bit(ext, Extension::X86_X2APIC, has_bit(ecx, 21));
                set_bit(ext, Extension::X86_MOVBE, has_bit(ecx, 22));
                set_bit(ext, Extension::X86_POPCNT, has_bit(ecx, 23));
                set_bit(ext, Extension::X86_TSC_DEADLINE, has_bit(ecx, 24));
                set_bit(ext, Extension::X86_AES, has_bit(ecx, 25));
                set_bit(ext, Extension::X86_XSAVE, has_bit(ecx, 26));
                set_bit(ext, Extension::X86_OSXSAVE, has_bit(ecx, 27));
                set_bit(ext, Extension::X86_AVX, has_bit(ecx, 28));
                set_bit(ext, Extension::X86_F16C, has_bit(ecx, 29));
                set_bit(ext, Extension::X86_RDRAND, has_bit(ecx, 30));
                set_bit(ext, Extension::X86_HYPERVISOR, has_bit(ecx, 31));
            }

            if (max_leaf >= 7) {
                read_leaf(7, 0, eax, ebx, ecx, edx);

                // EBX
                set_bit(ext, Extension::X86_FSGSBASE, has_bit(ebx, 0));
                set_bit(ext, Extension::X86_TSC_ADJUST, has_bit(ebx, 1));
                set_bit(ext, Extension::X86_SGX, has_bit(ebx, 2));
                set_bit(ext, Extension::X86_BMI1, has_bit(ebx, 3));
                set_bit(ext, Extension::X86_HLE, has_bit(ebx, 4));
                set_bit(ext, Extension::X86_AVX2, has_bit(ebx, 5));
                set_bit(ext, Extension::X86_SMEP, has_bit(ebx, 7));
                set_bit(ext, Extension::X86_BMI2, has_bit(ebx, 8));
                set_bit(ext, Extension::X86_ERMS, has_bit(ebx, 9));
                set_bit(ext, Extension::X86_INVPCID, has_bit(ebx, 10));
                set_bit(ext, Extension::X86_RTM, has_bit(ebx, 11));
                set_bit(ext, Extension::X86_MPX, has_bit(ebx, 14));
                set_bit(ext, Extension::X86_AVX512F, has_bit(ebx, 16));
                set_bit(ext, Extension::X86_AVX512DQ, has_bit(ebx, 17));
                set_bit(ext, Extension::X86_RDSEED, has_bit(ebx, 18));
                set_bit(ext, Extension::X86_ADX, has_bit(ebx, 19));
                set_bit(ext, Extension::X86_SMAP, has_bit(ebx, 20));
                set_bit(ext, Extension::X86_AVX512IFMA, has_bit(ebx, 21));
                set_bit(ext, Extension::X86_CLFLUSHOPT, has_bit(ebx, 23));
                set_bit(ext, Extension::X86_CLWB, has_bit(ebx, 24));
                set_bit(ext, Extension::X86_PT, has_bit(ebx, 25));
                set_bit(ext, Extension::X86_AVX512PF, has_bit(ebx, 26));
                set_bit(ext, Extension::X86_AVX512ER, has_bit(ebx, 27));
                set_bit(ext, Extension::X86_AVX512CD, has_bit(ebx, 28));
                set_bit(ext, Extension::X86_SHA, has_bit(ebx, 29));
                set_bit(ext, Extension::X86_AVX512BW, has_bit(ebx, 30));
                set_bit(ext, Extension::X86_AVX512VL, has_bit(ebx, 31));

                // ECX
                set_bit(ext, Extension::X86_PREFETCHWT1, has_bit(ecx, 0));
                set_bit(ext, Extension::X86_AVX512VBMI, has_bit(ecx, 1));
                set_bit(ext, Extension::X86_UMIP, has_bit(ecx, 2));
                set_bit(ext, Extension::X86_PKU, has_bit(ecx, 3));
                set_bit(ext, Extension::X86_WAITPKG, has_bit(ecx, 5));
                set_bit(ext, Extension::X86_AVX512VBMI2, has_bit(ecx, 6));
                set_bit(ext, Extension::X86_GFNI, has_bit(ecx, 8));
                set_bit(ext, Extension::X86_VAES, has_bit(ecx, 9));
                set_bit(ext, Extension::X86_VPCLMULQDQ, has_bit(ecx, 10));
                set_bit(ext, Extension::X86_AVX512VNNI, has_bit(ecx, 11));
                set_bit(ext, Extension::X86_AVX512BITALG, has_bit(ecx, 12));
                set_bit(ext, Extension::X86_TME, has_bit(ecx, 13));
                set_bit(ext, Extension::X86_AVX512VPOPCNTDQ, has_bit(ecx, 14));
                set_bit(ext, Extension::X86_LA57, has_bit(ecx, 16));
                set_bit(ext, Extension::X86_RDPID, has_bit(ecx, 22));
                set_bit(ext, Extension::X86_CLDEMOTE, has_bit(ecx, 25));
                set_bit(ext, Extension::X86_MOVDIRI, has_bit(ecx, 27));
                set_bit(ext, Extension::X86_MOVDIR64B, has_bit(ecx, 28));
                set_bit(ext, Extension::X86_ENQCMD, has_bit(ecx, 29));
                set_bit(ext, Extension::X86_SGX_LC, has_bit(ecx, 30));
                set_bit(ext, Extension::X86_PKS, has_bit(ecx, 31));

                // EDX
                set_bit(ext, Extension::X86_AVX512_4VNNIW, has_bit(edx, 2));
                set_bit(ext, Extension::X86_AVX512_4FMAPS, has_bit(edx, 3));
                set_bit(ext, Extension::X86_FSRM, has_bit(edx, 4));
                set_bit(ext, Extension::X86_UINTR, has_bit(edx, 5));
                set_bit(ext, Extension::X86_AVX512VP2INTERSECT, has_bit(edx, 8));
                set_bit(ext, Extension::X86_MD_CLEAR, has_bit(edx, 10));
                set_bit(ext, Extension::X86_SERIALIZE, has_bit(edx, 14));
                set_bit(ext, Extension::X86_HYBRID, has_bit(edx, 15));
                set_bit(ext, Extension::X86_TSXLDTRK, has_bit(edx, 16));
                set_bit(ext, Extension::X86_PCONFIG, has_bit(edx, 18));
                set_bit(ext, Extension::X86_CET_IBT, has_bit(edx, 20));
                set_bit(ext, Extension::X86_AMX_BF16, has_bit(edx, 22));
                set_bit(ext, Extension::X86_AVX512FP16, has_bit(edx, 23));
                set_bit(ext, Extension::X86_AMX_TILE, has_bit(edx, 24));
                set_bit(ext, Extension::X86_AMX_INT8, has_bit(edx, 25));
                set_bit(ext, Extension::X86_IBRS_IBPB, has_bit(edx, 26));
                set_bit(ext, Extension::X86_STIBP, has_bit(edx, 27));
                set_bit(ext, Extension::X86_L1D_FLUSH, has_bit(edx, 28));
                set_bit(ext, Extension::X86_SSBD, has_bit(edx, 31));
            }

            unsigned int max_ext_leaf = 0;
            read_leaf(0x80000000, 0, max_ext_leaf, ebx, ecx, edx);
            if (max_ext_leaf >= 0x80000001) {
                read_leaf(0x80000001, 0, eax, ebx, ecx, edx);

                // ECX
                set_bit(ext, Extension::X86_LAHF_LM, has_bit(ecx, 0));
                set_bit(ext, Extension::X86_CMP_LEGACY, has_bit(ecx, 1));
                set_bit(ext, Extension::X86_SVM, has_bit(ecx, 2));
                set_bit(ext, Extension::X86_EXTAPIC, has_bit(ecx, 3));
                set_bit(ext, Extension::X86_CR8_LEGACY, has_bit(ecx, 4));
                set_bit(ext, Extension::X86_ABM, has_bit(ecx, 5));
                set_bit(ext, Extension::X86_SSE4A, has_bit(ecx, 6));
                set_bit(ext, Extension::X86_MISALIGNSSE, has_bit(ecx, 7));
                set_bit(ext, Extension::X86_3DNOWPREFETCH, has_bit(ecx, 8));
                set_bit(ext, Extension::X86_OSVW, has_bit(ecx, 9));
                set_bit(ext, Extension::X86_IBS, has_bit(ecx, 10));
                set_bit(ext, Extension::X86_XOP, has_bit(ecx, 11));
                set_bit(ext, Extension::X86_SKINIT, has_bit(ecx, 12));
                set_bit(ext, Extension::X86_WDT, has_bit(ecx, 13));
                set_bit(ext, Extension::X86_LWP, has_bit(ecx, 15));
                set_bit(ext, Extension::X86_FMA4, has_bit(ecx, 16));
                set_bit(ext, Extension::X86_TCE, has_bit(ecx, 17));
                set_bit(ext, Extension::X86_NODEID_MSR, has_bit(ecx, 19));
                set_bit(ext, Extension::X86_TBM, has_bit(ecx, 21));
                set_bit(ext, Extension::X86_TOPOEXT, has_bit(ecx, 22));
                set_bit(ext, Extension::X86_PERFCTR_CORE, has_bit(ecx, 23));
                set_bit(ext, Extension::X86_PERFCTR_NB, has_bit(ecx, 24));
                set_bit(ext, Extension::X86_DBX, has_bit(ecx, 26));
                set_bit(ext, Extension::X86_PERFTSC, has_bit(ecx, 27));
                set_bit(ext, Extension::X86_PCX_L2I, has_bit(ecx, 28));
                set_bit(ext, Extension::X86_MONITORX, has_bit(ecx, 29));

                // EDX
                set_bit(ext, Extension::X86_SYSCALL, has_bit(edx, 11));
                set_bit(ext, Extension::X86_NX, has_bit(edx, 20));
                set_bit(ext, Extension::X86_MMXEXT, has_bit(edx, 22));
                set_bit(ext, Extension::X86_FXSR_OPT, has_bit(edx, 25));
                set_bit(ext, Extension::X86_PDPE1GB, has_bit(edx, 26));
                set_bit(ext, Extension::X86_RDTSCP, has_bit(edx, 27));
                set_bit(ext, Extension::X86_LM, has_bit(edx, 29));
                set_bit(ext, Extension::X86_3DNOWEXT, has_bit(edx, 30));
                set_bit(ext, Extension::X86_3DNOW, has_bit(edx, 31));
            }
        }

        struct CacheSizes {
            usize l1d = 0;
            usize l1i = 0;
            usize l2 = 0;
            usize l3 = 0;
        };

        // Leaf 4 (Intel-style deterministic cache parameters) and AMD's leaf 0x8000001D share the exact
        // same EAX/EBX/ECX field layout by design (AMD modeled 0x8000001D on Intel's leaf 4 for this
        // reason) -- one decoder handles both, selected by `cache_leaf`. Iterates subleaves until a
        // cache-type-0 entry ends the list (4-6 entries is typical: L1D, L1I, L2, L3, occasionally more).
        [[nodiscard]] CacheSizes decode_cache_leaves(unsigned int cache_leaf) noexcept {
            CacheSizes sizes{};
            for (unsigned int subleaf = 0; subleaf < 16; ++subleaf) {
                unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
                read_leaf(cache_leaf, subleaf, eax, ebx, ecx, edx);
                const unsigned int cache_type = eax & 0x1fu; // 0=null, 1=data, 2=instruction, 3=unified
                if (cache_type == 0) {
                    break;
                }
                const unsigned int level = (eax >> 5) & 0x7u;
                const usize ways = ((ebx >> 22) & 0x3ffu) + 1;
                const usize partitions = ((ebx >> 12) & 0x3ffu) + 1;
                const usize line_size = (ebx & 0xfffu) + 1;
                const usize sets = static_cast<usize>(ecx) + 1;
                const usize size_bytes = ways * partitions * line_size * sets;

                if (level == 1 && cache_type == 1) {
                    sizes.l1d = size_bytes;
                } else if (level == 1 && cache_type == 2) {
                    sizes.l1i = size_bytes;
                } else if (level == 2) {
                    sizes.l2 = size_bytes;
                } else if (level == 3) {
                    sizes.l3 = size_bytes; // the X3D signal -- differs per core on multi-CCD parts
                }
            }
            return sizes;
        }

        [[nodiscard]] CoreCapabilities detect_current_core() noexcept {
            CoreCapabilities caps{};
            caps.extensions.assign(static_cast<usize>(Extension::Count), false);
            decode_extensions(caps.extensions);
            caps.x2apic_id = read_x2apic_id();
            caps.type = read_core_type();

            const bool is_amd = Cpu::features().vendor_view() == "AuthenticAMD";
            const bool has_topoext = caps.has(Extension::X86_TOPOEXT);
            const CacheSizes cache = decode_cache_leaves(is_amd && has_topoext ? 0x8000001du : 4u);
            caps.l1d_bytes = cache.l1d;
            caps.l1i_bytes = cache.l1i;
            caps.l2_bytes = cache.l2;
            caps.l3_bytes = cache.l3;
            return caps;
        }

    #if defined(__linux__)

        [[nodiscard]] std::vector<u32> enumerate_cores() noexcept {
            std::vector<u32> cores;
            cpu_set_t available;
            CPU_ZERO(&available);
            if (sched_getaffinity(0, sizeof(available), &available) == 0) {
                for (int i = 0; i < CPU_SETSIZE; ++i) {
                    if (CPU_ISSET(i, &available)) {
                        cores.push_back(static_cast<u32>(i));
                    }
                }
            }
            if (cores.empty()) {
                const long online = sysconf(_SC_NPROCESSORS_ONLN);
                const u32 count = online > 0 ? static_cast<u32>(online) : 1u;
                for (u32 i = 0; i < count; ++i) {
                    cores.push_back(i);
                }
            }
            return cores;
        }

        // RAII: restores this thread's original affinity mask on scope exit, however the enumeration
        // loop below returns (including early on an empty core list). Linux only -- see the per-platform
        // pin_current_thread()/enumerate_cores() overloads for why Windows/macOS/other need their own
        // shapes here rather than sharing this type.
        class ScopedAffinityRestore {
          public:
            ScopedAffinityRestore() noexcept {
                had_original_ = pthread_getaffinity_np(pthread_self(), sizeof(original_), &original_) == 0;
            }
            ~ScopedAffinityRestore() noexcept {
                if (had_original_) {
                    pthread_setaffinity_np(pthread_self(), sizeof(original_), &original_);
                }
            }
            ScopedAffinityRestore(const ScopedAffinityRestore &) = delete;
            ScopedAffinityRestore &operator=(const ScopedAffinityRestore &) = delete;

          private:
            cpu_set_t original_{};
            bool had_original_ = false;
        };

        bool pin_current_thread(u32 core_index) noexcept {
            cpu_set_t target;
            CPU_ZERO(&target);
            CPU_SET(static_cast<int>(core_index), &target);
            return pthread_setaffinity_np(pthread_self(), sizeof(target), &target) == 0;
        }

    #elif defined(__APPLE__)

        [[nodiscard]] std::vector<u32> enumerate_cores() noexcept {
            std::vector<u32> cores;
            const unsigned int count = std::thread::hardware_concurrency();
            for (u32 i = 0; i < (count > 0 ? count : 1u); ++i) {
                cores.push_back(i);
            }
            return cores;
        }

        // Mach's thread-affinity-tag API is a scheduling *hint*, not a hard pin (same caveat already
        // documented for DedicatedThread::pin_to_core in Async/src/AffinityImpl.cpp) -- there is also no
        // Mach API to query a thread's *current* affinity tag, so unlike Linux there is nothing to save.
        // The destructor just releases the hint (THREAD_AFFINITY_TAG_NULL) rather than restoring a prior
        // value. Per-core detection built this way on macOS/x86 is therefore best-effort and unverified
        // (see CoreMap.hpp's header comment); on Apple Silicon there's no per-core signal to read anyway
        // (no `cpuid`), so this path only ever matters for x86 Macs.
        class ScopedAffinityRestore {
          public:
            ~ScopedAffinityRestore() noexcept {
                thread_affinity_policy_data_t policy{.affinity_tag = THREAD_AFFINITY_TAG_NULL};
                thread_policy_set(pthread_mach_thread_np(pthread_self()),
                                   THREAD_AFFINITY_POLICY,
                                   reinterpret_cast<thread_policy_t>(&policy),
                                   THREAD_AFFINITY_POLICY_COUNT);
            }
        };

        bool pin_current_thread(u32 core_index) noexcept {
            thread_affinity_policy_data_t policy{.affinity_tag = static_cast<integer_t>(core_index)};
            return thread_policy_set(pthread_mach_thread_np(pthread_self()),
                                      THREAD_AFFINITY_POLICY,
                                      reinterpret_cast<thread_policy_t>(&policy),
                                      THREAD_AFFINITY_POLICY_COUNT) == KERN_SUCCESS;
        }

    #elif !defined(_WIN32)

        // FreeBSD (and anything else): not implemented yet -- FreeBSD's cpuset_t API lives under
        // <pthread_np.h>/<sys/cpuset.h> and differs enough from Linux's that it needs its own verified
        // implementation rather than a guess, same stance already taken for
        // DedicatedThread::pin_to_core in Async/src/AffinityImpl.cpp. Every core reports whatever the
        // unpinned calling thread's cpuid happens to answer -- honestly degraded, not fabricated.
        [[nodiscard]] std::vector<u32> enumerate_cores() noexcept {
            std::vector<u32> cores;
            const unsigned int count = std::thread::hardware_concurrency();
            for (u32 i = 0; i < (count > 0 ? count : 1u); ++i) {
                cores.push_back(i);
            }
            return cores;
        }

        class ScopedAffinityRestore {};

        bool pin_current_thread(u32 /*core_index*/) noexcept { return false; }

    #endif // platform dispatch (Windows handled inline in build_cores() below -- see its own comment)

        [[nodiscard]] std::vector<CoreCapabilities> build_cores() noexcept {
    #if defined(_WIN32)
            // Windows has no "get current affinity mask" query -- SetThreadAffinityMask conveniently
            // *returns* the previous mask as a side effect of the first successful call, which is the
            // only way to recover it, so this platform can't share the RAII-around-a-separate-get-call
            // shape the other platforms use above.
            std::vector<u32> logical_cores;
            DWORD_PTR process_mask = 0, system_mask = 0;
            if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
                for (u32 i = 0; i < sizeof(DWORD_PTR) * 8; ++i) {
                    if ((process_mask >> i) & DWORD_PTR{1}) {
                        logical_cores.push_back(i);
                    }
                }
            }
            if (logical_cores.empty()) {
                SYSTEM_INFO info{};
                GetSystemInfo(&info);
                const u32 count = info.dwNumberOfProcessors > 0 ? info.dwNumberOfProcessors : 1u;
                for (u32 i = 0; i < count; ++i) {
                    logical_cores.push_back(i);
                }
            }

            std::vector<CoreCapabilities> result;
            result.reserve(logical_cores.size());
            DWORD_PTR original_mask = 0;
            bool have_original = false;
            for (const u32 core_index : logical_cores) {
                const DWORD_PTR previous = SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << core_index);
                if (previous != 0 && !have_original) {
                    original_mask = previous;
                    have_original = true;
                }
                result.push_back(detect_current_core());
            }
            if (have_original) {
                SetThreadAffinityMask(GetCurrentThread(), original_mask);
            }
            if (result.empty()) {
                result.push_back(detect_current_core());
            }
            return result;
    #else
            const std::vector<u32> logical_cores = enumerate_cores();
            if (logical_cores.empty()) {
                return {detect_current_core()};
            }

            std::vector<CoreCapabilities> result;
            result.reserve(logical_cores.size());
            {
                ScopedAffinityRestore restore_guard;
                for (const u32 core_index : logical_cores) {
                    pin_current_thread(core_index);
                    result.push_back(detect_current_core());
                }
            }
            return result;
    #endif
        }

#else // !STURDY_CPU_X86

        // Arm64/RISC-V/other: no per-core divergence is knowable here -- there is no per-core HWCAP
        // query on Linux, and no `cpuid`-equivalent instruction on either architecture at all (see
        // Extensions.hpp's header comment). Every core gets the single process-wide `Cpu::features()`
        // result; cache sizes are left at 0 rather than guessed.
        [[nodiscard]] CoreCapabilities uniform_capabilities() noexcept {
            CoreCapabilities caps{};
            caps.extensions.assign(static_cast<usize>(Extension::Count), false);
            const CpuFeatures &f = Cpu::features();
    #if defined(__aarch64__) || defined(_M_ARM64)
            caps.extensions[static_cast<usize>(Extension::ARM_NEON)] = f.neon;
            caps.extensions[static_cast<usize>(Extension::ARM_SVE)] = f.sve;
    #elif defined(__riscv) && (__riscv_xlen == 64)
            caps.extensions[static_cast<usize>(Extension::RISCV_V)] = f.rvv;
    #endif
            return caps;
        }

        [[nodiscard]] std::vector<CoreCapabilities> build_cores() noexcept {
            const unsigned int count = std::thread::hardware_concurrency();
            return std::vector<CoreCapabilities>(count > 0 ? count : 1u, uniform_capabilities());
        }

#endif // STURDY_CPU_X86

    } // namespace

    CoreMap::CoreMap() : cores_(build_cores()) {
        if (cores_.empty()) {
            cores_.push_back(CoreCapabilities{}); // core_count() >= 1 is a documented invariant
        }

        type_of_core_.resize(cores_.size());
        for (usize i = 0; i < cores_.size(); ++i) {
            usize type_index = cores_of_type_.size();
            bool found = false;
            for (usize t = 0; t < cores_of_type_.size(); ++t) {
                if (cores_[cores_of_type_[t].front()] == cores_[i]) {
                    type_index = t;
                    found = true;
                    break;
                }
            }
            if (!found) {
                cores_of_type_.emplace_back();
            }
            cores_of_type_[type_index].push_back(i);
            type_of_core_[i] = type_index;
        }

        index_of_x2apic_id_.reserve(cores_.size());
        for (usize i = 0; i < cores_.size(); ++i) {
            index_of_x2apic_id_.emplace_back(cores_[i].x2apic_id, i);
        }
        std::sort(index_of_x2apic_id_.begin(), index_of_x2apic_id_.end(),
                  [](const std::pair<u32, usize> &a, const std::pair<u32, usize> &b) noexcept {
                      return a.first < b.first;
                  });
    }

    const CoreMap &CoreMap::instance() noexcept {
        static const CoreMap instance;
        return instance;
    }

    const CoreCapabilities *CoreMap::capabilities_of_current_core() const noexcept {
        const u32 id = current_core().x2apic_id;
        const auto it = std::lower_bound(
            index_of_x2apic_id_.begin(),
            index_of_x2apic_id_.end(),
            id,
            [](const std::pair<u32, usize> &entry, u32 value) noexcept { return entry.first < value; });
        if (it == index_of_x2apic_id_.end() || it->first != id) {
            return nullptr;
        }
        return &cores_[it->second];
    }

} // namespace SFT::Foundation::Cpu
