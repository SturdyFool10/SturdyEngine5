#pragma once

// Task management beyond spawn/wait (tokio::task + tokio::select): JoinSet, select, spawn_blocking, block_on.

#include <Async/Cancellation.hpp>
#include <Async/IoHandler.hpp>
#include <Async/Mutex.hpp>
#include <Async/Promise.hpp>
#include <Async/Runtime.hpp>
#include <Async/Task.hpp>

#include <cstddef>
#include <deque>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace SFT::Async {

    // =============================================================================================
    // JoinSet: a dynamic set of tasks, joined in completion order.
    // =============================================================================================

    /// The outcome of one task of a JoinSet: its value, or the exception it ended with.
    template <typename T>
    using JoinResult = std::expected<Detail::ValueOrUnit<T>, std::exception_ptr>;

    /// Spawn many tasks, then `join_next()` them as they finish (fastest first), the way `tokio::task::JoinSet` works.
    /// Tasks keep running if the set is destroyed (they cannot be killed); `abort_all()` cancels the token handed to
    /// tasks spawned with `spawn_cancellable`, which should poll it.
    template <typename T>
    class JoinSet {
        using Outcome = JoinResult<T>;
        struct Inner {
            std::size_t active = 0;                          ///< spawned and not yet handed out by join_next
            std::deque<Outcome> finished;
            std::deque<Promise<std::optional<Outcome>>> waiters;
        };
        struct Shared {
            Mutex<Inner> inner;
            CancellationSource cancel;
        };

      public:
        JoinSet() : shared_(std::make_shared<Shared>()) {}

        /// Runs `fn` on runtime `Rt` and adds it to the set.
        template <AsyncRuntime Rt = DefaultRuntime, typename F>
        void spawn(F &&fn) {
            adopt(Rt::spawn(std::forward<F>(fn)));
        }

        /// Like `spawn`, but `fn(CancellationToken)` receives the set's token (see `abort_all`).
        template <AsyncRuntime Rt = DefaultRuntime, typename F>
        void spawn_cancellable(F fn) {
            adopt(Rt::spawn([fn = std::move(fn), token = shared_->cancel.token()]() mutable { return fn(token); }));
        }

        /// Adds an already-running operation (an IO call, a timer, a coroutine) to the set.
        void adopt(TaskHandle<T> handle) {
            {
                auto guard = shared_->inner.lock();
                ++guard->active;
            }
            handle.on_done([shared = shared_, handle]() mutable {
                Outcome outcome = std::unexpected(std::exception_ptr{});
                try {
                    if constexpr (std::is_void_v<T>) {
                        handle.wait();
                        outcome = Unit{};
                    } else {
                        outcome = handle.wait();
                    }
                } catch (...) {
                    outcome = std::unexpected(std::current_exception());
                }
                std::optional<Promise<std::optional<Outcome>>> waiter;
                {
                    auto guard = shared->inner.lock();
                    if (guard->waiters.empty()) {
                        guard->finished.push_back(std::move(outcome));
                        return;
                    }
                    waiter = std::move(guard->waiters.front());
                    guard->waiters.pop_front();
                    --guard->active;
                }
                (void)waiter->set_value(std::optional<Outcome>(std::move(outcome)));
            });
        }

        /// The next task to finish, or an empty optional once the set is empty.
        [[nodiscard]] TaskHandle<std::optional<Outcome>> join_next() {
            Promise<std::optional<Outcome>> promise;
            auto handle = promise.handle();
            std::optional<Outcome> ready;
            {
                auto guard = shared_->inner.lock();
                if (!guard->finished.empty()) {
                    ready = std::move(guard->finished.front());
                    guard->finished.pop_front();
                    --guard->active;
                } else if (guard->active == 0) {
                    (void)promise.set_value(std::optional<Outcome>{});
                    return handle;
                } else {
                    guard->waiters.push_back(promise);
                    return handle;
                }
            }
            (void)promise.set_value(std::move(ready));
            return handle;
        }

        /// Waits for everything currently in the set and returns the outcomes in completion order.
        [[nodiscard]] TaskHandle<std::vector<Outcome>> join_all() {
            return join_all_impl(*this);
        }

        /// Tasks spawned and not yet joined.
        [[nodiscard]] std::size_t len() const { return shared_->inner.lock()->active; }
        [[nodiscard]] bool empty() const { return len() == 0; }
        /// Cancels the token given to `spawn_cancellable` tasks.
        void abort_all() const { (void)shared_->cancel.cancel(); }

      private:
        static TaskHandle<std::vector<Outcome>> join_all_impl(JoinSet &set) {
            std::vector<Outcome> out;
            for (;;) {
                auto next = co_await set.join_next();
                if (!next) break;
                out.push_back(std::move(*next));
            }
            co_return out;
        }

        std::shared_ptr<Shared> shared_;
    };

    // =============================================================================================
    // select: the first of several differently-typed operations to finish (tokio::select!).
    // =============================================================================================
    template <typename... Rs>
    struct SelectResult {
        std::size_t index;                                   ///< which argument finished first
        std::variant<Detail::ValueOrUnit<Rs>...> value;      ///< its value (`Unit` for void)
    };

    namespace Detail {
        template <typename... Rs>
        struct SelectState {
            std::atomic<bool> claimed{false};
            Promise<SelectResult<Rs...>> promise;
        };

        template <std::size_t I, typename... Rs, typename R>
        void select_arm(const std::shared_ptr<SelectState<Rs...>> &state, TaskHandle<R> handle) {
            handle.on_done([state, handle]() mutable {
                if (state->claimed.exchange(true, std::memory_order_acq_rel)) return;
                try {
                    if constexpr (std::is_void_v<R>) {
                        handle.wait();
                        (void)state->promise.set_value(SelectResult<Rs...>{I, std::variant<ValueOrUnit<Rs>...>(std::in_place_index<I>, Unit{})});
                    } else {
                        (void)state->promise.set_value(SelectResult<Rs...>{I, std::variant<ValueOrUnit<Rs>...>(std::in_place_index<I>, handle.wait())});
                    }
                } catch (...) {
                    (void)state->promise.set_exception(std::current_exception());
                }
            });
        }
    } // namespace Detail

    /// Completes with the first operation to finish -- its index and value -- or fails with its exception. The others
    /// are not stopped; cancel them through the `CancellationToken` you gave them (see `select_cancel`).
    template <typename... Rs>
        requires(sizeof...(Rs) > 0)
    [[nodiscard]] TaskHandle<SelectResult<Rs...>> select(TaskHandle<Rs>... handles) {
        auto state = std::make_shared<Detail::SelectState<Rs...>>();
        auto result = state->promise.handle();
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (Detail::select_arm<I>(state, handles), ...);
        }(std::index_sequence_for<Rs...>{});
        return result;
    }

    /// `select`, then cancels `losers` as soon as a winner is known (pass the sources of the operations' tokens).
    template <typename... Rs>
    [[nodiscard]] TaskHandle<SelectResult<Rs...>> select_cancel(std::vector<CancellationSource> losers, TaskHandle<Rs>... handles) {
        auto winner = select(handles...);
        winner.on_done([losers = std::move(losers)]() mutable {
            for (auto &source : losers) (void)source.cancel();
        });
        return winner;
    }

    // =============================================================================================
    // spawn_blocking / block_on
    // =============================================================================================

    /// Runs a blocking call (a legacy API, a long syscall, a database driver) on the dedicated blocking pool instead of
    /// a CPU worker, so it cannot starve compute tasks. Returns a normal handle.
    template <typename F>
    [[nodiscard]] auto spawn_blocking(F &&fn) {
        return IoHandler::submit(std::forward<F>(fn));
    }

    /// Waits for `handle` on the calling thread. Refuses (throws `std::logic_error`) on the network reactor and timer
    /// threads, where blocking would deadlock the machinery that completes handles.
    template <typename T>
    auto block_on(TaskHandle<T> handle) {
        if (Detail::t_callback_thread) {
            throw std::logic_error("block_on called on a network/timer callback thread; hop to a scheduler first (then<Rt> / yield_to<Rt>)");
        }
        return handle.wait();
    }

    /// Calls `make()` (usually a coroutine returning a TaskHandle) and blocks on its result: the bridge from a
    /// synchronous `main` into async code.
    template <typename F>
        requires std::invocable<F &>
    auto block_on(F &&make) {
        return block_on(std::invoke(make));
    }

} // namespace SFT::Async
