#pragma once

#include <Foundation/Foundation.hpp>

#include <atomic>
#include <cassert>
#include <memory>
#include <type_traits>
#include <vector>

#include <tracy/Tracy.hpp>

namespace SFT::Async {


    template <typename T>
    class SpscRingBuffer {
        static_assert(std::is_trivially_copyable_v<T>, "SpscRingBuffer<T> requires a trivially copyable T");

      public:
        /// Constructs a `SpscRingBuffer` from the supplied initialization values.
        ///
        /// @param capacity `capacity` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit SpscRingBuffer(usize capacity) noexcept
            : capacity_(round_up_to_power_of_two(capacity)),
              mask_(capacity_ - 1),
              buffer_(std::make_unique<T[]>(capacity_)) {}

        /// Disables this construction form for `SpscRingBuffer`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        SpscRingBuffer(const SpscRingBuffer &) = delete;
        /// Assigns a new value to this `SpscRingBuffer`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        SpscRingBuffer &operator=(const SpscRingBuffer &) = delete;
        /// Disables this construction form for `SpscRingBuffer`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        SpscRingBuffer(SpscRingBuffer &&) = delete;
        /// Assigns a new value to this `SpscRingBuffer`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        SpscRingBuffer &operator=(SpscRingBuffer &&) = delete;
        /// Destroys the `SpscRingBuffer` and releases resources owned by it.
        ///
        /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
        ~SpscRingBuffer() = default;


        /// Attempts to push without requiring normal failure to be exceptional.
        ///
        /// @param value Value consumed by the operation.
        ///
        /// @return Returns `true` when the operation succeeds; otherwise returns `false`.
        /// @note Normal failure is reported by returning `false`.
        /// @note This function does not throw exceptions.
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


        /// Drains into using the supplied arguments and current state.
        ///
        /// @param out `out` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
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


        /// Returns the size for this `SpscRingBuffer`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        /// Returns the size for this `SpscRingBuffer`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize size() const noexcept {
            const usize tail = tail_.load(std::memory_order_acquire);
            const usize head = head_.load(std::memory_order_acquire);
            return tail - head;
        }

        /// Returns the current or globally available capacity value.
        ///
        /// @return Returns the current capacity value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize capacity() const noexcept { return capacity_; }

      private:
        /// Rounds up to power of two using the supplied arguments and current state.
        ///
        /// @param value Value consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static usize round_up_to_power_of_two(usize value) noexcept {
            usize result = 1;
            while (result < value) {
                result <<= 1U;
            }
            return result == 0 ? 1 : result;
        }

        usize capacity_;
        usize mask_;
        std::unique_ptr<T[]> buffer_;


        std::atomic<usize> head_{0};
        std::atomic<usize> tail_{0};
    };

} // namespace SFT::Async
