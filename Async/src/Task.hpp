#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace SFT::Async {

    namespace Detail {

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

        struct NoResult {};

        // External threads block on the completion atomic. Scheduler workers instead execute queued
        // work while waiting, preventing nested task graphs from deadlocking a saturated pool.
        void wait_for_task(std::atomic<bool> &done) noexcept;
        void notify_scheduler_task_completion() noexcept;

        template <typename R>
        struct TaskState {
            std::atomic<bool> done{false};
            [[no_unique_address]] std::conditional_t<std::is_void_v<R>, NoResult, std::optional<R>> result{};

            void wait() noexcept {
                wait_for_task(done);
            }

            void mark_done() noexcept {
                done.store(true, std::memory_order_release);
                done.notify_all();
                notify_scheduler_task_completion();
            }
        };

        template <typename F, typename R>
        class ConcreteTask final : public TaskBase {
          public:
            ConcreteTask(F fn, std::shared_ptr<TaskState<R>> state) noexcept(std::is_nothrow_move_constructible_v<F>)
                : fn_(std::move(fn)), state_(std::move(state)) {}

            void execute() noexcept override {
                if constexpr (std::is_void_v<R>) {
                    fn_();
                } else {
                    state_->result.emplace(fn_());
                }
                state_->mark_done();
            }

          private:
            F fn_;
            std::shared_ptr<TaskState<R>> state_;
        };

    } // namespace Detail

    template <typename R>
    class TaskHandle {
      public:
        TaskHandle() = default;

        explicit TaskHandle(std::shared_ptr<Detail::TaskState<R>> state) noexcept
            : state_(std::move(state)) {}

        [[nodiscard]] bool is_done() const noexcept {
            return state_ && state_->done.load(std::memory_order_acquire);
        }

        // Worker-side waits use cooperative work helping and therefore follow structured fork/join
        // semantics: tasks must not synchronously wait on an ancestor in the active call stack.
        auto wait() const noexcept {
            state_->wait();
            if constexpr (!std::is_void_v<R>) {
                return std::move(*state_->result);
            }
        }

      private:
        std::shared_ptr<Detail::TaskState<R>> state_;
    };

} // namespace SFT::Async
