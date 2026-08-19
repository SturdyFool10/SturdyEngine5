#include <Async/Topology.hpp>

#include <Foundation/Cpu/CoreMap.hpp>
#include <Foundation/Cpu/CpuTopology.hpp>

#include <algorithm>

#if !defined(STURDY_PLATFORM_WEB)
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
#endif
#endif

namespace SFT::Async {

    /// Returns the current or globally available ranked logical cores value.
    ///
    /// @return Returns the current ranked logical cores value.
    /// @note This function does not throw exceptions.
    std::vector<u32> ranked_logical_cores() noexcept {
        const Foundation::Cpu::CoreMap &map = Foundation::Cpu::CoreMap::instance();

        std::vector<u32> cores(map.core_count());
        for (Foundation::usize i = 0; i < cores.size(); ++i) {
            cores[i] = static_cast<u32>(i);
        }


        const auto rank_of_type = [](Foundation::Cpu::CoreType type) noexcept -> int {
            switch (type) {
                case Foundation::Cpu::CoreType::Performance: return 2;
                case Foundation::Cpu::CoreType::Unknown: return 1;
                case Foundation::Cpu::CoreType::Efficiency: return 0;
            }
            return 1;
        };

        std::stable_sort(cores.begin(), cores.end(), [&](u32 a, u32 b) noexcept {
            const Foundation::Cpu::CoreCapabilities &ca = map.core(a);
            const Foundation::Cpu::CoreCapabilities &cb = map.core(b);
            const int rank_a = rank_of_type(ca.type);
            const int rank_b = rank_of_type(cb.type);
            if (rank_a != rank_b) {
                return rank_a > rank_b;
            }
            if (ca.l3_bytes != cb.l3_bytes) {
                return ca.l3_bytes > cb.l3_bytes;
            }
            return ca.l2_bytes > cb.l2_bytes;
        });

        return cores;
    }

    /// Returns the current or globally available ranked physical cores value.
    ///
    /// @return Returns the current ranked physical cores value.
    /// @note This function does not throw exceptions.
    std::vector<u32> ranked_physical_cores() noexcept {
        const Foundation::Cpu::CoreMap &map = Foundation::Cpu::CoreMap::instance();
        const std::vector<u32> ranked_logical = ranked_logical_cores();


        std::vector<u32> physical;
        physical.reserve(map.physical_core_count());
        std::vector<bool> seen(map.physical_core_count(), false);
        for (u32 logical_index : ranked_logical) {
            const Foundation::usize physical_index = map.physical_core_of(logical_index);
            if (seen[physical_index]) {
                continue;
            }
            seen[physical_index] = true;
            physical.push_back(logical_index);
        }
        return physical;
    }

#if defined(STURDY_PLATFORM_WEB)

    /// Performs the pin thread to core operation for `Async` using the supplied arguments.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool pin_thread_to_core(std::thread &           , u32               ) noexcept { return false; }

#elif defined(_WIN32)

    /// Performs the pin thread to core operation for `Async` using the supplied arguments.
    ///
    /// @param thread Thread used or affected by the operation.
    /// @param core_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool pin_thread_to_core(std::thread &thread, u32 core_index) noexcept {
        if (!thread.joinable()) {
            return false;
        }
        const DWORD_PTR mask = DWORD_PTR{1} << core_index;
        return SetThreadAffinityMask(thread.native_handle(), mask) != 0;
    }

#elif defined(__APPLE__)


    /// Performs the pin thread to core operation for `Async` using the supplied arguments.
    ///
    /// @param thread Thread used or affected by the operation.
    /// @param core_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool pin_thread_to_core(std::thread &thread, u32 core_index) noexcept {
        if (!thread.joinable()) {
            return false;
        }
        thread_affinity_policy_data_t policy{.affinity_tag = static_cast<integer_t>(core_index)};
        const mach_port_t mach_thread = pthread_mach_thread_np(thread.native_handle());
        return thread_policy_set(mach_thread,
                                  THREAD_AFFINITY_POLICY,
                                  reinterpret_cast<thread_policy_t>(&policy),
                                  THREAD_AFFINITY_POLICY_COUNT) == KERN_SUCCESS;
    }

#elif defined(__linux__)

    /// Performs the pin thread to core operation for `Async` using the supplied arguments.
    ///
    /// @param thread Thread used or affected by the operation.
    /// @param core_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool pin_thread_to_core(std::thread &thread, u32 core_index) noexcept {
        if (!thread.joinable()) {
            return false;
        }
        cpu_set_t cpu_set;
        CPU_ZERO(&cpu_set);
        CPU_SET(static_cast<int>(core_index), &cpu_set);
        return pthread_setaffinity_np(thread.native_handle(), sizeof(cpu_set_t), &cpu_set) == 0;
    }

#else


    /// Performs the pin thread to core operation for `Async` using the supplied arguments.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool pin_thread_to_core(std::thread &           , u32               ) noexcept { return false; }

#endif

} // namespace SFT::Async
