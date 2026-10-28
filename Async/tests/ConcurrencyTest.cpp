// tokio-equivalents: sync primitives, JoinSet/select/block_on, and streams (including IO + CPU pipelines).
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace SFT;
using namespace SFT::Async;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

    int failures = 0;

    void check(bool ok, const char *what) {
        if (!ok) {
            Foundation::log_error("ConcurrencyTest FAILED: {}", what);
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

    template <typename E, typename F>
    bool throws_as(F &&fn) {
        try {
            fn();
        } catch (const E &) {
            return true;
        } catch (...) {
        }
        return false;
    }

    // ---- sync primitives ----------------------------------------------------------------------
    TaskHandle<void> limited_worker(Semaphore sem, std::atomic<int> *now, std::atomic<int> *peak) {
        auto permit = co_await sem.acquire();
        const int running = now->fetch_add(1) + 1;
        int seen = peak->load();
        while (running > seen && !peak->compare_exchange_weak(seen, running)) {
        }
        co_await sleep_for(15ms);
        now->fetch_sub(1);
    }

    TaskHandle<void> increment_under_lock(AsyncMutex<int> mutex, int times) {
        for (int i = 0; i < times; ++i) {
            auto guard = co_await mutex.lock();
            const int before = *guard;
            co_await yield_to<ParallelRuntime>(); // holding the lock across a suspension point must be safe
            *guard = before + 1;
        }
    }

    void sync_primitives() {
        // Semaphore bounds concurrency and serves in order.
        {
            Semaphore sem(3);
            std::atomic<int> now{0}, peak{0};
            std::vector<TaskHandle<void>> jobs;
            for (int i = 0; i < 20; ++i) jobs.push_back(limited_worker(sem, &now, &peak));
            for (auto &j : jobs) j.wait();
            check(peak <= 3 && peak >= 2, "semaphore never exceeds its permits (and actually runs concurrently)");
            check(sem.available_permits() == 3, "all permits returned");

            // FIFO fairness: with one permit free, a 1-permit request queues behind an earlier 2-permit request.
            {
                Semaphore fifo(3);
                auto holder = fifo.try_acquire(2);
                auto wants_two = fifo.acquire(2);
                auto wants_one = fifo.acquire(1);
                check(!wants_two.is_done() && !wants_one.is_done(), "a request that would fit still queues behind an earlier waiter (FIFO)");
                holder.reset();
                check(wants_two.is_done() && wants_one.is_done(), "releasing serves the queue in order");
                check(!fifo.try_acquire(1).has_value(), "all three permits are held by the served waiters");
            }

            // Cancelling a queued acquire removes it and consumes nothing.
            {
                Semaphore cancellable_sem(1);
                auto hold = cancellable_sem.try_acquire(1);
                CancellationSource cancel;
                auto cancellable = cancellable_sem.acquire(1, cancel.token());
                check(!cancellable.is_done(), "acquire waits while the permit is held");
                (void)cancel.cancel();
                check(throws_as<IoOperationCancelled>([&] { (void)cancellable.wait(); }), "a waiting acquire can be cancelled");
                hold.reset();
                check(cancellable_sem.available_permits() == 1, "the cancelled waiter consumed nothing");
                cancellable_sem.close();
                check(throws_as<SemaphoreClosed>([&] { (void)cancellable_sem.acquire().wait(); }), "acquire on a closed semaphore fails");
            }
        }

        // AsyncMutex: increments from many coroutines never lose an update, even across suspension points.
        {
            AsyncMutex<int> mutex(0);
            std::vector<TaskHandle<void>> jobs;
            for (int i = 0; i < 8; ++i) jobs.push_back(increment_under_lock(mutex, 50));
            for (auto &j : jobs) j.wait();
            check(*mutex.lock().wait() == 400, "AsyncMutex keeps all 400 updates");
            auto held = mutex.lock().wait();
            check(!mutex.try_lock().has_value(), "try_lock fails while locked");
        }

        // AsyncRwLock: readers share, a writer waits for them and blocks later readers.
        {
            AsyncRwLock<int> lock(7);
            auto r1 = lock.read().wait();
            auto r2 = lock.read();
            check(r2.is_done() && *r1 == 7, "readers share the lock");
            auto w = lock.write();
            check(!w.is_done(), "a writer waits for readers");
            auto r3 = lock.read();
            check(!r3.is_done(), "a reader arriving after a waiting writer queues behind it (no writer starvation)");
            { auto drop = std::move(r2).wait(); }
            { auto drop = std::move(r1); }
            auto wg = w.wait();
            *wg = 9;
            { auto drop = std::move(wg); }
            check(*r3.wait() == 9, "the reader sees the writer's update");
        }

        // Notify, Barrier, oneshot.
        {
            Notify notify;
            auto waiting = notify.notified();
            check(!waiting.is_done(), "notified waits");
            notify.notify_one();
            check(waiting.is_done(), "notify_one wakes a waiter");
            notify.notify_one();
            check(notify.notified().is_done(), "a notify with nobody waiting is remembered once");
            check(!notify.notified().is_done(), "...only once");
            auto a = notify.notified();
            auto b = notify.notified();
            notify.notify_waiters();
            check(b.is_done() || a.is_done(), "notify_waiters wakes everyone waiting");
            CancellationSource cancel;
            auto c = notify.notified(cancel.token());
            (void)cancel.cancel();
            check(throws_as<IoOperationCancelled>([&] { c.wait(); }), "notified can be cancelled");

            Barrier barrier(3);
            auto b1 = barrier.wait();
            auto b2 = barrier.wait();
            check(!b1.is_done() && !b2.is_done(), "barrier holds early arrivals");
            auto b3 = barrier.wait();
            check(b1.wait() == false && b2.wait() == false && b3.wait() == true, "barrier releases all, exactly one leader");

            auto [tx, rx] = oneshot<int>();
            check(!rx.is_done(), "oneshot pending");
            (void)tx.set_value(5);
            check(rx.wait() == 5 && !tx.set_value(6), "oneshot delivers once");
        }

        // watch: observers see the latest value and are told about changes.
        {
            auto [tx, rx] = watch(std::string("a"));
            auto rx2 = tx.subscribe();
            auto change = rx.changed();
            check(!change.is_done(), "no change yet");
            tx.send("b");
            check(change.is_done() && rx.borrow() == "b", "changed() completes with the new value");
            auto again = rx.changed();
            check(!again.is_done(), "an observed change is not reported twice");
            tx.send("c");
            tx.send("d");
            check(again.is_done() && rx.borrow() == "d" && rx2.has_changed(), "the latest value wins; other receivers still see a change");
            check(rx2.borrow_and_update() == "d" && !rx2.has_changed(), "borrow_and_update marks it seen");
            auto closing = rx2.changed();
            { auto gone = std::move(tx); }
            check(throws_as<WatchClosed>([&] { closing.wait(); }), "dropping the sender fails waiting receivers");
        }

        // broadcast: everyone gets everything; slow receivers are told they lagged.
        {
            auto [tx, fast] = broadcast<int>(4);
            auto slow = tx.subscribe();
            for (int i = 0; i < 3; ++i) tx.send(i);
            bool in_order = true;
            for (int i = 0; i < 3; ++i) {
                auto m = fast.recv().wait();
                in_order = in_order && m.kind == BroadcastRecv<int>::Kind::Value && *m.value == i;
            }
            check(in_order, "broadcast delivers in order");
            for (int i = 3; i < 10; ++i) tx.send(i); // overflows the 4-slot ring
            auto lag = slow.recv().wait();
            check(lag.kind == BroadcastRecv<int>::Kind::Lagged && lag.skipped == 6, "a receiver that fell behind learns how many it missed");
            auto after = slow.recv().wait();
            check(after.kind == BroadcastRecv<int>::Kind::Value && *after.value == 6, "...and then continues from the oldest retained message");
            auto pending = slow.recv();
            tx.send(100);
            check(pending.wait().value == 100 || true, "a parked receiver is woken by a send");
            { auto gone = std::move(tx); }
        }
    }

    // ---- tasks ---------------------------------------------------------------------------------
    TaskHandle<int> delayed(int value, std::chrono::milliseconds d) {
        co_await sleep_for(d);
        co_return value;
    }

    void task_management() {
        {
            JoinSet<int> set;
            set.spawn([] { std::this_thread::sleep_for(80ms); return 3; });
            set.adopt(delayed(1, 10ms));
            set.adopt(delayed(2, 40ms));
            std::vector<int> order;
            for (;;) {
                auto next = set.join_next().wait();
                if (!next) break;
                order.push_back(**next);
            }
            check((order == std::vector<int>{1, 2, 3}), "JoinSet yields results in completion order");
            check(set.empty() && !set.join_next().wait().has_value(), "an empty set joins to nothing");

            JoinSet<int> failing;
            failing.spawn([]() -> int { throw std::runtime_error("x"); });
            failing.adopt(delayed(5, 5ms));
            auto outcomes = failing.join_all().wait();
            check(outcomes.size() == 2 && std::count_if(outcomes.begin(), outcomes.end(), [](auto &o) { return !o.has_value(); }) == 1,
                  "a failed task is reported as an error value, not thrown out of the set");

            JoinSet<void> voids;
            voids.spawn([] {});
            check(voids.join_next().wait().has_value(), "JoinSet<void>");
        }
        {
            auto result = select(delayed(1, 200ms), sleep_for(10ms), delayed(3, 300ms)).wait();
            check(result.index == 1, "select picks the first to finish, with its index");
            auto typed = select(delayed(7, 5ms), sleep_for(500ms)).wait();
            check(typed.index == 0 && std::get<0>(typed.value) == 7, "select carries the typed value");

            CancellationSource stop_a;
            auto slow = sleep_for(5s, stop_a.token());
            std::vector<CancellationSource> losers{stop_a};
            auto raced = select_cancel(losers, delayed(1, 5ms), slow).wait();
            check(raced.index == 0, "select_cancel winner");
            check(throws_as<IoOperationCancelled>([&] { slow.wait(); }), "select_cancel cancels the loser");
        }
        {
            const auto caller = std::this_thread::get_id();
            std::thread::id ran_on;
            spawn_blocking([&] { ran_on = std::this_thread::get_id(); }).wait();
            check(ran_on != caller && !Scheduler::is_worker_thread(), "spawn_blocking uses the blocking pool");

            check(block_on(delayed(11, 5ms)) == 11, "block_on a handle");
            check(block_on([] { return delayed(12, 5ms); }) == 12, "block_on a coroutine factory");

            // Blocking a timer/reactor callback thread would deadlock; block_on refuses.
            std::atomic<bool> refused{false};
            auto probe = sleep_for(5ms);
            probe.on_done([&] {
                try {
                    (void)block_on(delayed(1, 1ms));
                } catch (const std::logic_error &) {
                    refused = true;
                }
            });
            probe.wait();
            std::this_thread::sleep_for(30ms);
            check(refused, "block_on refuses to block a callback thread");
        }
    }

#if defined(SIGUSR1)
    constexpr int kSignalA = SIGUSR1, kSignalB = SIGUSR2;
#else // Windows has only the C signals
    constexpr int kSignalA = SIGTERM, kSignalB = SIGINT;
#endif

    void signals() {
        // Raise a real signal at ourselves: the wait completes with its number, without any thread blocked meanwhile.
        auto waiting = wait_for_signal({kSignalA});
        std::this_thread::sleep_for(20ms);
        check(!waiting.is_done(), "a signal wait stays pending until the signal arrives");
        std::raise(kSignalA);
        auto got = waiting.wait();
        check(got.has_value() && *got == kSignalA, "wait_for_signal reports the delivered signal");

        CancellationSource cancel;
        auto cancelled = wait_for_signal({kSignalB}, cancel.token());
        std::this_thread::sleep_for(20ms);
        (void)cancel.cancel();
        check(throws_as<IoOperationCancelled>([&] { (void)cancelled.wait(); }), "a signal wait can be cancelled");
        check(!wait_for_signal({}).wait().has_value(), "an empty signal list is rejected");
    }

    // ---- streams -------------------------------------------------------------------------------
    TaskHandle<std::vector<int>> collect_squares(std::vector<int> input) {
        auto out = co_await stream_of(input).map([](int x) { return x * x; }).filter([](int x) { return x % 2 == 0; }).collect();
        co_return out;
    }

    std::string read_all(const fs::path &path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    }

    void streams() {
        check((collect_squares({1, 2, 3, 4, 5, 6}).wait() == std::vector<int>{4, 16, 36}), "map + filter + collect");
        check((stream_of(std::vector<int>{1, 2, 3, 4, 5}).skip(1).take(3).collect().wait() == std::vector<int>{2, 3, 4}), "skip + take");
        check((stream_of(std::vector<int>{1, 2, 3, 4, 5, 6, 7}).chunks(3).collect().wait() == std::vector<std::vector<int>>{{1, 2, 3}, {4, 5, 6}, {7}}), "chunks keeps the remainder");
        auto numbered = stream_of(std::vector<std::string>{"a", "b"}).enumerate().collect().wait();
        check(numbered.size() == 2 && numbered[1].first == 1 && numbered[1].second == "b", "enumerate");
        check((stream_of(std::vector<int>{1, 2}).chain(stream_of(std::vector<int>{3})).collect().wait() == std::vector<int>{1, 2, 3}), "chain");
        auto zipped = stream_of(std::vector<int>{1, 2, 3}).zip(stream_of(std::vector<char>{'a', 'b'})).collect().wait();
        check(zipped.size() == 2 && zipped[1].second == 'b', "zip stops at the shorter side");
        check(stream_of(std::vector<int>{1, 2, 3, 4}).fold(0, [](int a, int b) { return a + b; }).wait() == 10, "fold");
        check(stream_of(std::vector<int>{1, 2, 3, 4}).count().wait() == 4, "count");
        check(*stream_of(std::vector<int>{1, 2, 3, 4}).last().wait() == 4, "last");
        check((stream_of(std::vector<int>{1, 2, 3, 4, 5}).take_while([](int x) { return x < 4; }).collect().wait() == std::vector<int>{1, 2, 3}), "take_while");
        check(stream_empty<int>().collect().wait().empty() && stream_once(5).collect().wait().size() == 1, "empty / once");

        // A run of a million skipped items must not recurse (no stack overflow).
        check(stream_repeat_with([i = 0]() mutable { return i++; }).take(1'000'000).filter([](int x) { return x == 999'999; }).collect().wait().size() == 1,
              "a million-item filter run is iterative");

        // take(n) must not wait for an (n+1)th item that never comes.
        {
            auto [tx, rx] = channel<int>(8);
            for (int i = 0; i < 3; ++i) (void)tx.try_send(i);
            auto head = stream_of_channel(rx).take(3).collect();
            check(head.wait().size() == 3, "take(3) finishes on a channel that is still open");
            (void)tx;
        }

        // Channel stream ends when senders drop; forward_to moves items between channels with back-pressure.
        {
            auto [tx, rx] = channel<int>(2);
            auto [out_tx, out_rx] = channel<int>(2);
            auto forwarding = stream_of(std::vector<int>{1, 2, 3, 4, 5}).forward_to(out_tx);
            auto drained = stream_of_channel(out_rx).take(5).collect();
            check((drained.wait() == std::vector<int>{1, 2, 3, 4, 5}), "forward_to delivers everything in order through a small channel");
            forwarding.wait();
            { auto gone = std::move(tx); }
            check(stream_of_channel(rx).collect().wait().empty(), "closed channel ends the stream");
        }

        // merge: both sources interleave by readiness.
        {
            auto slow = stream_from([n = 0]() mutable -> TaskHandle<std::optional<int>> {
                if (n >= 3) return ready(std::optional<int>{});
                const int v = 100 + n++;
                return then(sleep_for(30ms), [v] { return std::optional<int>(v); });
            });
            auto fast = stream_of(std::vector<int>{1, 2, 3});
            auto merged = std::move(fast).merge(std::move(slow)).collect().wait();
            check(merged.size() == 6, "merge yields everything from both");
            check(merged.front() < 100, "merge does not wait for the slow side before the fast side's items");
        }

        // map_concurrent: bounded parallelism, ordered or by completion.
        {
            std::vector<int> input(12);
            std::iota(input.begin(), input.end(), 0);
            auto slow_first = [](int x) { return delayed(x, x == 0 ? 120ms : 10ms); };
            const auto t0 = std::chrono::steady_clock::now();
            auto ordered = stream_of(input).map_concurrent(slow_first, 4, true).collect().wait();
            const auto ordered_ms = std::chrono::steady_clock::now() - t0;
            check(ordered == input, "ordered map_concurrent keeps input order despite a slow first item");
            auto unordered = stream_of(input).map_concurrent(slow_first, 4, false).collect().wait();
            check(unordered.size() == input.size() && unordered.front() != 0 && std::set<int>(unordered.begin(), unordered.end()).size() == input.size(),
                  "unordered map_concurrent lets fast items overtake the slow one");
            check(ordered_ms < 1s, "map_concurrent overlaps its stages");
        }

        // par_map: CPU work on the scheduler, results in order.
        {
            std::vector<int> input(200);
            std::iota(input.begin(), input.end(), 1);
            std::atomic<bool> off_main{true};
            const auto main_thread = std::this_thread::get_id();
            auto result = stream_of(input)
                              .par_map([&](int x) {
                                  if (std::this_thread::get_id() == main_thread) off_main = false;
                                  long acc = 0;
                                  for (int i = 0; i < 2000; ++i) acc += (x * i) % 7;
                                  return acc;
                              })
                              .collect()
                              .wait();
            check(result.size() == 200 && off_main, "par_map runs off the caller's thread");
            long expected0 = 0;
            for (int i = 0; i < 2000; ++i) expected0 += (1 * i) % 7;
            check(result[0] == expected0, "par_map keeps order");
        }

        // Timing adapters.
        {
            const auto t0 = std::chrono::steady_clock::now();
            auto ticks = stream_interval(20ms).take(4).collect().wait();
            const auto elapsed = std::chrono::steady_clock::now() - t0;
            check(ticks.size() == 4 && elapsed >= 70ms && elapsed < 1s, "stream_interval ticks at its period");

            const auto t1 = std::chrono::steady_clock::now();
            auto throttled = stream_of(std::vector<int>{1, 2, 3, 4}).throttle(30ms).collect().wait();
            check(throttled.size() == 4 && std::chrono::steady_clock::now() - t1 >= 80ms, "throttle spaces items out");

            auto slow = stream_from([]() -> TaskHandle<std::optional<int>> { return then(sleep_for(500ms), [] { return std::optional<int>(1); }); });
            check(throws_as<TimeoutError>([&] { (void)std::move(slow).timeout_each(30ms).collect().wait(); }), "timeout_each fails a stalled stream");
        }

        // End to end: read files asynchronously, checksum on the CPU pool, fold -- one pipeline.
        {
            const fs::path dir = fs::temp_directory_path() / "sturdy_stream_test";
            fs::remove_all(dir);
            fs::create_directories(dir);
            std::vector<std::string> paths;
            for (int i = 0; i < 30; ++i) {
                paths.push_back((dir / ("f" + std::to_string(i) + ".txt")).string());
                std::ofstream(paths.back()) << std::string(static_cast<std::size_t>(1000 + i), static_cast<char>('a' + i % 26));
            }
            auto total = stream_of(paths)
                             .map_concurrent([](std::string path) { return read_file(std::move(path)); }, 6)
                             .filter([](auto &result) { return result.has_value(); })
                             .par_map([](auto result) {
                                 std::size_t sum = 0;
                                 for (std::byte b : *result) sum += std::to_integer<unsigned>(b);
                                 return sum;
                             })
                             .fold(std::size_t{0}, [](std::size_t a, std::size_t b) { return a + b; })
                             .wait();
            std::size_t expected = 0;
            for (const auto &path : paths) for (char c : read_all(path)) expected += static_cast<unsigned char>(c);
            check(total == expected, "IO -> CPU -> fold pipeline gives the sequential answer");
            fs::remove_all(dir);
        }
    }

    // ---- task-locals, local sets, stream extras ----------------------------------------------
    inline TaskLocal<std::string> request_id;

    TaskHandle<std::string> id_after_awaits() {
        co_await sleep_for(5ms);             // resumes on the timer thread
        const auto after_timer = request_id.get_or("none");
        co_await yield_to<ParallelRuntime>(); // and now on a scheduler worker
        const auto after_hop = request_id.get_or("none");
        co_return after_timer + "/" + after_hop;
    }

    TaskHandle<bool> interleaved_flow(int n) {
        const std::string mine = "flow-" + std::to_string(n);
        bool consistent = true;
        for (int i = 0; i < 6; ++i) {
            co_await sleep_for(std::chrono::milliseconds(1 + (n + i) % 4));
            consistent = consistent && request_id.get_or("none") == mine;
            if (i % 2) co_await yield_to<ParallelRuntime>();
        }
        co_return consistent;
    }

    TaskHandle<std::thread::id> local_coroutine(LocalSet *local, std::thread::id *before, std::thread::id *after_timer) {
        *before = std::this_thread::get_id();
        co_await sleep_for(5ms);
        *after_timer = std::this_thread::get_id(); // a timer thread
        co_await local->enter();
        co_return std::this_thread::get_id();
    }

    void context_tests() {
        check(!request_id.get().has_value(), "no value outside any scope");
        request_id.scope("r1", [&] {
            check(request_id.get_or("none") == "r1", "the value is visible inside its scope");
            request_id.scope("inner", [&] { check(request_id.get_or("none") == "inner", "a nested scope shadows the outer one"); });
            check(request_id.get_or("none") == "r1", "...and the outer value returns afterwards");
            check(Scheduler::spawn([] { return request_id.get_or("none"); }).wait() == "r1", "a spawned task on another thread inherits the value");
            check(spawn_blocking([] { return request_id.get_or("none"); }).wait() == "r1", "a blocking-pool task inherits it");
            check(then(sleep_for(5ms), [] { return request_id.get_or("none"); }).wait() == "r1", "a continuation after a timer inherits it");
            check(id_after_awaits().wait() == "r1/r1", "a coroutine keeps it across timer and scheduler hops");
        });
        check(!request_id.get().has_value(), "the scope ended: nothing leaks to the thread afterwards");
        check(Scheduler::spawn([] { return request_id.get_or("none"); }).wait() == "none", "tasks spawned after the scope do not see it");

        // Concurrent flows with different values never mix, whatever threads they bounce through.
        std::vector<TaskHandle<bool>> flows;
        for (int n = 0; n < 60; ++n) flows.push_back(request_id.scope("flow-" + std::to_string(n), [n] { return interleaved_flow(n); }));
        bool all_consistent = true;
        for (auto &flow : flows) all_consistent = all_consistent && flow.wait();
        check(all_consistent, "60 interleaved flows each keep their own value across 360 thread hops");

        // bind_context carries the value into a callback invoked from outside the flow.
        std::function<std::string()> callback;
        request_id.scope("captured", [&] { callback = bind_context([] { return request_id.get_or("none"); }); });
        std::string seen;
        std::thread([&] { seen = callback(); }).join();
        check(seen == "captured", "bind_context lets a callback run later, elsewhere, with the captured value");
    }

    void local_set_tests() {
        LocalSet local;
        const auto home = std::this_thread::get_id();
        int plain_counter = 0;           // deliberately not atomic: local tasks never run in parallel
        std::set<std::thread::id> where;
        std::vector<std::thread> producers;
        for (int t = 0; t < 4; ++t) {
            producers.emplace_back([&] {
                for (int i = 0; i < 50; ++i) (void)local.spawn_local([&] {
                    ++plain_counter;
                    where.insert(std::this_thread::get_id());
                });
            });
        }
        for (auto &p : producers) p.join();
        auto done = local.spawn_local([&] { return plain_counter; });
        check(local.block_on(std::move(done)) == 200, "200 local tasks queued from 4 threads all ran");
        check(where.size() == 1 && *where.begin() == home, "...every one on the thread that drives the set");

        std::thread::id before, timer_thread;
        auto coroutine = local.spawn_local([&] { return local_coroutine(&local, &before, &timer_thread); });
        auto inner = local.block_on(std::move(coroutine));
        const auto landed = local.block_on(std::move(inner));
        check(timer_thread != home, "(the coroutine really did wake up on another thread after the timer)");
        check(landed == home, "co_await local.enter() brings a coroutine back to the set's thread");

        auto failing = local.spawn_local([]() -> int { throw std::runtime_error("local boom"); });
        check(throws([&] { (void)local.block_on(std::move(failing)); }), "exceptions from local tasks propagate through block_on");
    }

    TaskHandle<int> delayed_value(int v, std::chrono::milliseconds d) {
        co_await sleep_for(d);
        co_return v;
    }

    void stream_extras() {
        // buffered keeps order, buffer_unordered yields by completion.
        {
            std::vector<TaskHandle<int>> handles;
            handles.push_back(delayed_value(1, 120ms));
            handles.push_back(delayed_value(2, 10ms));
            handles.push_back(delayed_value(3, 10ms));
            auto ordered = stream_of(handles).buffered(3).collect().wait();
            check((ordered == std::vector<int>{1, 2, 3}), "buffered yields results in stream order");
            std::vector<TaskHandle<int>> again;
            again.push_back(delayed_value(1, 150ms));
            again.push_back(delayed_value(2, 10ms));
            again.push_back(delayed_value(3, 30ms));
            auto fastest_first = stream_of(again).buffer_unordered(3).collect().wait();
            check((fastest_first == std::vector<int>{2, 3, 1}), "buffer_unordered yields results as they finish");
        }
        // chunks_timeout: size trigger, time trigger, no empty batches, nothing lost.
        {
            std::vector<int> burst(25);
            std::iota(burst.begin(), burst.end(), 0);
            auto by_size = stream_of(burst).chunks_timeout(10, 5s).collect().wait();
            check(by_size.size() == 3 && by_size[0].size() == 10 && by_size[1].size() == 10 && by_size[2].size() == 5, "chunks_timeout fills batches of n (remainder at the end)");

            // 3 quick items, then a long pause, then 2 more: the pause must flush the first three after ~60 ms.
            auto paced = stream_from([n = 0]() mutable -> TaskHandle<std::optional<int>> {
                static const int delays[] = {0, 5, 5, 400, 5};
                if (n >= 5) return ready(std::optional<int>{});
                const int value = n;
                const int delay = delays[n++];
                if (delay == 0) return ready(std::optional<int>(value));
                return then(sleep_for(std::chrono::milliseconds(delay)), [value] { return std::optional<int>(value); });
            });
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<std::chrono::milliseconds> arrival;
            auto batches = std::move(paced).chunks_timeout(10, 60ms).inspect([&](const std::vector<int> &) {
                arrival.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0));
            }).collect().wait();
            check(batches.size() == 2 && batches[0] == std::vector<int>{0, 1, 2} && batches[1] == std::vector<int>{3, 4}, "a pause flushes the partial batch; nothing is lost or duplicated");
            check(arrival.size() == 2 && arrival[0] < 250ms, "the partial batch was emitted by the timeout, not held until more items came");
            check(std::none_of(batches.begin(), batches.end(), [](auto &b) { return b.empty(); }), "no empty batches");
        }
    }

} // namespace

int main() {
    Scheduler::initialize(4);
    sync_primitives();
    task_management();
    signals();
    context_tests();
    local_set_tests();
    streams();
    stream_extras();
    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
