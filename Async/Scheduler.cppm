module;

#pragma region Imports
#include <memory>
#include <type_traits>
#include <utility>
#pragma endregion

export module Sturdy.Async:Scheduler;

import Sturdy.Foundation;
import :Task;

using std::invoke_result_t;
using std::make_shared;
using std::make_unique;
using std::unique_ptr;

export namespace SFT::Async {

    // A work-stealing thread pool. Each worker owns a local deque of tasks: it pushes/pops its own
    // work from the *back* (LIFO — depth-first, so a task that spawns subtasks keeps working on the
    // freshest one, which is usually still hot in cache) and, when its own deque runs dry, steals from
    // the *front* of another worker's deque (FIFO — oldest task first, which tends to be the biggest
    // remaining piece of work, for better load balancing). `spawn()` called from a non-worker thread
    // (e.g. the main thread) lands in a shared injector queue that every idle worker also checks.
    //
    // Lazily started on first use — just call `spawn()`; the pool exists after that. Call
    // `initialize()` yourself first only if you want a specific worker count.
    class Scheduler {
      public:
        // Starts `worker_count` worker threads (0 => `hardware_concurrency() - 1`, minimum 1). A
        // no-op if the pool is already running.
        static void initialize(u32 worker_count = 0) noexcept;

        // Signals every worker to stop after finishing its *current* task and joins them. Tasks still
        // sitting in a deque/the injector queue when this is called are discarded, not drained — call
        // it once you know no more work is coming (e.g. process shutdown), not mid-workload. Safe to
        // call from any thread except a worker thread itself; a no-op if not running.
        static void shutdown() noexcept;

        [[nodiscard]] static bool is_running() noexcept;

        // Number of worker threads currently running (0 if the pool hasn't started).
        [[nodiscard]] static u32 worker_count() noexcept;

        // Schedules `fn` (any no-argument callable) to run on a worker thread and returns a
        // `TaskHandle` to observe or await it. Callable from any thread, including from inside
        // another task — spawning from within a running task places the new task on the *calling*
        // worker's own deque rather than redistributing it, for cache locality.
        template <typename F>
        [[nodiscard]] static auto spawn(F &&fn) {
            using R = invoke_result_t<std::decay_t<F>>;
            auto state = make_shared<Detail::TaskState<R>>();
            auto task = make_unique<Detail::ConcreteTask<std::decay_t<F>, R>>(std::forward<F>(fn), state);
            enqueue(std::move(task));
            return TaskHandle<R>(std::move(state));
        }

      private:
        static void enqueue(unique_ptr<Detail::TaskBase> task) noexcept;
    };

} // namespace SFT::Async
