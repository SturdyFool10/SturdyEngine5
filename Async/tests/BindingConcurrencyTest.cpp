// The facade's concurrency surface: cancellation, channels and sync primitives, tasks and join sets, streams, and the extra
// data-parallel operations -- everything a foreign language needs to use the engine's Tokio-and-Rayon runtime.
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstring>
#include <numeric>
#include <string>
#include <thread>

using namespace SFT;
using namespace SFT::Async;
namespace B = SFT::Async::Binding;
using namespace std::chrono_literals;

namespace {

    int failures = 0;

    void check(bool ok, const char *what) {
        if (!ok) {
            Foundation::log_error("BindingConcurrencyTest FAILED: {}", what);
            ++failures;
        }
    }

    B::Bytes bytes(std::string_view text) { return B::Bytes(text.begin(), text.end()); }
    std::string text(const B::Bytes &data) { return std::string(data.begin(), data.end()); }

    B::Value finish(const B::OpPtr &op) {
        if (!op->wait(10'000)) {
            Foundation::log_error("BindingConcurrencyTest: an operation did not finish");
            std::abort();
        }
        return op->result();
    }
    bool ok(const B::OpPtr &op) { return finish(op).status == B::OpStatus::Ok; }

    B::TaskFn constant(std::int64_t number, std::string_view payload = {}) {
        return [number, payload = std::string(payload)](B::ObjectPtr) {
            B::TaskOutput out;
            out.number = number;
            out.bytes = bytes(payload);
            return out;
        };
    }

    std::string field(const B::Value &v, std::string_view name) {
        for (const auto &[key, value] : v.fields) if (key == name) return value;
        return {};
    }

} // namespace

int main() {
    Scheduler::initialize(4);

    // ---- cancellation ----
    {
        auto source = B::cancel_source_new();
        auto token = B::cancel_source_token(source);
        check(!B::cancel_token_is_cancelled(token), "a fresh token is live");
        auto waiting = B::cancel_token_wait(token);
        auto sleeper = B::sleep(60'000);
        B::op_link_cancel(sleeper, token);
        check(!waiting->done(), "waiting on a live token pends");
        check(B::cancel_source_cancel(source), "the first cancel reports true");
        check(!B::cancel_source_cancel(source), "later cancels report false");
        check(ok(waiting), "the wait finishes when the token is cancelled");
        check(finish(sleeper).status == B::OpStatus::Cancelled, "a linked op is cancelled with the token");
        check(B::cancel_token_is_cancelled(token) && B::cancel_source_is_cancelled(source), "state reads cancelled");
        check(ok(B::cancel_token_wait(token)), "waiting on a cancelled token finishes at once");

        auto slow = B::op_with_timeout(B::sleep(60'000), 50);
        const auto timed_out = finish(slow);
        check(timed_out.status == B::OpStatus::Failed && timed_out.code == IoErrorCode::TimedOut, "op_with_timeout fails with TimedOut");
        check(ok(B::op_with_timeout(B::sleep(10), 5'000)), "op_with_timeout passes a fast op through");
        auto outer = B::op_with_timeout(B::sleep(60'000), 60'000);
        outer->cancel();
        check(finish(outer).status == B::OpStatus::Cancelled, "cancelling the wrapper cancels the wrapped op");
    }

    // ---- channels ----
    {
        auto [tx, rx] = B::channel_create(2);
        check(B::channel_try_send(tx, bytes("a")) == B::TryStatus::Ok && B::channel_try_send(tx, bytes("b")) == B::TryStatus::Ok, "buffer fills");
        check(B::channel_try_send(tx, bytes("c")) == B::TryStatus::Full, "a full channel refuses try_send");
        auto blocked = B::channel_send(tx, bytes("c"));
        check(!blocked->wait(50), "send waits while the channel is full (back-pressure)");
        auto first = finish(B::channel_recv(rx));
        check(first.number == 1 && text(first.bytes) == "a", "messages arrive in order");
        check(ok(blocked), "the waiting send completes once there is room");
        B::Bytes got;
        check(B::channel_try_recv(rx, &got) == B::TryStatus::Ok && text(got) == "b", "try_recv takes a buffered message");
        auto clone = B::channel_sender_clone(tx);
        B::channel_close(clone);
        check(text(finish(B::channel_recv(rx)).bytes) == "c", "closing still delivers what was buffered");
        const auto end = finish(B::channel_recv(rx));
        check(end.status == B::OpStatus::Ok && end.number == 0, "a closed, drained channel reads as end of stream");
        check(B::channel_try_recv(rx, &got) == B::TryStatus::Closed, "try_recv reports closed");
        const auto refused = finish(B::channel_send(tx, bytes("late")));
        check(refused.status == B::OpStatus::Failed && refused.code == IoErrorCode::Closed, "sending into a closed channel fails with Closed");

        auto [tx2, rx2] = B::channel_create(8);
        auto parked = B::channel_recv(rx2);
        parked->cancel();
        check(finish(parked).status == B::OpStatus::Cancelled, "a waiting receive can be cancelled");
        std::thread producer([&] {
            for (int i = 0; i < 100; ++i) (void)finish(B::channel_send(tx2, bytes(std::to_string(i))));
        });
        int sum = 0;
        for (int i = 0; i < 100; ++i) sum += std::stoi(text(finish(B::channel_recv(rx2)).bytes));
        producer.join();
        check(sum == 4950, "a producer thread and a consumer exchange 100 messages");
    }
    {
        auto [tx, rx] = B::channel_create(4);
        tx.reset();
        const auto end = finish(B::channel_recv(rx));
        check(end.number == 0, "releasing the last sender closes the channel");
    }

    // ---- oneshot, watch, broadcast ----
    {
        auto [tx, rx] = B::oneshot_create();
        auto pending = B::oneshot_recv(rx);
        check(!pending->done(), "a oneshot receive waits");
        check(B::oneshot_send(tx, bytes("once")) && !B::oneshot_send(tx, bytes("twice")), "only the first send counts");
        check(text(finish(pending).bytes) == "once", "the value arrives");
        bool threw = false;
        try {
            (void)B::oneshot_recv(rx);
        } catch (const B::BindingError &) {
            threw = true;
        }
        check(threw, "a oneshot receiver can be waited on once");

        auto [tx2, rx2] = B::oneshot_create();
        auto lost = B::oneshot_recv(rx2);
        tx2.reset();
        check(finish(lost).status == B::OpStatus::Failed, "dropping the sender fails the receiver");
        auto [tx3, rx3] = B::oneshot_create();
        auto failing = B::oneshot_recv(rx3);
        (void)B::oneshot_fail(tx3, "nope");
        check(finish(failing).error.find("nope") != std::string::npos, "oneshot_fail carries its message");

        auto [wtx, wrx] = B::watch_create(bytes("v0"));
        auto other = B::watch_subscribe(wtx);
        check(text(B::watch_borrow(wrx)) == "v0" && !B::watch_has_changed(wrx), "a watch starts with its initial value, unchanged");
        auto changed = B::watch_changed(wrx);
        check(!changed->done(), "changed() waits");
        B::watch_send(wtx, bytes("v1"));
        check(ok(changed) && text(B::watch_borrow_and_update(wrx)) == "v1", "a send wakes the waiter");
        check(B::watch_has_changed(other) && text(B::watch_borrow(other)) == "v1", "receivers track what they have seen independently");
        wtx.reset();
        check(finish(B::watch_changed(other)).status == B::OpStatus::Ok, "a pending change is still delivered after the sender is gone");

        auto [btx, brx] = B::broadcast_create(2);
        auto brx2 = B::broadcast_subscribe(btx);
        check(B::broadcast_send(btx, bytes("one")) == 2, "send reports the live receivers");
        check(text(finish(B::broadcast_recv(brx)).bytes) == "one" && text(finish(B::broadcast_recv(brx2)).bytes) == "one", "every receiver sees every message");
        (void)B::broadcast_send(btx, bytes("a"));
        (void)B::broadcast_send(btx, bytes("b"));
        (void)B::broadcast_send(btx, bytes("c"));
        const auto lagged = finish(B::broadcast_recv(brx));
        check(lagged.number == 1 && field(lagged, "skipped") == "1", "a receiver that fell behind is told how many it missed");
        btx.reset();
        B::Value v;
        do v = finish(B::broadcast_recv(brx)); while (v.number == 0);
        check(v.number == 2, "a closed broadcast ends the receiver");
    }

    // ---- semaphore, locks, notify, barrier ----
    {
        auto semaphore = B::semaphore_new(2);
        auto a = finish(B::semaphore_acquire(semaphore, 1)).object;
        auto b = finish(B::semaphore_acquire(semaphore, 1)).object;
        check(B::semaphore_available(semaphore) == 0 && B::semaphore_try_acquire(semaphore, 1) == nullptr, "permits run out");
        auto waiter = B::semaphore_acquire(semaphore, 1);
        check(!waiter->wait(50), "an acquire waits for a permit");
        B::permit_release(a);
        check(ok(waiter), "releasing a permit wakes the waiter");
        auto c = finish(waiter).object;
        check(B::permit_count(c) == 1, "a permit knows its size");
        B::permit_release(b);
        B::permit_release(c);
        check(B::semaphore_available(semaphore) == 2, "releasing the permits returns them");
        B::semaphore_add_permits(semaphore, 3);
        check(B::semaphore_available(semaphore) == 5, "add_permits");
        auto forgotten = B::semaphore_try_acquire(semaphore, 2);
        B::permit_forget(forgotten);
        check(B::semaphore_available(semaphore) == 3, "a forgotten permit stays consumed");
        auto parked = B::semaphore_acquire(semaphore, 10);
        B::semaphore_close(semaphore);
        const auto closed = finish(parked);
        check(closed.status == B::OpStatus::Failed && closed.code == IoErrorCode::Closed && B::semaphore_is_closed(semaphore), "closing fails waiters with Closed");

        auto mutex = B::async_mutex_new(bytes("start"));
        auto guard = finish(B::async_mutex_lock(mutex)).object;
        check(B::async_mutex_try_lock(mutex) == nullptr, "a held mutex refuses try_lock");
        auto second = B::async_mutex_lock(mutex);
        check(!second->wait(50), "lock waits while held");
        check(text(B::guard_get(guard)) == "start", "the guard reads the protected value");
        B::guard_set(guard, bytes("changed"));
        B::guard_release(guard);
        auto next_guard = finish(second).object;
        check(text(B::guard_get(next_guard)) == "changed", "the next holder sees the change");
        bool released_threw = false;
        try {
            (void)B::guard_get(guard);
        } catch (const B::BindingError &) {
            released_threw = true;
        }
        check(released_threw, "a released guard cannot be read");

        auto lock = B::async_rwlock_new(bytes("r"));
        auto r1 = finish(B::async_rwlock_read(lock)).object;
        auto r2 = finish(B::async_rwlock_read(lock)).object;
        auto writer = B::async_rwlock_write(lock);
        check(!writer->wait(50), "a writer waits for readers");
        B::guard_release(r1);
        B::guard_release(r2);
        auto w = finish(writer).object;
        B::guard_set(w, bytes("w"));
        B::guard_release(w);
        check(text(B::guard_get(finish(B::async_rwlock_read(lock)).object)) == "w", "readers see what the writer stored");

        auto notify = B::notify_new();
        auto waiting = B::notify_notified(notify);
        check(!waiting->wait(30), "notified() waits");
        B::notify_one(notify);
        check(ok(waiting), "notify_one wakes a waiter");
        B::notify_one(notify);
        check(ok(B::notify_notified(notify)), "a notification with nobody waiting is stored once");
        auto w1 = B::notify_notified(notify);
        auto w2 = B::notify_notified(notify);
        B::notify_waiters(notify);
        check(ok(w1) && ok(w2), "notify_waiters wakes everyone");

        auto barrier = B::barrier_new(3);
        auto b1 = B::barrier_wait(barrier);
        auto b2 = B::barrier_wait(barrier);
        check(!b1->wait(30), "a barrier waits for all parties");
        auto b3 = B::barrier_wait(barrier);
        const int leaders = int(finish(b1).number) + int(finish(b2).number) + int(finish(b3).number);
        check(leaders == 1, "exactly one party leads");
    }

    // ---- tasks ----
    {
        for (auto lane : {B::Lane::Light, B::Lane::Heavy, B::Lane::Blocking}) {
            const auto done = finish(B::task_spawn(constant(7, "result"), lane));
            check(done.status == B::OpStatus::Ok && done.number == 7 && text(done.bytes) == "result", "task_spawn returns the task's output");
        }
        const auto failed = finish(B::task_spawn([](B::ObjectPtr) -> B::TaskOutput { throw std::runtime_error("boom"); }, B::Lane::Light));
        check(failed.status == B::OpStatus::Failed && failed.error == "boom", "a throwing task fails its op with the message");
        const auto error = finish(B::task_spawn([](B::ObjectPtr) { B::TaskOutput o; o.error = "reported"; return o; }, B::Lane::Light));
        check(error.status == B::OpStatus::Failed && error.error == "reported", "a task that returns an error fails its op");

        std::atomic<bool> saw_cancel{false};
        auto cooperative = B::task_spawn(
            [&](B::ObjectPtr token) {
                for (int i = 0; i < 400 && !B::cancel_token_is_cancelled(token); ++i) std::this_thread::sleep_for(5ms);
                saw_cancel = B::cancel_token_is_cancelled(token);
                return B::TaskOutput{};
            },
            B::Lane::Heavy);
        std::this_thread::sleep_for(50ms);
        cooperative->cancel();
        (void)finish(cooperative);
        check(saw_cancel, "cancelling the op sets the token the task polls");

        auto pool = B::pool_create(2, "binding-test");
        check(finish(B::task_spawn_on(pool, constant(9))).number == 9, "task_spawn_on runs on a custom pool");

        auto set = B::join_set_new();
        for (int i = 0; i < 5; ++i) B::join_set_spawn(set, constant(i), B::Lane::Light);
        B::join_set_spawn(set, [](B::ObjectPtr) -> B::TaskOutput { throw std::runtime_error("one bad"); }, B::Lane::Blocking);
        int sum = 0, failed_count = 0;
        for (int i = 0; i < 6; ++i) {
            const auto next = finish(B::join_set_join_next(set));
            if (field(next, "outcome") == "ok") sum += int(next.number);
            else if (field(next, "outcome") == "failed" && field(next, "error") == "one bad") ++failed_count;
        }
        check(sum == 10 && failed_count == 1, "a join set yields every task, failures included");
        check(field(finish(B::join_set_join_next(set)), "outcome") == "empty" && B::join_set_len(set) == 0, "an exhausted join set reports empty");
        B::join_set_spawn(set, [](B::ObjectPtr token) {
            for (int i = 0; i < 400 && !B::cancel_token_is_cancelled(token); ++i) std::this_thread::sleep_for(5ms);
            B::TaskOutput o;
            o.number = B::cancel_token_is_cancelled(token) ? 1 : 0;
            return o;
        }, B::Lane::Heavy);
        B::join_set_abort_all(set);
        check(finish(B::join_set_join_next(set)).number == 1, "abort_all sets the tokens of running tasks");

        auto local = B::local_set_new();
        std::thread::id ran_on;
        auto local_op = B::local_set_spawn(local, [&](B::ObjectPtr) { ran_on = std::this_thread::get_id(); return B::TaskOutput{5, {}, {}, {}}; });
        check(!local_op->wait(50), "a local task waits for its thread to pump");
        B::local_set_block_on(local, local_op);
        check(local_op->done() && ran_on == std::this_thread::get_id() && local_op->result().number == 5, "block_on runs local tasks on the calling thread");
        auto second_local = B::local_set_spawn(local, constant(6));
        check(B::local_set_run_pending(local) == 1 && finish(second_local).number == 6, "run_pending drains the queue");

        auto dedicated = B::dedicated_thread_new("binding-dedicated");
        std::thread::id first_id, second_id;
        (void)finish(B::dedicated_thread_run(dedicated, [&](B::ObjectPtr) { first_id = std::this_thread::get_id(); return B::TaskOutput{}; }));
        (void)finish(B::dedicated_thread_run(dedicated, [&](B::ObjectPtr) { second_id = std::this_thread::get_id(); return B::TaskOutput{}; }));
        check(first_id == second_id && first_id != std::this_thread::get_id(), "a dedicated thread runs everything on one thread of its own");

        auto local_value = B::task_local_new();
        B::Bytes seen;
        check(!B::task_local_get(local_value, &seen), "a task-local is unbound outside a scope");
        B::task_local_scope(local_value, bytes("request-42"), [&] {
            check(B::task_local_get(local_value, &seen) && text(seen) == "request-42", "bound inside the scope");
            B::Bytes in_task;
            const auto spawned = finish(B::task_spawn([&](B::ObjectPtr) {
                B::Bytes value;
                B::TaskOutput o;
                o.number = B::task_local_get(local_value, &value) && text(value) == "request-42" ? 1 : 0;
                return o;
            }, B::Lane::Light));
            check(spawned.number == 1, "tasks spawned inside the scope inherit the value");
        });
        check(!B::task_local_get(local_value, &seen), "unbound again after the scope");

        std::atomic<int> main_ran{0};
        B::main_thread_run([&] { ++main_ran; });
        B::main_thread_pump();
        check(main_ran == 1, "main-thread work runs when pumped");
    }

    // ---- runtime, timers, signals ----
    {
        check(B::scheduler_is_running() && B::scheduler_worker_count() == 4 && !B::scheduler_on_worker_thread(), "scheduler state");
        const auto io = B::io_lane_stats();
        check(!io.empty() && io.front().first == "backend", "io_lane_stats");
        check(!B::ranked_logical_cores().empty(), "topology lists cores");
        check(B::par_num_threads() >= 1, "par_num_threads");

        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto begin = std::chrono::steady_clock::now();
        check(ok(B::sleep_until_unix_ms(now_ms + 80)), "sleep_until_unix_ms");
        check(std::chrono::steady_clock::now() - begin >= 60ms, "and it waited");
        check(ok(B::sleep_until_unix_ms(now_ms - 1000)), "a deadline in the past finishes at once");

        auto interval = B::interval_new(20);
        const auto ticks_begin = std::chrono::steady_clock::now();
        for (int i = 0; i < 4; ++i) check(ok(B::interval_tick(interval)), "tick");
        check(std::chrono::steady_clock::now() - ticks_begin >= 60ms, "an interval keeps its period");

#if defined(__linux__)
        auto signal_op = B::signal_wait({SIGUSR1});
        std::this_thread::sleep_for(50ms);
        std::raise(SIGUSR1);
        check(finish(signal_op).number == SIGUSR1, "signal_wait reports the signal");
#endif
    }

    // ---- streams ----
    {
        auto collect = [&](const B::ObjectPtr &stream) {
            const auto done = finish(B::stream_collect(stream));
            std::vector<std::string> out;
            for (auto &frame : B::unpack_frames(done.bytes)) out.push_back(text(frame));
            return out;
        };
        using V = std::vector<std::string>;
        auto numbers = [] { return B::stream_of_items({bytes("1"), bytes("2"), bytes("3"), bytes("4"), bytes("5")}); };

        check(collect(numbers()) == V{"1", "2", "3", "4", "5"}, "stream_of_items + collect");
        check(collect(B::stream_map(numbers(), [](B::Bytes b) { return bytes("<" + text(b) + ">"); })) == V{"<1>", "<2>", "<3>", "<4>", "<5>"}, "map");
        check(collect(B::stream_filter(numbers(), [](const B::Bytes &b) { return (text(b)[0] - '0') % 2 == 1; })) == V{"1", "3", "5"}, "filter");
        check(collect(B::stream_filter_map(numbers(), [](B::Bytes b) { return B::StreamMaybe{text(b) > "3", bytes(text(b) + "!")}; })) == V{"4!", "5!"}, "filter_map");
        check(collect(B::stream_take(numbers(), 2)) == V{"1", "2"} && collect(B::stream_skip(numbers(), 3)) == V{"4", "5"}, "take and skip");
        check(collect(B::stream_take_while(numbers(), [](const B::Bytes &b) { return text(b) < "3"; })) == V{"1", "2"}, "take_while");
        check(collect(B::stream_chain(B::stream_of_items({bytes("a")}), B::stream_of_items({bytes("b")}))) == V{"a", "b"}, "chain");
        {
            auto chunks = collect(B::stream_chunks(numbers(), 2));
            check(chunks.size() == 3 && B::unpack_frames(bytes(chunks[0])).size() == 2 && B::unpack_frames(bytes(chunks[2])).size() == 1, "chunks pack batches");
        }
        {
            auto zipped = collect(B::stream_zip(numbers(), B::stream_of_items({bytes("a"), bytes("b")})));
            check(zipped.size() == 2 && text(B::unpack_frames(bytes(zipped[1]))[0]) == "2" && text(B::unpack_frames(bytes(zipped[1]))[1]) == "b", "zip pairs items");
            auto indexed = collect(B::stream_enumerate(B::stream_of_items({bytes("x")})));
            check(indexed.size() == 1 && text(B::unpack_frames(bytes(indexed[0]))[1]) == "x", "enumerate");
        }
        {
            auto merged = collect(B::stream_merge(B::stream_of_items({bytes("1"), bytes("3")}), B::stream_of_items({bytes("2"), bytes("4")})));
            std::sort(merged.begin(), merged.end());
            check(merged == V{"1", "2", "3", "4"}, "merge interleaves both");
        }
        check(collect(B::stream_map_concurrent(numbers(), [](B::Bytes b) { return bytes(text(b) + text(b)); }, 3, true)) == V{"11", "22", "33", "44", "55"},
              "map_concurrent keeps order when asked");

        check(finish(B::stream_count(numbers())).number == 5, "count");
        {
            const auto last = finish(B::stream_last(numbers()));
            check(last.number == 1 && text(last.bytes) == "5", "last");
            check(finish(B::stream_last(B::stream_empty())).number == 0, "last of an empty stream");
        }
        {
            const auto sum = finish(B::stream_fold(numbers(), bytes("0"), [](B::Bytes acc, B::Bytes item) { return bytes(std::to_string(std::stoi(text(acc)) + std::stoi(text(item)))); }));
            check(text(sum.bytes) == "15", "fold");
            std::atomic<int> seen{0};
            check(ok(B::stream_for_each(numbers(), [&](const B::Bytes &) { ++seen; })) && seen == 5, "for_each");
            std::atomic<int> inspected{0};
            (void)collect(B::stream_inspect(numbers(), [&](const B::Bytes &) { ++inspected; }));
            check(inspected == 5, "inspect");
        }
        {
            auto stream = numbers();
            check(finish(B::stream_next(stream)).number == 1 && text(finish(B::stream_next(stream)).bytes) == "2", "next pulls item by item");
            (void)B::stream_count(stream);
            bool threw = false;
            try {
                (void)B::stream_count(stream);
            } catch (const B::BindingError &) {
                threw = true;
            }
            check(threw, "a consumed stream cannot be used again");
        }
        {
            auto [tx, rx] = B::channel_create(8);
            auto from_channel = B::stream_from_channel(rx);
            auto out_pair = B::channel_create(8);
            auto forward = B::stream_forward(B::stream_map(from_channel, [](B::Bytes b) { return bytes(text(b) + "?"); }), out_pair.sender);
            (void)finish(B::channel_send(tx, bytes("q")));
            check(text(finish(B::channel_recv(out_pair.receiver)).bytes) == "q?", "a channel feeds a stream that feeds a channel");
            tx.reset();
            check(ok(forward), "the forward finishes when the source ends");
        }
        {
            int produced = 0;
            auto generated = B::stream_generate([&]() -> B::StreamMaybe {
                if (produced >= 3) return {};
                return {true, bytes(std::to_string(produced++))};
            });
            check(collect(generated) == V{"0", "1", "2"}, "stream_generate calls the source until it stops");
            check(collect(B::stream_take(B::stream_interval(10), 3)).size() == 3, "stream_interval ticks");
            auto stalled = B::channel_create(1);
            auto slow = B::stream_timeout_each(B::stream_from_channel(stalled.receiver), 40);
            check(finish(B::stream_next(slow)).status == B::OpStatus::Failed, "timeout_each fails a stalled stream");
            const auto begin = std::chrono::steady_clock::now();
            (void)collect(B::stream_throttle(B::stream_of_items({bytes("a"), bytes("b"), bytes("c")}), 30));
            check(std::chrono::steady_clock::now() - begin >= 50ms, "throttle spaces items out");
            auto batches = collect(B::stream_chunks_timeout(B::stream_of_items({bytes("a"), bytes("b"), bytes("c")}), 10, 20));
            check(batches.size() == 1, "chunks_timeout flushes the final partial batch");
        }
        {
            bool bad = false;
            try {
                (void)B::unpack_frames(bytes("junk"));
            } catch (const B::BindingError &) {
                bad = true;
            }
            check(bad, "unpack_frames rejects junk");
            const std::vector<B::Bytes> frames = {bytes(""), bytes("abc")};
            check(B::unpack_frames(B::pack_frames(frames)) == frames, "pack/unpack round-trip");
        }
    }

    // ---- more parallelism ----
    {
        std::atomic<int> joined{0};
        B::par_join([&] { joined += 1; }, [&] { joined += 10; });
        check(joined == 11, "par_join runs both");
        std::atomic<int> spawned{0};
        B::par_scope([&](const B::ObjectPtr &scope) {
            for (int i = 0; i < 20; ++i) B::par_scope_spawn(scope, [&] { ++spawned; });
        });
        check(spawned == 20, "par_scope waits for everything it spawned");
        std::atomic<int> windows{0};
        B::par_for_windows(10, 4, [&](std::uint64_t) { ++windows; });
        check(windows == 7, "par_for_windows visits count - window + 1 starts");

        check(B::par_any(1000, [](std::uint64_t i) { return i == 777; }) && !B::par_any(1000, [](std::uint64_t i) { return i == 5000; }), "par_any");
        check(B::par_all(1000, [](std::uint64_t i) { return i < 1000; }) && !B::par_all(1000, [](std::uint64_t i) { return i < 999; }), "par_all");
        check(B::par_count(1000, [](std::uint64_t i) { return i % 3 == 0; }) == 334, "par_count");
        check(B::par_find_first(1000, [](std::uint64_t i) { return i % 7 == 3 && i > 100; }) == 101, "par_find_first returns the lowest index");
        check(B::par_find_any(1000, [](std::uint64_t i) { return i == 5; }) == 5 && B::par_find_first(10, [](std::uint64_t) { return false; }) == -1, "find_any / none");
        check(B::par_map_reduce_i64(1000, [](std::uint64_t i) { return std::int64_t(i); }, 0, [](std::int64_t a, std::int64_t b) { return a + b; }) == 499500, "par_map_reduce_i64");
        check(B::par_map_reduce_f64(100, [](std::uint64_t i) { return double(i); }, 0.0, [](double a, double b) { return a > b ? a : b; }) == 99.0, "par_map_reduce_f64 with a max");

        struct Record {
            std::int32_t key;
            std::int32_t order;
        };
        std::vector<Record> records(5000);
        for (int i = 0; i < 5000; ++i) records[i] = {(i * 7919) % 10, i};
        B::par_sort_by(records.data(), records.size(), sizeof(Record), true, [](const void *a, const void *b) {
            return static_cast<const Record *>(a)->key < static_cast<const Record *>(b)->key;
        });
        bool sorted = true, stable = true;
        for (std::size_t i = 1; i < records.size(); ++i) {
            if (records[i - 1].key > records[i].key) sorted = false;
            if (records[i - 1].key == records[i].key && records[i - 1].order > records[i].order) stable = false;
        }
        check(sorted && stable, "par_sort_by sorts structs stably with a foreign comparator");

        std::vector<std::int32_t> ints(1000);
        std::iota(ints.begin(), ints.end(), 1);
        B::NumericValue value;
        check(B::par_reduce(B::Numeric::I32, B::Reduction::Sum, ints.data(), ints.size(), &value) && value.i64 == 500500, "par_reduce sums i32 into 64 bits");
        check(B::par_reduce(B::Numeric::I32, B::Reduction::Max, ints.data(), ints.size(), &value) && value.i64 == 1000, "par_reduce max");
        std::vector<float> floats = {3.f, 1.f, 2.f};
        check(B::par_reduce(B::Numeric::F32, B::Reduction::Min, floats.data(), floats.size(), &value) && value.f64 == 1.0, "par_reduce min of floats");
        check(B::par_reduce(B::Numeric::F32, B::Reduction::Product, floats.data(), floats.size(), &value) && value.f64 == 6.0, "par_reduce product");
        std::vector<std::uint32_t> unsigned_values = {4000000000u, 4000000000u};
        check(B::par_reduce(B::Numeric::U32, B::Reduction::Sum, unsigned_values.data(), unsigned_values.size(), &value) && value.u64 == 8000000000ull, "u32 sums do not overflow");
        check(!B::par_reduce(B::Numeric::F64, B::Reduction::Min, nullptr, 0, &value), "min of nothing is false");
    }

    if (failures == 0) Foundation::log_info("BindingConcurrencyTest passed");
    return failures == 0 ? 0 : 1;
}
