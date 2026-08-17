#pragma once

#include <Foundation/src/Types.hpp>

#include <string_view>

namespace SFT::Foundation::Cpu {

    /// Every vector/ISA extension this module detects or dispatches on, across every architecture this
    /// project builds for. All `false`/default on an architecture that doesn't define a given field
    /// (e.g. `avx2` on Arm64) — callers can read these unconditionally rather than `#ifdef` around every
    /// use site.
    ///
    /// x86 fields are detected via the CPU's own `cpuid` instruction (through the compiler's
    /// `<cpuid.h>`/`<immintrin.h>` builtins, identical across every compiler/OS this project targets)
    /// rather than any OS-provided "what does this CPU support" API. Arm64 and RISC-V do not have an
    /// equivalent user-mode discovery instruction — see the `neon`/`sve`/`rvv` comments below for what
    /// that means for each.
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

        /// Leaf 7 subleaf 0 EDX bit 15: the CPU package mixes core types (Intel P/E-core, ...). Only
        /// says the package is heterogeneous, not which core type is executing right now — see
        /// `Cpu::current_core()` in CpuTopology.hpp for that.
        bool hybrid = false;

        /// OS has enabled the wider register state via XSAVE/XGETBV. A CPUID feature bit alone does not
        /// mean the OS will actually preserve e.g. %zmm registers across a context switch — code that
        /// trusts the raw `avx`/`avx512f` bits without checking this can pass on the developer's machine
        /// and `SIGILL`/fault under a leaner OS or hypervisor. `best_simd_level()` below already applies
        /// this; check it directly only if you're bypassing that helper.
        bool os_supports_avx = false;
        bool os_supports_avx512 = false;

        /// --- Arm64 ---------------------------------------------------------------------------
        /// Advanced SIMD (NEON) is mandatory baseline on every AArch64 core ever shipped — there is
        /// nothing to detect, so this is simply `true` whenever compiled for Arm64, unconditionally.
        bool neon = false;
        /// SVE/SVE2 (scalable, 128-2048 bit) *is* optional, but unlike NEON there is no way to check for
        /// it without OS mediation (its presence bit lives in ID_AA64PFR0_EL1, an EL1+-only system
        /// register): Linux exposes it via `getauxval(AT_HWCAP)`, macOS via `sysctlbyname`, Windows via
        /// `IsProcessorFeaturePresent`. Wiring up three OS-specific paths for one still-rare extension,
        /// none of which this environment can compile-check (see CpuId.cpp), is deliberately left undone
        /// for now — always `false`. `neon` above already covers "the safe, always-present Arm64 tier".
        bool sve = false;

        /// --- RISC-V --------------------------------------------------------------------------
        /// The "V" vector extension is optional, and RISC-V (like Arm) has no user-mode discovery
        /// instruction — `misa` is M-mode-only. Detected on Linux via `getauxval(AT_HWCAP)`'s per-letter
        /// extension bits (the standard, kernel-documented mechanism since the 'V' bit landed); `false`
        /// on any other OS or if detection fails. See CpuId.cpp for why this path is unverified here.
        bool rvv = false;

        char vendor[13] = {};
        char brand[49] = {};

        [[nodiscard]] std::string_view vendor_view() const noexcept;
        [[nodiscard]] std::string_view brand_view() const noexcept;
    };

    /// Coarse SIMD tiers `SimdMath` (SimdMath.hpp) dispatches on. Values are architecture-specific tags,
    /// not a universal width ordering: an x86 build only ever produces `Scalar`/`AVX`/`AVX2`/`AVX512`, an
    /// Arm64 build only ever produces `Scalar`/`NEON`, a RISC-V build only ever produces
    /// `Scalar`/`RVV` — comparing e.g. `NEON` and `AVX2` for "which is wider" has no defined meaning.
    enum class SimdLevel {
        Scalar,
        AVX,
        AVX2,
        AVX512,
        NEON,
        RVV,
    };

    /// The detected feature set for this process, computed once on first use (a function-local static —
    /// the standard already guarantees this initialization is thread-safe and happens exactly once, so
    /// there is no hand-rolled locking here) and reused for the life of the process. `cpuid` reports
    /// ISA *support*, which is uniform across every logical processor on every shipping x86 design (see
    /// CpuTopology.hpp's header comment for the hybrid-core nuance), so a single process-wide snapshot
    /// is correct even on a hybrid CPU.
    [[nodiscard]] const CpuFeatures &features() noexcept;

    /// The widest `SimdMath` tier safe to use everywhere in this process right now: gated on both the
    /// CPUID feature bits and the OS-XSAVE check above.
    [[nodiscard]] SimdLevel best_simd_level() noexcept;

} // namespace SFT::Foundation::Cpu
