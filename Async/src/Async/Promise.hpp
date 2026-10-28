#pragma once

#include <Async/Runtime.hpp>
#include <Async/Task.hpp>

#include <atomic>
#include <concepts>
#include <cstddef>
#include <exception>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace SFT::Async {

    /// The producing half of a `TaskHandle<T>`: something other than a scheduler task (a timer, a channel,
    /// a network event, a C callback) completes the handle by calling `set_value` / `set_exception`.
    /// Only the first completion wins; later calls return false and change nothing.
    template <typename T>
    class Promise {
      public:
        Promise() : state_(std::make_shared<Detail::TaskState<T>>()) {}

        [[nodiscard]] TaskHandle<T> handle() const { return TaskHandle<T>(state_); }

        template <typename V>
            requires(!std::is_void_v<T> && std::convertible_to<V &&, T>)
        bool set_value(V &&value) {
            if (!claim()) {
                return false;
            }
            state_->result.emplace(std::forward<V>(value));
            state_->mark_done();
            return true;
        }

        bool set_value()
            requires std::is_void_v<T>
        {
            if (!claim()) {
                return false;
            }
            state_->mark_done();
            return true;
        }

        bool set_exception(std::exception_ptr error) {
            if (!claim()) {
                return false;
            }
            state_->exception = std::move(error);
            state_->mark_done();
            return true;
        }

        [[nodiscard]] bool is_completed() const noexcept { return completed_->load(std::memory_order_acquire); }

        /// Whether both refer to the same underlying promise (copies of one `Promise`).
        [[nodiscard]] bool same_as(const Promise &other) const noexcept { return state_ == other.state_; }

      private:
        [[nodiscard]] bool claim() noexcept {
            bool expected = false;
            return completed_->compare_exchange_strong(expected, true, std::memory_order_acq_rel);
        }

        std::shared_ptr<Detail::TaskState<T>> state_;
        std::shared_ptr<std::atomic<bool>> completed_ = std::make_shared<std::atomic<bool>>(false);
    };

    /// A handle that is already complete with `value`.
    template <typename T>
    [[nodiscard]] TaskHandle<std::decay_t<T>> ready(T &&value) {
        Promise<std::decay_t<T>> promise;
        (void)promise.set_value(std::forward<T>(value));
        return promise.handle();
    }

    [[nodiscard]] inline TaskHandle<void> ready() {
        Promise<void> promise;
        (void)promise.set_value();
        return promise.handle();
    }

    namespace Detail {
        template <typename Fn, typename R>
        struct ThenResult {
            using type = std::invoke_result_t<Fn &, R>;
        };
        template <typename Fn>
        struct ThenResult<Fn, void> {
            using type = std::invoke_result_t<Fn &>;
        };

        /// Moves the outcome of a finished `source` into `target`, running `fn` on its value if present.
        template <typename Out, typename In, typename Fn>
        void forward_outcome(TaskHandle<In> source, Promise<Out> target, Fn &fn) {
            try {
                if constexpr (std::is_void_v<In>) {
                    source.wait();
                    if constexpr (std::is_void_v<Out>) {
                        fn();
                        (void)target.set_value();
                    } else {
                        (void)target.set_value(fn());
                    }
                } else {
                    auto value = source.wait();
                    if constexpr (std::is_void_v<Out>) {
                        fn(std::move(value));
                        (void)target.set_value();
                    } else {
                        (void)target.set_value(fn(std::move(value)));
                    }
                }
            } catch (...) {
                (void)target.set_exception(std::current_exception());
            }
        }
    } // namespace Detail

    /// Continues `source` with `fn` on runtime `Rt` once it completes -- no thread blocks in between.
    /// `fn` takes the source's value (nothing for `TaskHandle<void>`). If the source failed, `fn` is skipped
    /// and the returned handle fails with the same exception; an exception thrown by `fn` fails it too.
    template <AsyncRuntime Rt = DefaultRuntime, typename R, typename Fn>
    [[nodiscard]] auto then(TaskHandle<R> source, Fn fn) {
        using Out = typename Detail::ThenResult<Fn, R>::type;
        Promise<Out> promise;
        source.on_done([source, promise, fn = std::move(fn)]() mutable {
            (void)Rt::spawn([source = std::move(source), promise = std::move(promise), fn = std::move(fn)]() mutable {
                Detail::forward_outcome<Out>(std::move(source), std::move(promise), fn);
            });
        });
        return promise.handle();
    }

    /// Completes with every input's value (in argument order) once all of them are done.
    /// If any input fails, the result fails with the first exception in argument order, after all finished.
    template <typename... Rs>
        requires(sizeof...(Rs) > 0)
    [[nodiscard]] auto when_all(TaskHandle<Rs>... handles) {
        using Value = std::tuple<std::conditional_t<std::is_void_v<Rs>, std::monostate, Rs>...>;
        struct Shared {
            explicit Shared(TaskHandle<Rs>... hs) : inputs(std::move(hs)...) {}
            std::tuple<TaskHandle<Rs>...> inputs;
            std::atomic<std::size_t> remaining{sizeof...(Rs)};
            Promise<Value> promise;
        };
        auto shared = std::make_shared<Shared>(handles...);
        auto finish = [shared]() {
            if (shared->remaining.fetch_sub(1, std::memory_order_acq_rel) != 1) {
                return;
            }
            try {
                auto take = [](auto &handle) {
                    if constexpr (std::is_void_v<decltype(handle.wait())>) {
                        handle.wait();
                        return std::monostate{};
                    } else {
                        return handle.wait();
                    }
                };
                // Braced init guarantees left-to-right evaluation, so the first failure in order wins.
                (void)shared->promise.set_value(std::apply(
                    [&](auto &...hs) { return Value{take(hs)...}; }, shared->inputs));
            } catch (...) {
                (void)shared->promise.set_exception(std::current_exception());
            }
        };
        (handles.on_done(finish), ...);
        return shared->promise.handle();
    }

    /// Completes with all values of a homogeneous set, preserving order. An empty set completes immediately.
    template <typename R>
    [[nodiscard]] TaskHandle<std::vector<std::conditional_t<std::is_void_v<R>, std::monostate, R>>> when_all(
        std::vector<TaskHandle<R>> handles) {
        using Elem = std::conditional_t<std::is_void_v<R>, std::monostate, R>;
        using Value = std::vector<Elem>;
        struct Shared {
            std::vector<TaskHandle<R>> inputs;
            std::atomic<std::size_t> remaining;
            Promise<Value> promise;
        };
        auto shared = std::make_shared<Shared>();
        shared->inputs = std::move(handles);
        shared->remaining.store(shared->inputs.size());
        if (shared->inputs.empty()) {
            (void)shared->promise.set_value(Value{});
            return shared->promise.handle();
        }
        auto finish = [shared]() {
            if (shared->remaining.fetch_sub(1, std::memory_order_acq_rel) != 1) {
                return;
            }
            try {
                Value out;
                out.reserve(shared->inputs.size());
                for (auto &handle : shared->inputs) {
                    if constexpr (std::is_void_v<R>) {
                        handle.wait();
                        out.emplace_back();
                    } else {
                        out.push_back(handle.wait());
                    }
                }
                (void)shared->promise.set_value(std::move(out));
            } catch (...) {
                (void)shared->promise.set_exception(std::current_exception());
            }
        };
        for (auto &handle : shared->inputs) {
            handle.on_done(finish);
        }
        return shared->promise.handle();
    }

    /// Result of `when_any`: which input finished first and its value.
    template <typename R>
    struct AnyResult {
        std::size_t index;
        std::conditional_t<std::is_void_v<R>, std::monostate, R> value;
    };

    /// Completes as soon as the first of `handles` finishes. The others keep running (tasks are not
    /// cancellable by handle); cancel them through their `CancellationToken` if they take one.
    /// A failure of the winner fails the result. An empty set fails with `std::invalid_argument`.
    template <typename R>
    [[nodiscard]] TaskHandle<AnyResult<R>> when_any(std::vector<TaskHandle<R>> handles) {
        struct Shared {
            std::vector<TaskHandle<R>> inputs;
            Promise<AnyResult<R>> promise;
        };
        auto shared = std::make_shared<Shared>();
        shared->inputs = std::move(handles);
        if (shared->inputs.empty()) {
            (void)shared->promise.set_exception(std::make_exception_ptr(std::invalid_argument("when_any: no handles")));
            return shared->promise.handle();
        }
        for (std::size_t i = 0; i < shared->inputs.size(); ++i) {
            shared->inputs[i].on_done([shared, i]() {
                if (shared->promise.is_completed()) {
                    return;
                }
                try {
                    if constexpr (std::is_void_v<R>) {
                        shared->inputs[i].wait();
                        (void)shared->promise.set_value(AnyResult<R>{i, {}});
                    } else {
                        (void)shared->promise.set_value(AnyResult<R>{i, shared->inputs[i].wait()});
                    }
                } catch (...) {
                    (void)shared->promise.set_exception(std::current_exception());
                }
            });
        }
        return shared->promise.handle();
    }

    /// `co_await yield_to<Rt>()` hops the coroutine onto runtime `Rt` (e.g. back onto the CPU scheduler
    /// after resuming on an IO or timer thread).
    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto yield_to() {
        struct Awaiter {
            [[nodiscard]] bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> resumer) const {
                (void)Rt::spawn([resumer]() { resumer.resume(); });
            }
            void await_resume() const noexcept {}
        };
        return Awaiter{};
    }

} // namespace SFT::Async
