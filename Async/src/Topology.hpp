#pragma once

#include <cstdint>
#include <thread>
#include <vector>

namespace SFT::Async {

    using u32 = std::uint32_t;

    // Logical core indices (as understood by Foundation::Cpu::CoreMap / pin_thread_to_core below),
    // best-first. Ranking key: `CoreType` (Performance > Unknown > Efficiency) first, then L3 size
    // descending, then L2 size descending. One ranking captures both signals a hybrid machine can
    // expose: ISA-heterogeneous designs (Intel P/E -- `CoreType` differs) and cache-only-heterogeneous
    // ones (AMD multi-CCD X3D -- `CoreType` stays Unknown on every core, but L3 size differs between
    // CCDs). On a uniform machine every core ties, so the returned order is simply core index order.
    //
    // Empty only if Foundation::Cpu::CoreMap somehow reports zero cores (it never does -- core_count()
    // >= 1 is a documented invariant there) -- callers should still treat an empty result as "topology
    // unavailable, don't pin anything" defensively.
    [[nodiscard]] std::vector<u32> ranked_logical_cores() noexcept;

    // Pins `thread` to logical `core_index`. Shared by DedicatedThread::pin_to_core (Affinity.hpp) and
    // Scheduler's own worker threads (SchedulerImpl.cpp) so the platform-`#ifdef` body exists exactly
    // once. Best-effort: returns false (and leaves the thread's affinity unchanged) on platforms/cases
    // where pinning isn't available -- see TopologyImpl.cpp for the per-platform caveats.
    [[nodiscard]] bool pin_thread_to_core(std::thread &thread, u32 core_index) noexcept;

} // namespace SFT::Async
