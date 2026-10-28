#include <Async/Affinity.hpp>
#include <Async/ParIter.hpp>
#include <Async/Scheduler.hpp>
#include <Async/Runtime.hpp>
#include <Async/Topology.hpp>
#include <Foundation/Cpu/IsaSet.hpp>
#include <Foundation/Cpu/IsaScope.hpp>
#include <Foundation/Cpu/CoreMap.hpp>

#if defined(__linux__)
    #include <pthread.h>
    #include <sched.h>
#endif

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

    /// Checks the supplied condition and reports the accompanying diagnostic message when it is false.
    ///
    /// @param condition Condition controlling whether the operation proceeds.
    /// @param message Text consumed by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }

    /// Returns the current or globally available single worker nested wait value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool single_worker_nested_wait() {
        SFT::Async::Scheduler::initialize(1);
        auto parent = SFT::Async::Scheduler::spawn([] {
            auto child = SFT::Async::Scheduler::spawn([] { return 42; });
            return child.wait();
        });
        const bool passed = check(parent.wait() == 42, "single-worker child wait returned the wrong value");
        SFT::Async::Scheduler::shutdown();
        return passed;
    }

    /// Returns the current or globally available single worker nested parallel reduce value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool single_worker_nested_parallel_reduce() {
        SFT::Async::Scheduler::initialize(1);
        auto parent = SFT::Async::Scheduler::spawn([] {
            std::vector<std::uint32_t> values(4096);
            std::iota(values.begin(), values.end(), 1u);
            return (values | SFT::Async::par_iter_on<SFT::Async::ParallelRuntime>).sum();
        });
        const bool passed = check(parent.wait() == 8'390'656u, "nested parallel reduction returned the wrong sum");
        SFT::Async::Scheduler::shutdown();
        return passed;
    }

    /// Returns the current or globally available worker wait wakes for late dependency work value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool worker_wait_wakes_for_late_dependency_work() {
        SFT::Async::Scheduler::initialize(2);
        std::atomic<bool> target_started{false};
        std::atomic<bool> release_target{false};

        auto target = SFT::Async::Scheduler::spawn([&] {
            target_started.store(true, std::memory_order_release);
            target_started.notify_all();
            release_target.wait(false, std::memory_order_acquire);
        });
        target_started.wait(false, std::memory_order_acquire);

        auto waiter = SFT::Async::Scheduler::spawn([target] { target.wait(); });
        std::jthread delayed_dependency([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
            auto unblock = SFT::Async::Scheduler::spawn([&] {
                release_target.store(true, std::memory_order_release);
                release_target.notify_all();
            });
            unblock.wait();
        });

        waiter.wait();
        delayed_dependency.join();
        const bool passed = check(
            release_target.load(std::memory_order_acquire),
            "worker wait did not execute dependency work enqueued after its initial scan");
        SFT::Async::Scheduler::shutdown();
        return passed;
    }

    /// Returns the current or globally available saturated pool nested waits value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool saturated_pool_nested_waits() {
        constexpr std::uint32_t worker_count = 4;
        SFT::Async::Scheduler::initialize(worker_count);

        std::barrier parents_started(static_cast<std::ptrdiff_t>(worker_count));
        std::vector<SFT::Async::TaskHandle<std::uint32_t>> parents;
        parents.reserve(worker_count);
        for (std::uint32_t index = 0; index < worker_count; ++index) {
            parents.push_back(SFT::Async::Scheduler::spawn([index, &parents_started] {
                parents_started.arrive_and_wait();
                auto child = SFT::Async::Scheduler::spawn([index] { return index + 1; });
                return child.wait();
            }));
        }

        bool passed = true;
        for (std::uint32_t index = 0; index < worker_count; ++index) {
            passed &= check(parents[index].wait() == index + 1, "saturated-pool child wait returned the wrong value");
        }
        SFT::Async::Scheduler::shutdown();
        return passed;
    }


    /// Returns the current or globally available heavy and light tasks complete value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool heavy_and_light_tasks_complete() {
        constexpr std::uint32_t worker_count = 4;
        SFT::Async::Scheduler::initialize(worker_count);

        std::vector<SFT::Async::TaskHandle<std::uint32_t>> heavy_tasks;
        std::vector<SFT::Async::TaskHandle<std::uint32_t>> light_tasks;
        heavy_tasks.reserve(32);
        light_tasks.reserve(32);
        for (std::uint32_t index = 0; index < 32; ++index) {
            heavy_tasks.push_back(SFT::Async::Scheduler::spawn([index] { return index * 2; }, SFT::Async::TaskWeight::Heavy));
            light_tasks.push_back(SFT::Async::Scheduler::spawn([index] { return index * 3; }));
        }

        bool passed = true;
        for (std::uint32_t index = 0; index < 32; ++index) {
            passed &= check(heavy_tasks[index].wait() == index * 2, "heavy task returned the wrong value");
            passed &= check(light_tasks[index].wait() == index * 3, "light task returned the wrong value");
        }
        SFT::Async::Scheduler::shutdown();
        return passed;
    }

    /// Verifies task-scoped ISA scheduling and requirement restoration across a nested wait.
    bool task_isa_scope_survives_nested_wait() {
        using namespace SFT::Foundation::Cpu;
        IsaSet required;
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        required = required_extensions(IsaTier::X86Sse2);
#elif defined(__aarch64__) || defined(_M_ARM64)
        required = required_extensions(IsaTier::ArmNeon);
#else
        return true;
#endif
        if (!IsaScope::has_compatible_core(required)) {
            return true;
        }

        IsaSet impossible;
        for (std::size_t i = 0; i < IsaSet::extension_count; ++i) {
            impossible.add(static_cast<Extension>(i));
        }
        if (!IsaScope::has_compatible_core(impossible)) {
            bool unexpectedly_ran = false;
            auto rejected = SFT::Async::Scheduler::spawn([&] { unexpectedly_ran = true; }, impossible);
            bool wait_reported_failure = false;
            try {
                rejected.wait();
            } catch (const std::runtime_error &) {
                wait_reported_failure = true;
            }
            if (!check(rejected.failed_due_to_isa() && wait_reported_failure && !unexpectedly_ran,
                       "unavailable ISA task was not rejected clearly")) {
                return false;
            }
        }

        SFT::Async::Scheduler::initialize(1);
        auto parent = SFT::Async::Scheduler::spawn([required] {
            bool changed = current_isa_requirements().empty();
#if defined(__linux__)
            cpu_set_t original{};
            cpu_set_t actual{};
            cpu_set_t expected{};
            CPU_ZERO(&expected);
            if (pthread_getaffinity_np(pthread_self(), sizeof(original), &original) != 0) {
                return false;
            }
#endif
            IsaSet active_required = required;
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
            IsaSet avx512f;
            avx512f.add(Extension::X86_AVX512F);
            if (current_core_isa_set().contains(avx512f) && IsaScope::try_set_requirements(avx512f)) {
                active_required = avx512f;
            } else {
                const IsaSet avx2 = required_extensions(IsaTier::X86Avx2);
                if (current_core_isa_set().contains(avx2) && IsaScope::try_set_requirements(avx2)) {
                    active_required = avx2;
                }
            }
#endif
            if (active_required == required || !current_isa_requirements().contains(active_required)) {
                changed &= IsaScope::try_set_requirements(required);
                active_required = required;
            }
            changed &= current_isa_requirements() == active_required;
#if defined(__linux__)
            if (pthread_getaffinity_np(pthread_self(), sizeof(actual), &actual) != 0) {
                return false;
            }
            const auto &map = CoreMap::instance();
            for (SFT::Foundation::usize i = 0; i < map.core_count(); ++i) {
                const auto &core = map.core(i);
                if (core.logical_cpu < CPU_SETSIZE &&
                    CPU_ISSET(static_cast<int>(core.logical_cpu), &original) && core.supports_isa(active_required)) {
                    CPU_SET(static_cast<int>(core.logical_cpu), &expected);
                }
            }
            changed &= CPU_COUNT(&actual) > 0 && CPU_EQUAL(&actual, &expected);
#endif
            auto child = SFT::Async::Scheduler::spawn(
                [active_required] { return current_isa_requirements() == active_required; }, active_required);
            const bool child_ran = child.wait();
            const bool resumed = current_isa_requirements() == active_required;
            const bool returned_to_scalar = IsaScope::try_set_requirements(IsaSet{}) &&
                                            current_isa_requirements().empty();
#if defined(__linux__)
            cpu_set_t restored{};
            const bool affinity_restored = pthread_getaffinity_np(pthread_self(), sizeof(restored), &restored) == 0 &&
                                           CPU_EQUAL(&restored, &original);
#else
            const bool affinity_restored = true;
#endif
            return changed && child_ran && resumed && returned_to_scalar && affinity_restored;
        });
        const bool passed = check(parent.wait(), "dynamic ISA change or nested wait/resume failed");
        SFT::Async::Scheduler::shutdown();
        return passed;
    }

    /// Verifies dynamic ISA declarations in synchronous, dedicated-thread, main-thread, and inline-loop paths.
    bool isa_requirements_cover_non_scheduler_execution_paths() {
        using namespace SFT::Foundation::Cpu;
        IsaSet required;
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        required = required_extensions(IsaTier::X86Sse2);
#elif defined(__aarch64__) || defined(_M_ARM64)
        required = required_extensions(IsaTier::ArmNeon);
#else
        return true;
#endif
        if (!IsaScope::has_compatible_core(required)) {
            return true;
        }
        const auto dynamic_body = [required] {
            if (!current_isa_requirements().empty() || !current_core_isa_set().contains(required) ||
                !IsaScope::try_set_requirements(required) || current_isa_requirements() != required) {
                return false;
            }
            return IsaScope::try_set_requirements(IsaSet{}) && current_isa_requirements().empty();
        };

        auto synchronous = SFT::Async::SynchronousRuntime::spawn(dynamic_body);
        if (!check(synchronous.wait(), "SynchronousRuntime callback did not receive a dynamic ISA scope")) {
            return false;
        }

        SFT::Async::DedicatedThread dedicated("ISA audit");
        auto dedicated_dynamic = dedicated.run(dynamic_body);
        auto dedicated_declared = dedicated.run([required] {
            return current_isa_requirements() == required &&
                   IsaScope::try_set_requirements(IsaSet{}) && current_isa_requirements().empty();
        }, required);
        if (!check(dedicated_dynamic.wait() && dedicated_declared.wait(),
                   "DedicatedThread did not enforce or expose task ISA requirements")) {
            return false;
        }

        bool main_thread_ok = false;
        SFT::Async::ParallelRuntime::run_on_main_thread([&] { main_thread_ok = dynamic_body(); });
        SFT::Async::ParallelRuntime::pump_main_thread();
        if (!check(main_thread_ok, "main-thread callback did not receive a dynamic ISA scope")) {
            return false;
        }

        bool inline_loop_ok = false;
        SFT::Async::parallel_for_erased(
            1, 1, [](std::size_t, std::size_t, void *data) {
                auto *result = static_cast<bool *>(data);
                using namespace SFT::Foundation::Cpu;
                IsaSet required_set = required_extensions(IsaTier::X86Sse2);
#if defined(__aarch64__) || defined(_M_ARM64)
                required_set = required_extensions(IsaTier::ArmNeon);
#endif
                *result = current_isa_requirements().empty() && current_core_isa_set().contains(required_set) &&
                          IsaScope::try_set_requirements(required_set) &&
                          IsaScope::try_set_requirements(IsaSet{});
            },
            &inline_loop_ok);
        return check(inline_loop_ok, "inline parallel_for_erased callback bypassed the ISA scope");
    }

    /// Verifies a worker stealing an ISA-constrained task from another worker's deque.
    bool isa_requirements_are_honored_when_stealing() {
        using namespace SFT::Foundation::Cpu;
        IsaSet required;
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        required = required_extensions(IsaTier::X86Sse2);
#elif defined(__aarch64__) || defined(_M_ARM64)
        required = required_extensions(IsaTier::ArmNeon);
#else
        return true;
#endif
        if (!IsaScope::has_compatible_core(required)) {
            return true;
        }

        SFT::Async::Scheduler::initialize(2);
        std::mutex mutex;
        std::condition_variable cv;
        bool child_started = false;
        bool parent_waiting = false;
        bool release_parent = false;
        std::thread::id parent_thread;
        std::thread::id child_thread;

        auto parent = SFT::Async::Scheduler::spawn([&] {
            parent_thread = std::this_thread::get_id();
            auto child = SFT::Async::Scheduler::spawn([&] {
                const bool entered = current_isa_requirements() == required;
                {
                    std::lock_guard lock(mutex);
                    child_thread = std::this_thread::get_id();
                    child_started = true;
                }
                cv.notify_all();
                return entered;
            }, required);
            {
                std::unique_lock lock(mutex);
                parent_waiting = true;
                cv.notify_all();
                cv.wait(lock, [&] { return release_parent; });
            }
            return child.wait() && child_thread != parent_thread;
        });

        bool stole_while_parent_waited = false;
        {
            std::unique_lock lock(mutex);
            stole_while_parent_waited = cv.wait_for(lock, std::chrono::seconds(2), [&] {
                return child_started && parent_waiting;
            });
            release_parent = true;
        }
        cv.notify_all();
        const bool child_result = parent.wait();
        SFT::Async::Scheduler::shutdown();
        return check(stole_while_parent_waited && child_result,
                     "work stealing ran a task without its ISA requirements or on its owner worker");
    }

    /// Covers the type-erased parallel loop and the native-handle pinning overloads.
    bool erased_parallel_for_and_native_pinning() {
        SFT::Async::Scheduler::initialize(4);
        constexpr std::size_t count = 10007;
        std::vector<std::atomic<int>> hits(count);
        SFT::Async::parallel_for_erased(
            count, 64,
            [](std::size_t begin, std::size_t end, void *user) {
                auto *slots = static_cast<std::vector<std::atomic<int>> *>(user);
                for (std::size_t index = begin; index < end; ++index) {
                    (*slots)[index].fetch_add(1);
                }
            },
            &hits);
        bool passed = true;
        for (std::size_t index = 0; index < count; ++index) {
            if (hits[index].load() != 1) {
                passed &= check(false, "parallel_for_erased visited an index other than exactly once");
                break;
            }
        }

        // Automatic chunking and a range smaller than one chunk.
        std::atomic<int> total{0};
        SFT::Async::parallel_for_erased(
            100, 0, [](std::size_t begin, std::size_t end, void *user) { static_cast<std::atomic<int> *>(user)->fetch_add(static_cast<int>(end - begin)); },
            &total);
        passed &= check(total.load() == 100, "automatic chunking must cover the whole range");
        SFT::Async::Scheduler::shutdown();

        // Pinning a thread the caller did not create through std::thread. Failure is acceptable in a
        // restricted environment, so only the call shape is asserted.
        (void)SFT::Async::pin_current_thread_to_core(0);
        std::thread worker([] {});
        (void)SFT::Async::pin_native_thread_to_core(worker.native_handle(), 0);
        worker.join();
        return passed;
    }

} // namespace

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    const bool passed = single_worker_nested_wait() &&
                        single_worker_nested_parallel_reduce() &&
                        worker_wait_wakes_for_late_dependency_work() &&
                        saturated_pool_nested_waits() &&
                        heavy_and_light_tasks_complete() &&
                        task_isa_scope_survives_nested_wait() &&
                        isa_requirements_cover_non_scheduler_execution_paths() &&
                        isa_requirements_are_honored_when_stealing() &&
                        erased_parallel_for_and_native_pinning();
    if (passed) {
        std::cout << "Async scheduler nested-wait tests passed.\n";
        return 0;
    }
    return 1;
}
