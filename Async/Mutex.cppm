module;

#pragma region Imports
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#pragma endregion

export module Sturdy.Async:Mutex;

using std::mutex;
using std::optional;
using std::unique_lock;

export namespace SFT::Async {

    template <typename T>
    class Mutex;

    // RAII handle returned by `Mutex<T>::lock()` / `try_lock()`. It is the *only* way to reach the
    // guarded value: transparent via `operator*`/`operator->`, and it releases the lock automatically
    // when it goes out of scope — there is no `unlock()` to remember to call. Move-only, so ownership
    // of the lock can be returned from a function or stashed in a container, but never accidentally
    // duplicated.
    template <typename T>
    class MutexGuard {
      public:
        MutexGuard(const MutexGuard &) = delete;
        MutexGuard &operator=(const MutexGuard &) = delete;
        MutexGuard(MutexGuard &&) noexcept = default;
        MutexGuard &operator=(MutexGuard &&) noexcept = default;
        ~MutexGuard() noexcept = default;

        [[nodiscard]] T &operator*() noexcept { return *value_; }
        [[nodiscard]] const T &operator*() const noexcept { return *value_; }
        [[nodiscard]] T *operator->() noexcept { return value_; }
        [[nodiscard]] const T *operator->() const noexcept { return value_; }

      private:
        friend class Mutex<T>;

        MutexGuard(unique_lock<mutex> lock, T *value) noexcept
            : lock_(std::move(lock)), value_(value) {
        }

        unique_lock<mutex> lock_;
        T *value_;
    };

    // A `std::mutex` that owns the data it protects, instead of protecting data the caller has to
    // remember to guard separately elsewhere. There is no way to reach `T` without holding the lock:
    // `lock()` blocks until acquired and returns a `MutexGuard<T>`; `try_lock()` is the non-blocking
    // form. Modeled on `std::mutex` closely enough to reason about the same way — in particular, like
    // `std::mutex`, a `Mutex<T>` is neither copyable nor movable (moving a locked mutex out from under
    // a live `MutexGuard` would be a use-after-free waiting to happen).
    template <typename T>
    class Mutex {
      public:
        Mutex() noexcept(std::is_nothrow_default_constructible_v<T>) = default;

        explicit Mutex(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
            : value_(std::move(value)) {
        }

        Mutex(const Mutex &) = delete;
        Mutex &operator=(const Mutex &) = delete;
        Mutex(Mutex &&) = delete;
        Mutex &operator=(Mutex &&) = delete;

        [[nodiscard]] MutexGuard<T> lock() noexcept {
            return MutexGuard<T>(unique_lock<mutex>(mutex_), &value_);
        }

        // Non-blocking form of `lock()` — `nullopt` if another thread already holds the lock.
        [[nodiscard]] optional<MutexGuard<T>> try_lock() noexcept {
            unique_lock<mutex> lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock()) {
                return std::nullopt;
            }
            return MutexGuard<T>(std::move(lock), &value_);
        }

      private:
        mutex mutex_;
        T value_{};
    };

} // namespace SFT::Async
