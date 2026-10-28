#pragma once

#include <Foundation/Attributes.hpp>
#include <Foundation/Cpu/IsaScope.hpp>

#include <Async/Mutex.hpp>

#include <atomic>
#include <concepts>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <vector>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace SFT::Async {

    namespace Detail {
        /// An immutable chain of task-local values (see <Async/Context.hpp>). A task captures the context of whoever
        /// spawned it, and a continuation the context of whoever registered it, so values follow the logical flow of work
        /// across threads, `co_await`s and timers rather than staying with an OS thread.
        struct ContextNode {
            const void *key;
            std::shared_ptr<void> value;
            std::shared_ptr<const ContextNode> parent;
        };
        using ContextPtr = std::shared_ptr<const ContextNode>;
        inline thread_local ContextPtr t_context;

        /// Installs `context` on this thread for the lifetime of the object.
        class ContextScope {
          public:
            explicit ContextScope(ContextPtr context) noexcept : saved_(std::move(t_context)) { t_context = std::move(context); }
            ~ContextScope() { t_context = std::move(saved_); }
            ContextScope(const ContextScope &) = delete;
            ContextScope &operator=(const ContextScope &) = delete;

          private:
            ContextPtr saved_;
        };

        /// True on threads that run completion callbacks for others (network reactor, timer): blocking one with
        /// `wait()` can deadlock the very machinery that would complete the awaited handle.
        inline thread_local bool t_callback_thread = false;

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
            [[nodiscard]] virtual bool can_run_here() const noexcept = 0;
            [[nodiscard]] virtual bool can_run_anywhere() const noexcept = 0;
            [[nodiscard]] virtual bool has_isa_requirements() const noexcept = 0;
            virtual void fail_isa_requirements() noexcept = 0;
        };

        /// A user-created thread pool (see <Async/ThreadPool.hpp>). A thread belonging to one has `t_pool` set: work spawned
        /// from it stays in that pool, and when it waits for a task it runs other queued work of the pool meanwhile.
        class PoolBase {
          public:
            virtual ~PoolBase() = default;
            virtual void enqueue(std::unique_ptr<TaskBase> task) noexcept = 0;
            /// Runs one queued task if there is one; false when the queue was empty.
            virtual bool try_run_one() noexcept = 0;
            [[nodiscard]] virtual std::size_t thread_count() const noexcept = 0;
        };
        inline thread_local PoolBase *t_pool = nullptr;

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
            std::atomic<bool> isa_unavailable{false};
            std::exception_ptr exception;
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
                run_continuations();
            }

            /// Registers `fn` to run once the task is done. If it already is, `fn` runs immediately on the
            /// calling thread; otherwise it runs on the thread that completes the task. `fn` must not throw.
            void on_done(std::function<void()> fn) {
                if (t_context) { // run the continuation in the context it was registered from
                    fn = [inner = std::move(fn), context = t_context]() mutable {
                        ContextScope scope(context);
                        inner();
                    };
                }
                {
                    auto guard = continuations.lock();
                    if (!done.load(std::memory_order_acquire)) {
                        guard->push_back(std::move(fn));
                        return;
                    }
                }
                fn();
            }

            Mutex<std::vector<std::function<void()>>> continuations;

          private:
            void run_continuations() noexcept {
                std::vector<std::function<void()>> pending;
                {
                    auto guard = continuations.lock();
                    pending.swap(*guard);
                }
                for (auto &fn : pending) {
                    fn();
                }
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
            ConcreteTask(F fn, std::shared_ptr<TaskState<R>> state,
                         Foundation::Cpu::IsaSet required = {}) noexcept(std::is_nothrow_move_constructible_v<F>)
                : fn_(std::move(fn)), state_(std::move(state)), required_(std::move(required)), context_(t_context) {}

            /// Executes the requested work.
            ///
            /// @note This function does not throw exceptions.
            void execute() noexcept override {
                Foundation::Cpu::IsaScope isa_scope(required_);
                if (!isa_scope.valid()) {
                    fail_isa_requirements();
                    return;
                }
                ContextScope context_scope(context_);
                try {
                    if constexpr (std::is_void_v<R>) {
                        fn_();
                    } else {
                        state_->result.emplace(fn_());
                    }
                } catch (...) {
                    state_->exception = std::current_exception();
                }
                state_->mark_done();
            }

            [[nodiscard]] bool can_run_here() const noexcept override {
                return Foundation::Cpu::IsaScope::can_enter(required_);
            }

            [[nodiscard]] bool can_run_anywhere() const noexcept override {
                return Foundation::Cpu::IsaScope::has_compatible_core(required_);
            }

            [[nodiscard]] bool has_isa_requirements() const noexcept override { return !required_.empty(); }

            void fail_isa_requirements() noexcept override {
                state_->isa_unavailable.store(true, std::memory_order_release);
                state_->mark_done();
            }

          private:
            F fn_;
            std::shared_ptr<TaskState<R>> state_;
            Foundation::Cpu::IsaSet required_;
            ContextPtr context_;
        };

    } // namespace Detail

    namespace Detail {
        /// `return_value` and `return_void` cannot coexist in one promise type, so the right one is chosen by base.
        template <typename R>
        struct CoroutineReturn {
            std::shared_ptr<TaskState<R>> state = std::make_shared<TaskState<R>>();
            template <typename V>
                requires std::convertible_to<V &&, R>
            void return_value(V &&value) {
                state->result.emplace(std::forward<V>(value));
                state->mark_done();
            }
        };
        template <>
        struct CoroutineReturn<void> {
            std::shared_ptr<TaskState<void>> state = std::make_shared<TaskState<void>>();
            void return_void() { state->mark_done(); }
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
        /// @throws std::runtime_error when the task's required ISA set cannot be provided.
        auto wait() const {
            state_->wait();
            if (state_->isa_unavailable.load(std::memory_order_acquire)) {
                throw std::runtime_error("task ISA requirements are unavailable on every eligible CPU");
            }
            if (state_->exception) {
                std::rethrow_exception(state_->exception);
            }
            if constexpr (!std::is_void_v<R>) {
                return std::move(*state_->result);
            }
        }

        [[nodiscard]] bool failed_due_to_isa() const noexcept {
            return state_ && state_->isa_unavailable.load(std::memory_order_acquire);
        }

        /// Whether this handle refers to a task at all (false for default-constructed handles).
        [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }

        /// Runs `fn` when the task completes -- successfully or not -- without blocking. `fn` runs inline on
        /// the completing thread (or immediately if already done), so keep it short and non-throwing;
        /// use `then<Rt>` to continue on a scheduler. Consume the result afterwards with `wait()`.
        void on_done(std::function<void()> fn) const { state_->on_done(std::move(fn)); }

        /// Awaiter used by `co_await handle`: suspends until the task completes, then yields its value
        /// (or rethrows its exception). The coroutine resumes on the thread that completed the task.
        struct Awaiter {
            TaskHandle handle;
            [[nodiscard]] bool await_ready() const noexcept { return handle.is_done(); }
            void await_suspend(std::coroutine_handle<> resumer) const {
                handle.on_done([resumer]() { resumer.resume(); });
            }
            auto await_resume() const { return handle.wait(); }
        };

        [[nodiscard]] Awaiter operator co_await() const & { return Awaiter{*this}; }
        [[nodiscard]] Awaiter operator co_await() && { return Awaiter{std::move(*this)}; }

        /// Lets a function returning `TaskHandle<R>` be a coroutine. It starts eagerly on the caller's
        /// thread and the returned handle completes when the coroutine finishes (or throws).
        struct promise_type : Detail::CoroutineReturn<R> {
            TaskHandle get_return_object() { return TaskHandle(this->state); }
            std::suspend_never initial_suspend() const noexcept { return {}; }
            std::suspend_never final_suspend() const noexcept { return {}; }
            void unhandled_exception() noexcept {
                this->state->exception = std::current_exception();
                this->state->mark_done();
            }
        };

      private:
        std::shared_ptr<Detail::TaskState<R>> state_;
    };

} // namespace SFT::Async
