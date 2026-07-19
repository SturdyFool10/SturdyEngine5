#pragma once

#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace SFT::Async {

    template <typename T>
    class Mutex;

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

        MutexGuard(std::unique_lock<std::mutex> lock, T *value) noexcept
            : lock_(std::move(lock)), value_(value) {}

        std::unique_lock<std::mutex> lock_;
        T *value_ = nullptr;
    };

    template <typename T>
    class Mutex {
      public:
        Mutex() noexcept(std::is_nothrow_default_constructible_v<T>) = default;

        explicit Mutex(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
            : value_(std::move(value)) {}

        Mutex(const Mutex &) = delete;
        Mutex &operator=(const Mutex &) = delete;
        Mutex(Mutex &&) = delete;
        Mutex &operator=(Mutex &&) = delete;

        [[nodiscard]] MutexGuard<T> lock() noexcept {
            return MutexGuard<T>(std::unique_lock<std::mutex>(mutex_), &value_);
        }

        [[nodiscard]] std::optional<MutexGuard<T>> try_lock() noexcept {
            std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock()) {
                return std::nullopt;
            }
            return MutexGuard<T>(std::move(lock), &value_);
        }

      private:
        std::mutex mutex_;
        T value_{};
    };

} // namespace SFT::Async
