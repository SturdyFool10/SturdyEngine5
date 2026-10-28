// Continuations, promises, combinators, coroutines, timers and channels.
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace SFT;
using namespace SFT::Async;
using namespace std::chrono_literals;

namespace {

    int failures = 0;

    void check(bool ok, const char *what) {
        if (!ok) {
            Foundation::log_error("FutureTest FAILED: {}", what);
            ++failures;
        }
    }

    template <typename F>
    bool throws(F &&fn) {
        try {
            fn();
        } catch (...) {
            return true;
        }
        return false;
    }

    TaskHandle<int> coroutine_adds() {
        const int a = co_await ready(20);
        const int b = co_await Scheduler::spawn([] { return 22; });
        co_return a + b;
    }

    TaskHandle<std::string> coroutine_sleeps_and_hops() {
        co_await sleep_for(20ms);
        co_await yield_to<ParallelRuntime>();
        co_return std::string(Scheduler::is_worker_thread() ? "worker" : "other");
    }

    TaskHandle<int> coroutine_throws() {
        co_await ready();
        throw std::runtime_error("boom");
        co_return 0;
    }

    TaskHandle<void> coroutine_void(std::atomic<int> &counter) {
        co_await sleep_for(1ms);
        counter.fetch_add(1);
    }

    TaskHandle<int> sum_channel(Receiver<int> rx) {
        int total = 0;
        while (auto value = co_await rx.recv()) {
            total += *value;
        }
        co_return total;
    }

} // namespace

int main() {
    Scheduler::initialize(2);

    // Promise: first completion wins; wait() sees the value.
    {
        Promise<int> p;
        auto h = p.handle();
        check(!h.is_done(), "promise starts pending");
        check(p.set_value(7), "first set_value wins");
        check(!p.set_value(8), "second set_value is rejected");
        check(h.wait() == 7, "promise value");
    }

    // on_done fires for both pending and already-complete tasks.
    {
        Promise<int> p;
        std::atomic<int> fired{0};
        p.handle().on_done([&] { fired.fetch_add(1); });
        (void)p.set_value(1);
        p.handle().on_done([&] { fired.fetch_add(1); });
        check(fired.load() == 2, "on_done before and after completion");
    }

    // then: chains on the scheduler, propagates failures, skips fn on failure.
    {
        auto h = then(Scheduler::spawn([] { return 5; }), [](int v) { return v * 3; });
        check(h.wait() == 15, "then maps value");
        std::atomic<bool> ran{false};
        auto failed = then(Scheduler::spawn([]() -> int { throw std::logic_error("x"); }), [&](int v) {
            ran = true;
            return v;
        });
        check(throws([&] { (void)failed.wait(); }), "then propagates failure");
        check(!ran.load(), "then skips fn after failure");
        auto chained = then(then(ready(1), [](int v) { return v + 1; }), [](int v) { return std::to_string(v); });
        check(chained.wait() == "2", "then chains");
    }

    // Scheduler tasks now capture exceptions instead of terminating.
    check(throws([] { (void)Scheduler::spawn([]() -> int { throw std::runtime_error("e"); }).wait(); }),
          "scheduler task exception reaches wait()");

    // when_all: tuple of mixed types, vector, empty, failure.
    {
        auto all = when_all(Scheduler::spawn([] { return 1; }), Scheduler::spawn([] { return std::string("a"); }),
                            Scheduler::spawn([] {}));
        auto [i, s, unit] = all.wait();
        (void)unit;
        check(i == 1 && s == "a", "when_all tuple");

        std::vector<TaskHandle<int>> many;
        for (int n = 0; n < 50; ++n) {
            many.push_back(Scheduler::spawn([n] { return n; }));
        }
        auto values = when_all(std::move(many)).wait();
        bool in_order = values.size() == 50;
        for (int n = 0; n < static_cast<int>(values.size()); ++n) {
            in_order = in_order && values[static_cast<std::size_t>(n)] == n;
        }
        check(in_order, "when_all vector keeps order");
        check(when_all(std::vector<TaskHandle<int>>{}).wait().empty(), "when_all of nothing");
        check(throws([] {
                  (void)when_all(ready(1), Scheduler::spawn([]() -> int { throw std::runtime_error("f"); })).wait();
              }),
              "when_all fails if any input fails");
    }

    // when_any: the fastest wins.
    {
        std::vector<TaskHandle<int>> racers;
        racers.push_back(then(sleep_for(300ms), [] { return 0; }));
        racers.push_back(then(sleep_for(10ms), [] { return 1; }));
        auto winner = when_any(std::move(racers)).wait();
        check(winner.index == 1 && winner.value == 1, "when_any picks the earliest");
    }

    // Coroutines.
    {
        check(coroutine_adds().wait() == 42, "coroutine co_awaits handles");
        check(coroutine_sleeps_and_hops().wait() == "worker", "yield_to hops onto the scheduler");
        check(throws([] { (void)coroutine_throws().wait(); }), "coroutine exception reaches wait()");
        std::atomic<int> counter{0};
        coroutine_void(counter).wait();
        check(counter.load() == 1, "void coroutine");
    }

    // Timers.
    {
        const auto start = Clock::now();
        sleep_for(60ms).wait();
        const auto elapsed = Clock::now() - start;
        check(elapsed >= 55ms && elapsed < 1s, "sleep_for duration");

        // Many concurrent sleeps cost no worker threads.
        std::vector<TaskHandle<void>> sleepers;
        for (int n = 0; n < 500; ++n) {
            sleepers.push_back(sleep_for(std::chrono::milliseconds(10 + n % 20)));
        }
        const auto t0 = Clock::now();
        when_all(std::move(sleepers)).wait();
        check(Clock::now() - t0 < 1s, "500 concurrent sleeps overlap");

        CancellationSource source;
        auto cancellable = sleep_for(10s, source.token());
        std::this_thread::sleep_for(30ms);
        (void)source.cancel();
        const auto c0 = Clock::now();
        check(throws([&] { cancellable.wait(); }), "cancelled sleep fails");
        check(Clock::now() - c0 < 1s, "cancelled sleep ends promptly");

        check(throws([] { timeout(sleep_for(5s), 30ms).wait(); }), "timeout fires");
        check(timeout(ready(9), 5s).wait() == 9, "timeout passes through fast results");

        Interval interval(20ms);
        const auto i0 = Clock::now();
        for (int n = 0; n < 3; ++n) {
            interval.tick().wait();
        }
        const auto spent = Clock::now() - i0;
        check(spent >= 55ms && spent < 1s, "interval ticks at its period");
    }

    // Channels.
    {
        auto [tx, rx] = channel<int>(2);
        check(tx.try_send(1).has_value() && tx.try_send(2).has_value(), "try_send fills buffer");
        check(tx.try_send(3).error() == TrySendError::Full, "try_send reports Full");
        auto parked = tx.send(3);
        check(!parked.is_done(), "send parks when full (backpressure)");
        check(rx.try_recv().value() == 1, "try_recv fifo");
        check(parked.is_done(), "receiving releases the parked sender");
        check(rx.recv().wait().value() == 2 && rx.recv().wait().value() == 3, "values arrive in order");
        check(rx.try_recv().error() == TryRecvError::Empty, "try_recv Empty");

        auto pending = rx.recv();
        check(!pending.is_done(), "recv parks when empty");
        (void)tx.send(99).wait();
        check(pending.wait().value() == 99, "send hands value straight to a waiting receiver");
    }
    {
        // Producers on several threads, one coroutine consumer; closes when the last Sender drops.
        auto [tx, rx] = channel<int>(8);
        auto consumer = sum_channel(rx);
        {
            std::vector<std::thread> producers;
            for (int t = 0; t < 4; ++t) {
                producers.emplace_back([sender = tx] {
                    for (int n = 1; n <= 250; ++n) {
                        sender.send(n).wait();
                    }
                });
            }
            tx = Sender<int>{};
            for (auto &thread : producers) {
                thread.join();
            }
        }
        check(consumer.wait() == 4 * (250 * 251 / 2), "multi-producer sum with auto close");
    }
    {
        auto [tx, rx] = channel<int>(1);
        (void)tx.try_send(1);
        auto blocked = tx.send(2);
        rx.close();
        check(throws([&] { blocked.wait(); }), "close fails parked senders");
        check(rx.recv().wait().value() == 1, "close keeps buffered values");
        check(!rx.recv().wait().has_value(), "closed+drained yields end-of-stream");
        check(tx.try_send(5).error() == TrySendError::Closed, "send after close fails");
    }

    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
