#pragma once

#include <Foundation/Attributes.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace SFT::Async {

    namespace Detail {

        class TaskBase {
          public:
            /// Constructs a `TaskBase` in its default state.
            ///
            /// @note This function does not throw exceptions.
            TaskBase() = default;
            /// Destroys the `TaskBase` and releases resources owned by it.
            ///
            /// @note This function does not throw exceptions.
            virtual ~TaskBase() = default;
            /// Disables this construction form for `TaskBase`.
            ///
            /// @note This overload is deleted; attempting to call it is a compile-time error.
            TaskBase(const TaskBase &) = delete;
            /// Assigns a new value to this `TaskBase`.
            ///
            /// @return Returns `*this` so the operation can be chained.
            /// @note This overload is deleted; attempting to call it is a compile-time error.
            TaskBase &operator=(const TaskBase &) = delete;
            /// Disables this construction form for `TaskBase`.
            ///
            /// @note This overload is deleted; attempting to call it is a compile-time error.
            TaskBase(TaskBase &&) = delete;
            /// Assigns a new value to this `TaskBase`.
            ///
            /// @return Returns `*this` so the operation can be chained.
            /// @note This overload is deleted; attempting to call it is a compile-time error.
            TaskBase &operator=(TaskBase &&) = delete;

            /// Executes the requested work.
            ///
            /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
            /// @note This function does not throw exceptions.
            virtual void execute() noexcept = 0;
        };

        struct NoResult {};


        /// Waits for for task to complete.
        ///
        /// @param done `done` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void wait_for_task(std::atomic<bool> &done) noexcept;
        /// Notifies scheduler task completion.
        ///
        /// @note This function does not throw exceptions.
        void notify_scheduler_task_completion() noexcept;

        template <typename R>
        struct TaskState {
            std::atomic<bool> done{false};
            STURDY_NO_UNIQUE_ADDRESS
            std::conditional_t<std::is_void_v<R>, NoResult, std::optional<R>> result{};

            /// Waits for the associated operation or synchronization primitive to complete.
            ///
            /// @note This function does not throw exceptions.
            void wait() noexcept {
                wait_for_task(done);
            }

            /// Marks done using the supplied arguments and current state.
            ///
            /// @note This function does not throw exceptions.
            void mark_done() noexcept {
                done.store(true, std::memory_order_release);
                done.notify_all();
                notify_scheduler_task_completion();
            }
        };

        template <typename F, typename R>
        class ConcreteTask final : public TaskBase {
          public:
            /// Constructs a `ConcreteTask` from the supplied initialization values.
            ///
            /// @param fn Callable invoked by the operation.
            /// @param state `state` value used by the operation.
            ///
            /// @note This function does not throw exceptions.
            ConcreteTask(F fn, std::shared_ptr<TaskState<R>> state) noexcept(std::is_nothrow_move_constructible_v<F>)
                : fn_(std::move(fn)), state_(std::move(state)) {}

            /// Executes the requested work.
            ///
            /// @note This function does not throw exceptions.
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
        /// Constructs a `TaskHandle` in its default state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        TaskHandle() = default;

        /// Constructs a `TaskHandle` from the supplied initialization values.
        ///
        /// @param state `state` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit TaskHandle(std::shared_ptr<Detail::TaskState<R>> state) noexcept
            : state_(std::move(state)) {}

        /// Reports whether done holds for this `TaskHandle`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_done() const noexcept {
            return state_ && state_->done.load(std::memory_order_acquire);
        }


        /// Waits for the associated operation or synchronization primitive to complete.
        ///
        /// @return Returns the current wait value.
        /// @note This function does not throw exceptions.
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
