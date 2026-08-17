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

        MutexGuard(std::unique_lock<LockableBase(std::mutex)> lock, T *value) noexcept
            : lock_(std::move(lock)), value_(value) {}

        std::unique_lock<LockableBase(std::mutex)> lock_;
        T *value_ = nullptr;
    };

    /// `LockableBase(std::mutex)` (tracy/Tracy.hpp) is `tracy::Lockable<std::mutex>` when Tracy
    /// instrumentation is compiled in, or plain `std::mutex` otherwise — every `lock()`/`try_lock()`
    /// below goes through it unconditionally, so acquiring/holding/releasing this mutex shows up in
    /// Tracy's lock view with zero call-site changes anywhere already using `Mutex<T>`, and compiles
    /// down to exactly today's behavior when Tracy is off (`STURDY_ENABLE_TRACY`, off by default).
    ///
    /// One wrinkle specific to this being a template: `tracy::Lockable`'s displayed name is normally
    /// derived from the *source location where the lockable is declared* (see `TracyLockable`'s
    /// expansion) — fine for a single hand-written mutex, but every one of this template's
    /// instantiations across the whole codebase would otherwise collapse to the exact same generic
    /// "std::mutex mutex_" label at this one line, with no way to tell e.g. the Renderer's bloom-cache
    /// mutex apart from Scheduler's worker deque mutex in Tracy's lock view. `set_debug_name()` below
    /// opts a specific instance into its own label via Tracy's runtime `CustomName()` — call it once,
    /// right after construction, for any lock worth telling apart on the timeline (this codebase's own
    /// convention: name it at the hot/contended ones, e.g. Async::Scheduler's WorkerDeque; leave the
    /// rest under the shared generic label until they're worth distinguishing too).
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

        /// See class doc comment. `debug_name` must outlive this call (Tracy copies it internally, so
        /// a temporary/stack string is fine) — a no-op, including the null check, when Tracy is
        /// disabled or `debug_name` is null.
        void set_debug_name(const char *debug_name) noexcept {
#if defined(TRACY_ENABLE)
            if (debug_name != nullptr) {
                mutex_.CustomName(debug_name, std::strlen(debug_name));
            }
#else
            (void)debug_name;
#endif
        }

        [[nodiscard]] MutexGuard<T> lock() noexcept {
            return MutexGuard<T>(std::unique_lock<LockableBase(std::mutex)>(mutex_), std::addressof(value_));
        }

        [[nodiscard]] std::optional<MutexGuard<T>> try_lock() noexcept {
            std::unique_lock<LockableBase(std::mutex)> lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock()) {
                return std::nullopt;
            }
            return MutexGuard<T>(std::move(lock), std::addressof(value_));
        }

      private:
#if defined(TRACY_ENABLE)
        /// tracy::Lockable<T> has no default constructor — it requires a SourceLocationData* up
        /// front, which is normally supplied by the TracyLockable() macro at a single, fixed
        /// declaration site. mutex_'s NSDMI below plays that same role for every Mutex<T>
        /// instantiation, sharing one generic srcloc (see class doc comment for why per-instance
        /// names go through set_debug_name() instead of trying to give each instantiation its own
        /// compile-time source location).
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
