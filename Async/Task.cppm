module;

#pragma region Imports
#include <atomic>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#pragma endregion

export module Sturdy.Async:Task;

using std::atomic;
using std::conditional_t;
using std::invoke_result_t;
using std::is_void_v;
using std::optional;
using std::shared_ptr;

export namespace SFT::Async {

    namespace Detail {

        // Type-erased base for one schedulable unit of work. Owned via `unique_ptr` by whichever
        // deque/queue currently holds it (a worker's local deque, the injector queue, or the
        // main-thread queue — see :Scheduler and :MainThread), and `execute()` is called exactly
        // once, from whichever thread pulls it out.
        //
        // `execute()` is `noexcept` on purpose: a task that lets an exception escape has no sensible
        // receiver on a worker thread, so it terminates the process (the same tradeoff `std::thread`
        // itself makes) rather than being silently swallowed. Spawned callables should report failure
        // through their return value, not by throwing.
        class TaskBase {
          public:
            TaskBase() = default;
            virtual ~TaskBase() = default;
            TaskBase(const TaskBase &) = delete;
            TaskBase &operator=(const TaskBase &) = delete;
            TaskBase(TaskBase &&) = delete;
            TaskBase &operator=(TaskBase &&) = delete;

            virtual void execute() noexcept = 0;
        };

        // Placeholder result storage for a `void`-returning task — keeps `TaskState<void>` from
        // needing to special-case "no result" everywhere it touches `result`.
        struct NoResult {};

        // Shared completion state for one task, referenced by both its `TaskHandle<R>` and the
        // `ConcreteTask` that runs on the worker/main thread.
        //
        // `done` uses `atomic<bool>::wait`/`notify_all` rather than a `condition_variable` — they
        // give the same block/wake behavior and, crucially, the same happens-before guarantee: once
        // `wait()` observes `done == true`, everything the executing thread wrote before its
        // `notify_all()` (in particular `result` below, which is otherwise plain, unsynchronized
        // storage) is visible without any extra locking.
        template <typename R>
        struct TaskState {
            atomic<bool> done{false};
            [[no_unique_address]] conditional_t<is_void_v<R>, NoResult, optional<R>> result{};

            void wait() noexcept {
                done.wait(false, std::memory_order_acquire);
            }

            void mark_done() noexcept {
                done.store(true, std::memory_order_release);
                done.notify_all();
            }
        };

        template <typename F, typename R>
        class ConcreteTask final : public TaskBase {
          public:
            ConcreteTask(F fn, shared_ptr<TaskState<R>> state) noexcept(std::is_nothrow_move_constructible_v<F>)
                : fn_(std::move(fn)), state_(std::move(state)) {
            }

            void execute() noexcept override {
                if constexpr (is_void_v<R>) {
                    fn_();
                } else {
                    state_->result.emplace(fn_());
                }
                state_->mark_done();
            }

          private:
            F fn_;
            shared_ptr<TaskState<R>> state_;
        };

    } // namespace Detail

    // Handle to a task in flight, returned by `Scheduler::spawn()`. Copyable (multiple observers can
    // await/poll the same task) — the underlying completion state is shared and only ever written
    // once, by the task itself.
    //
    // `wait()` blocks the calling thread until the task finishes. Note: if the calling thread is
    // itself a scheduler worker, this is a *plain* blocking wait — it does not steal/execute other
    // tasks while waiting (see :Scheduler's docs for that tradeoff), so waiting on a task from inside
    // another task risks stalling a worker if the pool is saturated. For a non-`void` task, `wait()`
    // also hands back the result, moved out of shared storage — call it at most once.
    template <typename R>
    class TaskHandle {
      public:
        TaskHandle() = default;

        // Constructed by Scheduler::spawn(); public only because module partitions can't easily
        // befriend a template across partition boundaries — not meant to be called directly.
        explicit TaskHandle(shared_ptr<Detail::TaskState<R>> state) noexcept
            : state_(std::move(state)) {
        }

        [[nodiscard]] bool is_done() const noexcept {
            return state_ && state_->done.load(std::memory_order_acquire);
        }

        auto wait() const noexcept {
            state_->wait();
            if constexpr (!is_void_v<R>) {
                return std::move(*state_->result);
            }
        }

      private:
        shared_ptr<Detail::TaskState<R>> state_;
    };

} // namespace SFT::Async
