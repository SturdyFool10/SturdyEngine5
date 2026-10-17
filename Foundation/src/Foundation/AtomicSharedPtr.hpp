#pragma once

#include <atomic>
#include <memory>
#include <utility>
#include <version>

namespace SFT::Foundation {

    /// A `std::shared_ptr<T>` that can be loaded and stored from several threads at once (the publish-a-snapshot pattern).
    ///
    /// This is `std::atomic<std::shared_ptr<T>>` where the standard library has it. libc++ does not implement that
    /// specialization (the primary template then fails its trivially-copyable requirement), so there it falls back to the
    /// `std::atomic_load` / `std::atomic_store` free functions for `shared_ptr`, which every standard library provides.
    template <class T>
    class AtomicSharedPtr {
      public:
        AtomicSharedPtr() = default;
        AtomicSharedPtr(const AtomicSharedPtr &) = delete;
        AtomicSharedPtr &operator=(const AtomicSharedPtr &) = delete;

        [[nodiscard]] std::shared_ptr<T> load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
#if defined(__cpp_lib_atomic_shared_ptr)
            return ptr_.load(order);
#else
            return std::atomic_load_explicit(&ptr_, order);
#endif
        }

        void store(std::shared_ptr<T> desired, std::memory_order order = std::memory_order_seq_cst) noexcept {
#if defined(__cpp_lib_atomic_shared_ptr)
            ptr_.store(std::move(desired), order);
#else
            std::atomic_store_explicit(&ptr_, std::move(desired), order);
#endif
        }

      private:
#if defined(__cpp_lib_atomic_shared_ptr)
        std::atomic<std::shared_ptr<T>> ptr_{};
#else
        // The free functions are deprecated in C++20 in favour of the atomic specialization this branch cannot use.
        mutable std::shared_ptr<T> ptr_{};
#endif
    };

} // namespace SFT::Foundation
