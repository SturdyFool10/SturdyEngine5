#pragma once

#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

#include <tracy/Tracy.hpp>

namespace SFT::Async {

    template <typename T>
    class Mutex;

    template <typename T>
    class MutexGuard {
      public:
        /// Disables this construction form for `MutexGuard`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        MutexGuard(const MutexGuard &) = delete;
        /// Assigns a new value to this `MutexGuard`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        MutexGuard &operator=(const MutexGuard &) = delete;
        /// Constructs a `MutexGuard` from another instance.
        ///
        /// @note This function does not throw exceptions.
        MutexGuard(MutexGuard &&) noexcept = default;
        /// Assigns a new value to this `MutexGuard`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        MutexGuard &operator=(MutexGuard &&) noexcept = default;
        /// Destroys the `MutexGuard` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~MutexGuard() noexcept = default;

        /// Dereferences this iterator or handle.
        ///
        /// @return Returns the value or reference currently addressed by the iterator/handle.
        /// @note This function does not throw exceptions.
        [[nodiscard]] T &operator*() noexcept { return *value_; }
        /// Dereferences this iterator or handle.
        ///
        /// @return Returns the value or reference currently addressed by the iterator/handle.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const T &operator*() const noexcept { return *value_; }
        /// Accesses the object referenced by this `MutexGuard`.
        ///
        /// @return Returns a pointer through which the referenced object can be accessed.
        /// @note This function does not throw exceptions.
        [[nodiscard]] T *operator->() noexcept { return value_; }
        /// Accesses the object referenced by this `MutexGuard`.
        ///
        /// @return Returns a pointer through which the referenced object can be accessed.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const T *operator->() const noexcept { return value_; }

      private:
        friend class Mutex<T>;

        /// Constructs a `MutexGuard` from the supplied initialization values.
        ///
        /// @param lock `lock` value used by the operation.
        /// @param value Value consumed by the operation.
        ///
        /// @note This function does not throw exceptions.
        MutexGuard(std::unique_lock<LockableBase(std::mutex)> lock, T *value) noexcept
            : lock_(std::move(lock)), value_(value) {}

        std::unique_lock<LockableBase(std::mutex)> lock_;
        T *value_ = nullptr;
    };


    template <typename T>
    class Mutex {
      public:
        /// Constructs a `Mutex` in its default state.
        ///
        /// @note This function does not throw exceptions.
        Mutex() noexcept(std::is_nothrow_default_constructible_v<T>) = default;

        /// Constructs a `Mutex` from the supplied initialization values.
        ///
        /// @param value Value consumed by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit Mutex(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
            : value_(std::move(value)) {}

        /// Disables this construction form for `Mutex`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Mutex(const Mutex &) = delete;
        /// Assigns a new value to this `Mutex`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Mutex &operator=(const Mutex &) = delete;
        /// Disables this construction form for `Mutex`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Mutex(Mutex &&) = delete;
        /// Assigns a new value to this `Mutex`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Mutex &operator=(Mutex &&) = delete;


        /// Returns a human-readable name for the supplied set debug value.
        ///
        /// @param debug_name Name used to identify or label the target.
        ///
        /// @note This function does not throw exceptions.
        void set_debug_name(const char *debug_name) noexcept {
#if defined(TRACY_ENABLE)
            if (debug_name != nullptr) {
                mutex_.CustomName(debug_name, std::strlen(debug_name));
            }
#else
            (void)debug_name;
#endif
        }

        /// Acquires the associated synchronization primitive before protected access.
        ///
        /// @return Returns the current lock value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] MutexGuard<T> lock() noexcept {
            return MutexGuard<T>(std::unique_lock<LockableBase(std::mutex)>(mutex_), std::addressof(value_));
        }

        /// Attempts to lock without requiring normal failure to be exceptional.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::optional<MutexGuard<T>> try_lock() noexcept {
            std::unique_lock<LockableBase(std::mutex)> lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock()) {
                return std::nullopt;
            }
            return MutexGuard<T>(std::move(lock), std::addressof(value_));
        }

      private:
#if defined(TRACY_ENABLE)


        /// Returns the current or globally available mutex source location value.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static const tracy::SourceLocationData *mutex_source_location() noexcept {
            static constexpr tracy::SourceLocationData srcloc{nullptr, "SFT::Async::Mutex", __FILE__, __LINE__, 0};
            return &srcloc;
        }

        LockableBase(std::mutex) mutex_{mutex_source_location()};
#else
        LockableBase(std::mutex) mutex_;
#endif
        T value_{};
    };

} // namespace SFT::Async
