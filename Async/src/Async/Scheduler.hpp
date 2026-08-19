#pragma once

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

#include <Async/Task.hpp>

namespace SFT::Async {

    using u32 = std::uint32_t;
    using i32 = std::int32_t;
    using usize = std::size_t;

    struct SchedulerConfig {
        u32 worker_count = 0;


        u32 idle_spin_iterations = 256;
        u32 idle_yield_iterations = 64;
        u32 idle_sleep_microseconds = 100;
        bool notify_all_on_enqueue = false;
    };


    enum class TaskWeight {
        Light,
        Heavy,
    };

    class Scheduler {
      public:
        /// Initializes the `Scheduler` for use.
        ///
        /// @param worker_count Number of elements or operations to process.
        ///
        /// @note This function does not throw exceptions.
        static void initialize(u32 worker_count = 0) noexcept;
        /// Initializes the `Scheduler` for use.
        ///
        /// @param config Configuration values controlling the operation.
        ///
        /// @note This function does not throw exceptions.
        static void initialize(const SchedulerConfig &config) noexcept;
        /// Initializes low latency for use.
        ///
        /// @param worker_count Number of elements or operations to process.
        ///
        /// @note This function does not throw exceptions.
        static void initialize_low_latency(u32 worker_count = 0) noexcept;
        /// Shuts down the `Scheduler` and releases associated runtime state.
        ///
        /// @note This function does not throw exceptions.
        static void shutdown() noexcept;
        /// Reports whether running holds for this `Scheduler`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static bool is_running() noexcept;
        /// Reports whether worker thread holds for this `Scheduler`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static bool is_worker_thread() noexcept;
        /// Returns the worker count for this `Scheduler`.
        ///
        /// @return Returns the current worker count value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static u32 worker_count() noexcept;

        /// Spawns the supplied asynchronous work.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename F>
        [[nodiscard]] static auto spawn(F &&fn, TaskWeight weight = TaskWeight::Light) {
            using R = std::invoke_result_t<std::decay_t<F>>;
            auto state = std::make_shared<Detail::TaskState<R>>();
            auto task = std::make_unique<Detail::ConcreteTask<std::decay_t<F>, R>>(std::forward<F>(fn), state);
            enqueue(std::move(task), weight);
            return TaskHandle<R>(std::move(state));
        }

      private:
        /// Performs the enqueue operation for `Scheduler` using the supplied arguments.
        ///
        /// @param task Task used or affected by the operation.
        /// @param weight `weight` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        static void enqueue(std::unique_ptr<Detail::TaskBase> task, TaskWeight weight) noexcept;
    };

} // namespace SFT::Async
