// The facade's shared-state primitives: blocking mutex, SPSC ring, Arc, Promise and the execution (sender) layer.
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <atomic>
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
            Foundation::log_error("BindingPrimitivesTest FAILED: {}", what);
            ++failures;
        }
    }
    B::Bytes bytes(std::string_view text) { return B::Bytes(text.begin(), text.end()); }
    std::string text(const B::Bytes &data) { return std::string(data.begin(), data.end()); }
    B::Value finish(const B::OpPtr &op) {
        if (!op->wait(10'000)) {
            Foundation::log_error("BindingPrimitivesTest: an operation did not finish");
            std::abort();
        }
        return op->result();
    }

} // namespace

int main() {
    Scheduler::initialize(2);

    // ---- blocking mutex ----
    {
        auto mutex = B::sync_mutex_new(bytes("v0"), "test-mutex");
        auto guard = B::sync_mutex_lock(mutex);
        check(text(B::guard_get(guard)) == "v0", "the guard reads the value");
        check(B::sync_mutex_try_lock(mutex) == nullptr, "a held mutex refuses try_lock");
        std::atomic<bool> got{false};
        std::thread other([&] {
            auto second = B::sync_mutex_lock(mutex); // blocks until the first guard is released
            got = text(B::guard_get(second)) == "v1";
            B::guard_set(second, bytes("v2"));
        });
        std::this_thread::sleep_for(50ms);
        check(!got, "lock blocks while held");
        B::guard_set(guard, bytes("v1"));
        B::guard_release(guard);
        other.join();
        check(got, "the waiter sees what the holder stored");
        auto last = B::sync_mutex_try_lock(mutex);
        check(last && text(B::guard_get(last)) == "v2", "try_lock succeeds when free");
    }

    // ---- SPSC ring ----
    {
        auto ring = B::ring_new(4, 20);
        check(B::ring_capacity(ring) == 4 && B::ring_max_item_bytes(ring) == 32, "capacity and slot size");
        check(B::ring_try_push(ring, bytes("a")) == B::RingStatus::Ok && B::ring_try_push(ring, bytes("")) == B::RingStatus::Ok, "push, including an empty item");
        check(B::ring_try_push(ring, B::Bytes(33, 'x')) == B::RingStatus::TooLarge, "an item beyond the slot size is refused");
        (void)B::ring_try_push(ring, bytes("c"));
        (void)B::ring_try_push(ring, bytes("d"));
        check(B::ring_try_push(ring, bytes("e")) == B::RingStatus::Full && B::ring_size(ring) == 4, "a full ring refuses");
        B::Bytes item;
        check(B::ring_try_pop(ring, &item) == B::RingStatus::Ok && text(item) == "a", "pop is in order");
        auto rest = B::ring_drain(ring, 0);
        check(rest.size() == 3 && rest[0].empty() && text(rest[2]) == "d", "drain takes the rest");
        check(B::ring_try_pop(ring, &item) == B::RingStatus::Empty, "empty");

        auto big = B::ring_new(1024, 20000);
        check(B::ring_max_item_bytes(big) == 65536, "the largest slot class");
        auto fast = B::ring_new(256, 8);
        std::thread producer([&] {
            for (int i = 0; i < 20000;) {
                if (B::ring_try_push(fast, bytes(std::to_string(i))) == B::RingStatus::Ok) ++i;
            }
        });
        long long sum = 0;
        for (int received = 0; received < 20000;) {
            B::Bytes got;
            if (B::ring_try_pop(fast, &got) == B::RingStatus::Ok) {
                sum += std::stoi(text(got));
                ++received;
            }
        }
        producer.join();
        check(sum == 199990000LL, "20000 messages cross the ring in order and intact");
        bool refused = false;
        try {
            (void)B::ring_new(0, 8);
        } catch (const B::BindingError &) {
            refused = true;
        }
        check(refused, "a zero-capacity ring is refused");
    }

    // ---- Arc ----
    {
        auto arc = B::arc_new(bytes("shared"));
        auto clone = B::arc_clone(arc);
        check(B::arc_use_count(arc) == 2 && B::arc_ptr_eq(arc, clone) && text(B::arc_bytes(clone)) == "shared", "clones share one value");
        clone.reset();
        check(B::arc_use_count(arc) == 1, "dropping a clone lowers the count");
        check(!B::arc_ptr_eq(arc, B::arc_new(bytes("shared"))), "equal content is not the same arc");

        int freed = 0;
        int payload = 5;
        auto foreign = B::arc_new_foreign(&payload, [&] { ++freed; });
        auto second = B::arc_clone(foreign);
        check(B::arc_pointer(second) == &payload && B::arc_bytes(second).empty(), "a foreign arc holds a pointer");
        foreign.reset();
        check(freed == 0, "the destroyer waits for the last reference");
        second.reset();
        check(freed == 1, "and runs once when it goes");
    }

    // ---- Promise ----
    {
        auto [promise, future] = B::promise_new();
        check(!future->done() && !B::promise_is_completed(promise), "a promise starts pending");
        std::thread setter([&] {
            std::this_thread::sleep_for(30ms);
            (void)B::promise_set_value(promise, 7, bytes("later"), "text");
        });
        const auto value = finish(future);
        setter.join();
        check(value.status == B::OpStatus::Ok && value.number == 7 && text(value.bytes) == "later" && value.text == "text", "the future carries the value");
        check(!B::promise_set_value(promise, 8, {}, {}) && !B::promise_set_error(promise, IoErrorCode::Unknown, "x"), "only the first completion wins");

        auto failing = B::promise_new();
        (void)B::promise_set_error(failing.promise, IoErrorCode::NotFound, "gone");
        const auto failure = finish(failing.future);
        check(failure.status == B::OpStatus::Failed && failure.code == IoErrorCode::NotFound && failure.error == "gone", "an error completes the future as Failed");

        auto cancelled = B::promise_new();
        (void)B::promise_cancel(cancelled.promise);
        check(finish(cancelled.future).status == B::OpStatus::Cancelled, "a cancelled promise cancels its future");

        auto dropped = B::promise_new();
        dropped.promise.reset();
        const auto broken = finish(dropped.future);
        check(broken.status == B::OpStatus::Failed && broken.code == IoErrorCode::Closed, "dropping a pending promise fails the future with Closed");

        check(finish(B::op_ready(3, bytes("r"), "t")).number == 3 && finish(B::op_failed(IoErrorCode::TimedOut, "slow")).code == IoErrorCode::TimedOut, "ready and failed ops");
    }

    // ---- execution senders ----
    {
        auto stage = [](std::int64_t n) { return [n](B::ObjectPtr) { B::TaskOutput out; out.number = n; return out; }; };
        auto chain = B::exec_just(stage(2), B::Lane::Light);
        chain = B::exec_then(chain, [](B::ObjectPtr, const B::TaskOutput &in) { B::TaskOutput out; out.number = in.number * 10; return out; }, B::Lane::Heavy);
        chain = B::exec_then(chain, [](B::ObjectPtr, const B::TaskOutput &in) { B::TaskOutput out; out.number = in.number + 1; out.text = "done"; return out; }, B::Lane::Blocking);
        const auto result = finish(B::exec_submit(chain));
        check(result.number == 21 && result.text == "done", "stages feed each other across lanes");
        check(B::exec_sync_wait(chain).number == 21, "sync_wait blocks for the result");
        check(B::exec_sync_wait(chain).number == 21, "a sender can be started again");

        std::atomic<int> ran{0};
        auto failing = B::exec_just([](B::ObjectPtr) -> B::TaskOutput { throw std::runtime_error("stage one failed"); }, B::Lane::Light);
        failing = B::exec_then(failing, [&](B::ObjectPtr, const B::TaskOutput &) { ++ran; return B::TaskOutput{}; }, B::Lane::Light);
        const auto failed = finish(B::exec_submit(failing));
        check(failed.status == B::OpStatus::Failed && failed.error == "stage one failed" && ran == 0, "a failure skips the later stages and propagates");
        bool threw = false;
        try {
            (void)B::exec_sync_wait(failing);
        } catch (const B::BindingError &error) {
            threw = std::string(error.what()) == "stage one failed";
        }
        check(threw, "sync_wait throws the failure");
    }

    if (failures == 0) Foundation::log_info("BindingPrimitivesTest passed");
    return failures == 0 ? 0 : 1;
}
