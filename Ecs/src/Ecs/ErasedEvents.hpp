#pragma once

/// A type-erased event buffer, the untyped counterpart of `Events<T>`.
///
/// `Events<T>` stores a `std::vector<T>` and is reached through the resource table by a key derived
/// from its C++ type name. A caller that has no C++ type — a system written in another language —
/// needs the same behavior over raw bytes, which is what this provides: a flat buffer of
/// fixed-stride records, drained by the scheduler between frames exactly as a typed event buffer is.
///
/// Bound into the world as an ordinary resource whose `clear` hook points at `clear_erased_events`,
/// which is what makes `ScheduleConfig::clear_events_on_run` drain it. Without that hook it would
/// behave like a plain resource and grow without bound.

#include <Ecs/Resource.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace SFT::Ecs {

    class ErasedEvents {
      public:
        /// Constructs an `ErasedEvents` holding records of a fixed size.
        ///
        /// @param element_size Bytes per event. Must be nonzero.
        ///
        /// @note This function does not throw exceptions.
        explicit ErasedEvents(usize element_size) noexcept : element_size_(element_size) {}

        /// Appends one event, copying its bytes.
        ///
        /// @param event Bytes to copy; must address at least `element_size()`.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void send(const void *event) {
            const usize offset = storage_.size();
            storage_.resize(offset + element_size_);
            std::memcpy(storage_.data() + offset, event, element_size_);
            ++count_;
        }

        /// Returns the event at `index`.
        ///
        /// @param index Event to address, less than `size()`.
        ///
        /// @return Borrowed storage for that event, or null when `index` is out of range. Valid
        ///         until the next `send` or `clear`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const void *at(usize index) const noexcept {
            if (index >= count_) {
                return nullptr;
            }
            return storage_.data() + (index * element_size_);
        }

        /// Returns how many events are buffered.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize size() const noexcept { return count_; }

        /// Returns the byte size of one event.
        ///
        /// @return Returns the current element size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize element_size() const noexcept { return element_size_; }

        /// Discards every buffered event.
        ///
        /// @note This function does not throw exceptions.
        void clear() noexcept {
            storage_.clear();
            count_ = 0;
        }

      private:
        std::vector<std::byte> storage_;
        usize element_size_ = 0;
        usize count_ = 0;
    };

    /// Clear hook bound alongside an `ErasedEvents` resource.
    ///
    /// Matches the `void (*)(void *) noexcept` shape the world stores for event resources, so the
    /// scheduler drains an erased event buffer on the same schedule as a typed one.
    ///
    /// @param object The `ErasedEvents` to drain.
    /// @note This function does not throw exceptions.
    inline void clear_erased_events(void *object) noexcept {
        static_cast<ErasedEvents *>(object)->clear();
    }

} // namespace SFT::Ecs
