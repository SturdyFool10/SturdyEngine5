#pragma once

#include <Foundation/src/Types.hpp>

namespace SFT::Foundation::Cpu {

    /// A hybrid x86 package (Intel P/E-core, ...) still exposes one uniform instruction set across
    /// every core on every shipping design to date — Intel ships identical CPUID feature bits on P- and
    /// E-cores specifically so ISA-dispatch code doesn't need per-core awareness (early Alder Lake
    /// engineering samples that fused AVX-512 off only on P-cores never shipped retail for exactly this
    /// reason). `Cpu::features()`/`best_simd_level()` (CpuId.hpp) are therefore correct as one
    /// process-wide snapshot even on a hybrid CPU. `CoreType` below is for scheduling/affinity
    /// decisions ("prefer a Performance core for this latency-sensitive task"), not ISA safety.
    enum class CoreType {
        Unknown,
        Efficiency,
        Performance,
    };

    struct CurrentCore {
        /// Topology identity of the logical processor executing this call (x2APIC ID). Not a small
        /// sequential index — treat it as an opaque identity for comparison, not an array subscript.
        u32 x2apic_id = 0;
        CoreType type = CoreType::Unknown;
    };

    /// Reports the logical processor *this call* is executing on, by reading `cpuid` fresh every time —
    /// deliberately uncached, since the OS scheduler can migrate this thread between two calls, and
    /// `cpuid` always answers for whichever core is currently running it. The instruction itself costs
    /// tens of cycles with no syscall, so this is cheap enough to call per hot-path decision if needed.
    ///
    /// `CoreType` is currently decoded from CPUID leaf 0x1A, which only GenuineIntel defines; every
    /// other vendor (including hybrid AMD designs) reports `CoreType::Unknown` here rather than guess at
    /// an unverified bit layout — see CpuTopology.cpp.
    [[nodiscard]] CurrentCore current_core() noexcept;

} // namespace SFT::Foundation::Cpu
