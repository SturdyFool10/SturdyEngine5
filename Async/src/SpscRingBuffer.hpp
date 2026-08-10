#pragma once

#include <Foundation/src/Foundation.hpp>

#include <atomic>
#include <cassert>
#include <memory>
#include <type_traits>
#include <vector>

#include <tracy/Tracy.hpp>

namespace SFT::Async {

    // Fixed-capacity, single-producer/single-consumer lock-free ring buffer. Exactly one thread may
    // ever call try_push() and exactly one (possibly different) thread may ever call drain_into()/
    // size() -- concurrent producers or concurrent consumers are not safe, only one of each at once.
    //
    // Capacity is rounded up to the next power of two at construction and allocated exactly once
    // (a `unique_ptr<T[]>`, default-constructing every slot up front); there is no further heap
    // activity on the hot path, and no growth -- a full buffer simply rejects try_push() rather than
    // reallocating, so callers get the same "bounded, drops under sustained overflow" contract a
    // capped vector would give them, without ever taking a lock to check it.
    //
    // T must be trivially copyable: slots are plain value-initialized array elements, written via
    // ordinary assignment and read via copy, not placement-new/manual lifetime management. That's a
    // deliberate simplification (this primitive is for plain-old-data messages like WindowEvent, not
    // a generic move-only-friendly queue) which keeps push/drain to a single assignment/copy each.
    //
    // Memory ordering mirrors Async::Scheduler's existing atomic-gate convention
    // (Async/src/SchedulerImpl.cpp's queued_count): the producer stores its own index with `release`
    // after writing the slot and loads the consumer's index with `acquire` to check for space; the
    // consumer loads the producer's index with `acquire` before reading slots and stores its own
    // index with `release` after it has finished reading them.
    template <typename T>
    class SpscRingBuffer {
        static_assert(std::is_trivially_copyable_v<T>, "SpscRingBuffer<T> requires a trivially copyable T");

      public:
        explicit SpscRingBuffer(usize capacity) noexcept
            : capacity_(round_up_to_power_of_two(capacity)),
              mask_(capacity_ - 1),
              buffer_(std::make_unique<T[]>(capacity_)) {}

        SpscRingBuffer(const SpscRingBuffer &) = delete;
        SpscRingBuffer &operator=(const SpscRingBuffer &) = delete;
        SpscRingBuffer(SpscRingBuffer &&) = delete;
        SpscRingBuffer &operator=(SpscRingBuffer &&) = delete;
        ~SpscRingBuffer() = default;

        // Producer-only. Returns false (and leaves the buffer untouched) if it's currently full --
        // the caller decides how to handle an overflow (this codebase's convention elsewhere is a
        // one-time-latched warning log, see WindowManager::drain_window_into()).
        [[nodiscard]] bool try_push(const T &value) noexcept {
            ZoneScopedN("Async::SpscRingBuffer::try_push");
            const usize tail = tail_.load(std::memory_order_relaxed);
            const usize head = head_.load(std::memory_order_acquire);
            assert(tail - head <= capacity_ && "SpscRingBuffer: occupancy exceeds capacity");
            if (tail - head >= capacity_) {
                return false;
            }
            const usize index = tail & mask_;
            assert(index < capacity_ && "SpscRingBuffer: computed index out of bounds");
            buffer_[index] = value;
            tail_.store(tail + 1, std::memory_order_release);
            return true;
        }

        // Consumer-only. Appends every currently-available slot to `out` (in FIFO order) and returns
        // how many were drained; does not clear or resize `out` first, matching vector::insert-style
        // "append what's new" semantics so a caller can drain multiple channels into one buffer.
        usize drain_into(std::vector<T> &out) noexcept {
            ZoneScopedN("Async::SpscRingBuffer::drain_into");
            const usize tail = tail_.load(std::memory_order_acquire);
            usize head = head_.load(std::memory_order_relaxed);
            const usize available = tail - head;
            assert(available <= capacity_ && "SpscRingBuffer: drain observed occupancy exceeding capacity");
            if (available == 0) {
                return 0;
            }
            out.reserve(out.size() + available);
            for (usize i = 0; i < available; ++i) {
                const usize index = head & mask_;
                assert(index < capacity_ && "SpscRingBuffer: computed index out of bounds");
                out.push_back(buffer_[index]);
                ++head;
            }
            head_.store(head, std::memory_order_release);
            return available;
        }

        // Approximate -- may be stale by the time the caller observes it if the other side is
        // concurrently active. Diagnostics only, never used to gate correctness.
        [[nodiscard]] usize size() const noexcept {
            const usize tail = tail_.load(std::memory_order_acquire);
            const usize head = head_.load(std::memory_order_acquire);
            return tail - head;
        }

        [[nodiscard]] usize capacity() const noexcept { return capacity_; }

      private:
        [[nodiscard]] static usize round_up_to_power_of_two(usize value) noexcept {
            usize result = 1;
            while (result < value) {
                result <<= 1U;
            }
            return result == 0 ? 1 : result; // guards a zero/overflow input rather than returning 0
        }

        usize capacity_;
        usize mask_;
        std::unique_ptr<T[]> buffer_;

        // Monotonically increasing, never wrapped directly (only the masked index into buffer_
        // wraps) -- this is what lets tail - head safely compute "how many slots are occupied" even
        // across usize wraparound, the same unsigned-difference trick Async::Scheduler's own atomics
        // rely on elsewhere in this codebase.
        //
        // Deliberately *not* alignas(64)-padded to separate cache lines: not worth the false-sharing
        // micro-optimization (and the over-alignment it would impose on WindowEventChannel, which
        // embeds this type and is allocated via make_shared) for a producer/consumer pair that already
        // synchronizes at most a few times a millisecond.
        std::atomic<usize> head_{0}; // consumer-owned index
        std::atomic<usize> tail_{0}; // producer-owned index
    };

} // namespace SFT::Async
