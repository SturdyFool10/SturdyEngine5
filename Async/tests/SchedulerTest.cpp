#include <Async/src/ParIter.hpp>
#include <Async/src/Scheduler.hpp>
#include <Async/src/Runtime.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

namespace {

    bool check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }

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

} // namespace

int main() {
    const bool passed = single_worker_nested_wait() &&
                        single_worker_nested_parallel_reduce() &&
                        worker_wait_wakes_for_late_dependency_work() &&
                        saturated_pool_nested_waits();
    if (passed) {
        std::cout << "Async scheduler nested-wait tests passed.\n";
        return 0;
    }
    return 1;
}
