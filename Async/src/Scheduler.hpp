#pragma once

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

#include <Async/src/Task.hpp>

namespace SFT::Async {

    using u32 = std::uint32_t;
    using i32 = std::int32_t;
    using usize = std::size_t;

    struct SchedulerConfig {
        u32 worker_count = 0;
        // Low-latency workers briefly spin/yield before sleeping so short task bursts avoid a full
        // kernel wakeup round trip. Keep defaults conservative; games can tune explicitly at startup.
        u32 idle_spin_iterations = 256;
        u32 idle_yield_iterations = 64;
        u32 idle_sleep_microseconds = 100;
        bool notify_all_on_enqueue = false;
    };

    // A task's placement preference, not a placement guarantee: workers are still free to steal any
    // task off any deque regardless of weight (see SchedulerImpl.cpp's enqueue()/try_take_task()) — a
    // `Heavy` task that lands on a busy fast core can still be picked up by whichever worker goes idle
    // first. `Light` (the default) is byte-for-byte today's existing behavior for every call site that
    // doesn't pass this explicitly.
    enum class TaskWeight {
        Light,
        Heavy,
    };

    class Scheduler {
      public:
        static void initialize(u32 worker_count = 0) noexcept;
        static void initialize(const SchedulerConfig &config) noexcept;
        static void initialize_low_latency(u32 worker_count = 0) noexcept;
        static void shutdown() noexcept;
        [[nodiscard]] static bool is_running() noexcept;
        [[nodiscard]] static bool is_worker_thread() noexcept;
        [[nodiscard]] static u32 worker_count() noexcept;

        template <typename F>
        [[nodiscard]] static auto spawn(F &&fn, TaskWeight weight = TaskWeight::Light) {
            using R = std::invoke_result_t<std::decay_t<F>>;
            auto state = std::make_shared<Detail::TaskState<R>>();
            auto task = std::make_unique<Detail::ConcreteTask<std::decay_t<F>, R>>(std::forward<F>(fn), state);
            enqueue(std::move(task), weight);
            return TaskHandle<R>(std::move(state));
        }

      private:
        static void enqueue(std::unique_ptr<Detail::TaskBase> task, TaskWeight weight) noexcept;
    };

} // namespace SFT::Async
