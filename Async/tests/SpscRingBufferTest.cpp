#include <Async/src/SpscRingBuffer.hpp>

#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

namespace {

    using SFT::usize;
    using SFT::u64;

    /// Checks the supplied condition and reports the accompanying diagnostic message when it is false.
    ///
    /// @param condition Condition controlling whether the operation proceeds.
    /// @param message Text consumed by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }

    /// Adds the supplied value to the end or work queue.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool push_and_drain_preserves_order() {
        SFT::Async::SpscRingBuffer<int> ring(8);
        bool passed = true;

        passed &= check(ring.capacity() == 8, "capacity should round an exact power of two to itself");

        for (int i = 0; i < 5; ++i) {
            passed &= check(ring.try_push(i), "push should succeed while under capacity");
        }

        std::vector<int> out;
        const usize drained = ring.drain_into(out);
        passed &= check(drained == 5, "drain_into should report exactly what was pushed");
        passed &= check(out.size() == 5, "drain_into should append exactly what was pushed");
        for (int i = 0; i < 5; ++i) {
            passed &= check(out[static_cast<usize>(i)] == i, "drain_into should preserve FIFO order");
        }

        std::vector<int> empty_drain;
        passed &= check(ring.drain_into(empty_drain) == 0, "draining an already-empty ring should report zero");
        passed &= check(empty_drain.empty(), "draining an already-empty ring should append nothing");

        return passed;
    }

    /// Returns the current or globally available capacity rounds up to next power of two value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool capacity_rounds_up_to_next_power_of_two() {
        SFT::Async::SpscRingBuffer<int> ring(5);
        return check(ring.capacity() == 8, "non-power-of-two capacity should round up to the next power of two");
    }

    /// Returns the current or globally available full ring rejects further pushes without corrupting state value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool full_ring_rejects_further_pushes_without_corrupting_state() {
        SFT::Async::SpscRingBuffer<int> ring(4);
        bool passed = true;

        for (int i = 0; i < 4; ++i) {
            passed &= check(ring.try_push(i), "push should succeed up to capacity");
        }
        passed &= check(!ring.try_push(99), "push should fail once the ring is at capacity");

        std::vector<int> out;
        passed &= check(ring.drain_into(out) == 4, "a full ring should still drain everything that was pushed");
        for (int i = 0; i < 4; ++i) {
            passed &= check(out[static_cast<usize>(i)] == i, "rejected push must not have corrupted earlier slots");
        }
        return passed;
    }


    /// Returns the current or globally available sustained cycles survive index wraparound value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool sustained_cycles_survive_index_wraparound() {
        SFT::Async::SpscRingBuffer<int> ring(4);
        bool passed = true;
        int next_expected_value = 0;

        for (int cycle = 0; cycle < 1000; ++cycle) {
            for (int i = 0; i < 3; ++i) {
                passed &= check(ring.try_push(next_expected_value + i), "push should succeed within one cycle's headroom");
            }
            std::vector<int> out;
            passed &= check(ring.drain_into(out) == 3, "each cycle should drain exactly what it pushed");
            for (int i = 0; i < 3; ++i) {
                passed &= check(out[static_cast<usize>(i)] == next_expected_value + i,
                                "wraparound should not reorder or corrupt values across many cycles");
            }
            next_expected_value += 3;
            if (!passed) {
                break;
            }
        }
        return passed;
    }


    /// Returns the current or globally available concurrent producer consumer loses nothing value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool concurrent_producer_consumer_loses_nothing() {
        constexpr usize ring_capacity = 256;
        constexpr u64 total_items = 2'000'000;

        SFT::Async::SpscRingBuffer<u64> ring(ring_capacity);
        bool passed = true;

        std::thread producer([&]() {
            for (u64 i = 0; i < total_items; ++i) {
                while (!ring.try_push(i)) {
                    std::this_thread::yield();
                }
            }
        });

        std::vector<u64> received;
        received.reserve(total_items);
        std::vector<u64> batch;
        while (received.size() < total_items) {
            batch.clear();
            ring.drain_into(batch);
            if (batch.empty()) {
                std::this_thread::yield();
                continue;
            }
            received.insert(received.end(), batch.begin(), batch.end());
        }

        producer.join();

        passed &= check(received.size() == total_items, "consumer should observe every item the producer pushed");
        if (received.size() == total_items) {
            for (u64 i = 0; i < total_items; ++i) {
                if (received[i] != i) {
                    passed &= check(false, "concurrent stress run lost, duplicated, or reordered an item");
                    break;
                }
            }
        }
        return passed;
    }

} // namespace

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    bool passed = push_and_drain_preserves_order();
    passed &= capacity_rounds_up_to_next_power_of_two();
    passed &= full_ring_rejects_further_pushes_without_corrupting_state();
    passed &= sustained_cycles_survive_index_wraparound();
    passed &= concurrent_producer_consumer_loses_nothing();
    return passed ? 0 : 1;
}
