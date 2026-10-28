#pragma once

// Task-local values and single-threaded task sets (tokio::task_local! and tokio::task::LocalSet).

#include <Async/Mutex.hpp>
#include <Async/Promise.hpp>
#include <Async/Runtime.hpp>
#include <Async/Task.hpp>

#include <condition_variable>
#include <coroutine>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace SFT::Async {

    /// A value that follows the logical flow of work instead of a thread: set it with `scope`, and every task spawned,
    /// continuation registered, coroutine resumed or `then` chained from inside sees it -- on whatever thread it ends up --
    /// until the scope ends. Typical uses: request ids for logging, the current user, a tracing span, a deadline.
    ///
    /// Declare one with static storage (`inline TaskLocal<std::string> request_id;`); its address is the key.
    /// Nested scopes shadow outer ones. Values are copied in (`T` must be copyable to be read back with `get`).
    template <typename T>
    class TaskLocal {
      public:
        TaskLocal() = default;
        TaskLocal(const TaskLocal &) = delete;
        TaskLocal &operator=(const TaskLocal &) = delete;

        /// Runs `fn` with the value set, then restores the previous context. Whatever `fn` starts inherits the value.
        template <typename F>
        auto scope(T value, F &&fn) const {
            auto node = std::make_shared<Detail::ContextNode>(Detail::ContextNode{this, std::make_shared<T>(std::move(value)), Detail::t_context});
            Detail::ContextScope context(std::move(node));
            return std::forward<F>(fn)();
        }

        /// The value in effect here, if any scope set one.
        [[nodiscard]] std::optional<T> get() const {
            for (const Detail::ContextNode *node = Detail::t_context.get(); node != nullptr; node = node->parent.get()) {
                if (node->key == this) return *static_cast<const T *>(node->value.get());
            }
            return std::nullopt;
        }

        [[nodiscard]] T get_or(T fallback) const {
            auto value = get();
            return value ? std::move(*value) : std::move(fallback);
        }

        /// Calls `fn(const T&)` with the value without copying it; returns false if none is set.
        template <typename F>
        bool with(F &&fn) const {
            for (const Detail::ContextNode *node = Detail::t_context.get(); node != nullptr; node = node->parent.get()) {
                if (node->key == this) {
                    std::forward<F>(fn)(*static_cast<const T *>(node->value.get()));
                    return true;
                }
            }
            return false;
        }
    };

    /// Captures the current task-local context into a callable, so it can be run later from code that is not part of the
    /// flow (a callback from a C library, an event handler registered long ago).
    template <typename F>
    [[nodiscard]] auto bind_context(F fn) {
        return [fn = std::move(fn), context = Detail::t_context](auto &&...args) mutable -> decltype(auto) {
            Detail::ContextScope scope(context);
            return fn(std::forward<decltype(args)>(args)...);
        };
    }

    /// A set of tasks that all run on one thread -- the one that drives it with `run_until` / `block_on` -- for work that
    /// is not thread-safe (UI toolkits, scripting engines, GL contexts, `thread_local` caches). Tasks never run in parallel
    /// with each other, so they can share unsynchronised state.
    ///
    ///   LocalSet local;
    ///   auto result = local.block_on(local.spawn_local([&] { return use_non_thread_safe_thing(); }));
    ///
    /// Inside a local task, `co_await local.enter()` after any other `co_await` brings the coroutine back to this thread
    /// (other awaits may resume it on a reactor or timer thread).
    class LocalSet {
      public:
        LocalSet() : shared_(std::make_shared<Shared>()) {}

        /// Queues `fn` to run on the set's thread; the handle completes when it has. Safe from any thread.
        template <typename F>
        [[nodiscard]] auto spawn_local(F &&fn) {
            using R = std::invoke_result_t<std::decay_t<F>>;
            auto state = std::make_shared<Detail::TaskState<R>>();
            post([fn = std::decay_t<F>(std::forward<F>(fn)), state]() mutable {
                try {
                    if constexpr (std::is_void_v<R>) fn();
                    else state->result.emplace(fn());
                } catch (...) {
                    state->exception = std::current_exception();
                }
                state->mark_done();
            });
            return TaskHandle<R>(std::move(state));
        }

        /// Queues a raw closure. Safe from any thread.
        void post(std::function<void()> work) const {
            {
                auto guard = shared_->queue.lock();
                guard->tasks.push_back(std::move(work));
            }
            shared_->notify();
        }

        /// Runs the set's tasks on the calling thread until `handle` completes, then returns its result. Tasks queued by the
        /// handle's own work are run too.
        template <typename T>
        auto block_on(TaskHandle<T> handle) {
            if (Detail::t_callback_thread) throw std::logic_error("LocalSet::block_on cannot run on a network/timer callback thread");
            handle.on_done([shared = shared_] { shared->notify(); });
            for (;;) {
                std::function<void()> work;
                {
                    std::unique_lock lock(shared_->wait_mutex);
                    shared_->wake.wait(lock, [&] {
                        if (handle.is_done()) return true;
                        auto guard = shared_->queue.lock();
                        return !guard->tasks.empty();
                    });
                }
                {
                    auto guard = shared_->queue.lock();
                    if (!guard->tasks.empty()) {
                        work = std::move(guard->tasks.front());
                        guard->tasks.pop_front();
                    }
                }
                if (work) {
                    work();
                    continue;
                }
                if (handle.is_done()) break;
            }
            return handle.wait();
        }

        /// Runs everything currently queued (and what that queues) without waiting for more. Returns how many ran.
        std::size_t run_pending() {
            std::size_t ran = 0;
            for (;;) {
                std::function<void()> work;
                {
                    auto guard = shared_->queue.lock();
                    if (guard->tasks.empty()) return ran;
                    work = std::move(guard->tasks.front());
                    guard->tasks.pop_front();
                }
                work();
                ++ran;
            }
        }

        /// `co_await set.enter()`: continue the coroutine on the set's thread.
        [[nodiscard]] auto enter() const {
            struct Awaiter {
                std::shared_ptr<Shared> shared;
                bool await_ready() const noexcept { return false; }
                void await_suspend(std::coroutine_handle<> resumer) const {
                    {
                        auto guard = shared->queue.lock();
                        guard->tasks.push_back([resumer] { resumer.resume(); });
                    }
                    shared->notify();
                }
                void await_resume() const noexcept {}
            };
            return Awaiter{shared_};
        }

      private:
        struct Queue {
            std::deque<std::function<void()>> tasks;
        };
        struct Shared {
            Mutex<Queue> queue;
            std::mutex wait_mutex;
            std::condition_variable wake;
            /// Taking the wait mutex between "state changed" and "notify" closes the window in which a waiter has checked its
            /// predicate but not yet gone to sleep (which would lose the wake-up).
            void notify() {
                { std::lock_guard lock(wait_mutex); }
                wake.notify_all();
            }
        };
        std::shared_ptr<Shared> shared_;
    };

} // namespace SFT::Async
