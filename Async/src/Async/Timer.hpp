#pragma once

#include <Async/Cancellation.hpp>
#include <Async/Promise.hpp>
#include <Async/Task.hpp>

#include <chrono>
#include <functional>
#include <stdexcept>
#include <utility>

namespace SFT::Async {

    using Clock = std::chrono::steady_clock;

    /// Thrown (via the failed handle) by `timeout` when the deadline passes before the operation completes.
    class TimeoutError final : public std::runtime_error {
      public:
        TimeoutError() : std::runtime_error("Async operation timed out") {}
    };

    namespace Detail {
        /// Runs `fn` on the timer thread at `deadline`. `fn` must be short and must not block: it should
        /// only complete a promise. If `token` is cancelled first, `on_cancel` runs instead (within ~10 ms).
        void schedule_at(Clock::time_point deadline, std::function<void()> fn, CancellationToken token,
                         std::function<void()> on_cancel);
    } // namespace Detail

    /// Completes after `duration`, without occupying a scheduler or IO worker. Cancelling `token` fails the
    /// handle with `IoOperationCancelled`. Continuations (including `co_await` resumption) run on the timer
    /// thread: hop with `then<Rt>` or `co_await yield_to<Rt>()` before doing real work.
    [[nodiscard]] inline TaskHandle<void> sleep_until(Clock::time_point deadline, CancellationToken token = {}) {
        Promise<void> promise;
        Detail::schedule_at(
            deadline, [promise]() mutable { (void)promise.set_value(); }, token,
            [promise]() mutable { (void)promise.set_exception(std::make_exception_ptr(IoOperationCancelled{})); });
        return promise.handle();
    }

    [[nodiscard]] inline TaskHandle<void> sleep_for(Clock::duration duration, CancellationToken token = {}) {
        return sleep_until(Clock::now() + duration, std::move(token));
    }

    /// Mirrors `source`, but fails with `TimeoutError` if it has not finished within `duration`.
    /// The underlying work is not cancelled; pass it a `CancellationToken` if it should stop.
    template <typename R>
    [[nodiscard]] TaskHandle<R> timeout(TaskHandle<R> source, Clock::duration duration) {
        Promise<R> promise;
        source.on_done([source, promise]() mutable {
            try {
                if constexpr (std::is_void_v<R>) {
                    source.wait();
                    (void)promise.set_value();
                } else {
                    (void)promise.set_value(source.wait());
                }
            } catch (...) {
                (void)promise.set_exception(std::current_exception());
            }
        });
        Detail::schedule_at(
            Clock::now() + duration,
            [promise]() mutable { (void)promise.set_exception(std::make_exception_ptr(TimeoutError{})); }, {}, {});
        return promise.handle();
    }

    /// Fixed-rate ticker. `tick()` completes at each period boundary; if the consumer falls behind, missed
    /// ticks are skipped rather than delivered in a burst, so the next tick is always in the future.
    class Interval {
      public:
        explicit Interval(Clock::duration period) : period_(period), next_(Clock::now() + period) {}

        [[nodiscard]] TaskHandle<void> tick(CancellationToken token = {}) {
            const auto now = Clock::now();
            const auto due = next_ > now ? next_ : now;
            next_ += period_;
            if (next_ <= now) {
                next_ = now + period_;
            }
            return sleep_until(due, std::move(token));
        }

        [[nodiscard]] Clock::duration period() const noexcept { return period_; }

      private:
        Clock::duration period_;
        Clock::time_point next_;
    };

} // namespace SFT::Async
