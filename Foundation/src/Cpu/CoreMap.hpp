#pragma once

#include <Foundation/src/Cpu/CpuTopology.hpp>
#include <Foundation/src/Cpu/Extensions.hpp>
#include <Foundation/src/Types.hpp>

#include <utility>
#include <vector>

namespace SFT::Foundation::Cpu {

    // Everything known about one logical core: its extension support (a `vector<bool>` indexed by
    // `Extension`, per the project's explicit storage-mechanism requirement) plus cache sizes plus
    // `CoreType` (CpuTopology.hpp). Cache size and `type` are both part of a core's identity here on
    // purpose: a cache-asymmetric design (e.g. AMD's X3D parts, where half the cores get a 3D V-Cache
    // die with much larger L3 and half don't) has zero ISA difference between its core types, so
    // `extensions` alone can't distinguish them, but `l3_bytes` can. Conversely `type` distinguishes
    // Intel P/E cores even in the (largely theoretical, given Intel's documented ISA-parity policy)
    // case where their cache sizes happen to coincide.
    struct CoreCapabilities {
        std::vector<bool> extensions; // size == static_cast<usize>(Extension::Count)
        usize l1d_bytes = 0;
        usize l1i_bytes = 0;
        usize l2_bytes = 0;
        usize l3_bytes = 0;              // the L3 slice/shared cache visible from this core -- the X3D signal
        u32 x2apic_id = 0;               // topology identity (CpuTopology.hpp); 0 where unavailable
        CoreType type = CoreType::Unknown; // P/E classification (CpuTopology.hpp); Unknown on non-Intel/uniform

        [[nodiscard]] bool has(Extension extension) const noexcept {
            const auto index = static_cast<usize>(extension);
            return index < extensions.size() && extensions[index];
        }
    };

    // Two cores are the same "type" iff every field here matches — same extension support, same cache
    // sizes, and same `CoreType` (x2apic_id is identity, not type, and is deliberately excluded). This
    // is what makes X3D-style cache-only asymmetry register as more than one type without needing a
    // dedicated enum value for every possible future kind of core asymmetry.
    [[nodiscard]] bool operator==(const CoreCapabilities &a, const CoreCapabilities &b) noexcept;

    // Per-core view of the whole machine, built once by pinning the calling thread to each logical
    // core in turn and reading its capabilities directly (verified on Windows/x86 — see CoreMap.cpp for
    // the per-platform caveats this can't verify from here). Expensive relative to `Cpu::features()`
    // (CpuId.hpp): building it temporarily changes the calling thread's affinity `core_count()` times.
    // Deliberately kept separate from `Cpu::features()`/`SimdMath` (CpuId.hpp/SimdMath.hpp), which stay
    // cheap, current-core-only, and unaffected by this existing at all.
    class CoreMap {
      public:
        // Built once, lazily, on first use (a function-local static, thread-safe per the standard).
        [[nodiscard]] static const CoreMap &instance() noexcept;

        [[nodiscard]] usize core_count() const noexcept { return cores_.size(); }
        [[nodiscard]] const CoreCapabilities &core(usize logical_index) const noexcept { return cores_.at(logical_index); }

        // What the logical processor *this call* is executing on can do, right now. Looks up
        // `Cpu::current_core()` (CpuTopology.hpp; reads fresh via `cpuid` every call) against the
        // x2apic_id -> index map built once at construction. A task that migrated between workers
        // between two calls gets a correctly different answer each time — this is deliberately not
        // cached per-call the way `cores_` itself is cached once. Returns `nullptr` on the rare machine
        // where the running core's x2apic_id wasn't part of the set enumerated at construction (e.g. a
        // CPU hot-plug after startup).
        [[nodiscard]] const CoreCapabilities *capabilities_of_current_core() const noexcept;

        // Number of distinct core types present (by `CoreCapabilities` equality — see `operator==`
        // above). 1 on a uniform machine; >1 on any hybrid design, ISA-heterogeneous (Intel P/E) or
        // cache-only-heterogeneous (AMD X3D) alike.
        [[nodiscard]] usize distinct_type_count() const noexcept { return cores_of_type_.size(); }
        [[nodiscard]] bool is_hybrid() const noexcept { return distinct_type_count() > 1; }

        [[nodiscard]] usize type_index_of_core(usize logical_index) const noexcept { return type_of_core_.at(logical_index); }
        // Logical core indices (as usable with `core()` above) belonging to the given type index, in
        // `[0, distinct_type_count())`.
        [[nodiscard]] const std::vector<usize> &core_indices_of_type(usize type_index) const noexcept {
            return cores_of_type_.at(type_index);
        }

      private:
        CoreMap();

        std::vector<CoreCapabilities> cores_;
        std::vector<usize> type_of_core_;              // core index -> type index
        std::vector<std::vector<usize>> cores_of_type_; // type index -> core indices
        std::vector<std::pair<u32, usize>> index_of_x2apic_id_; // sorted by x2apic_id; linear-scan-sized
    };

} // namespace SFT::Foundation::Cpu
