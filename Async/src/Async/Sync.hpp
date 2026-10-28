#pragma once

// Async-aware synchronization (tokio::sync): waiting never blocks a thread -- the waiter is a TaskHandle that the
// releasing side completes, so thousands of waiters cost memory, not threads. All of it composes with `co_await`.
// Waiters are served first-in first-out. Cancelling a waiter (CancellationToken) removes it without consuming anything.
//
// Closing / dropping the producing side fails pending waiters with the exceptions below.

#include <Async/Cancellation.hpp>
#include <Async/Mutex.hpp>
#include <Async/Promise.hpp>
#include <Async/Task.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace SFT::Async {

    class SemaphoreClosed final : public std::runtime_error {
      public:
        SemaphoreClosed() : std::runtime_error("Async semaphore is closed") {}
    };
    class WatchClosed final : public std::runtime_error {
      public:
        WatchClosed() : std::runtime_error("Async watch sender was dropped") {}
    };

    namespace Detail {
        template <typename Fn, typename A>
        struct MapInlineResult {
            using type = std::invoke_result_t<Fn &, A>;
        };
        template <typename Fn>
        struct MapInlineResult<Fn, void> {
            using type = std::invoke_result_t<Fn &>;
        };

        /// Completes the returned handle with `fn(value)` as soon as `source` finishes (on the completing thread).
        template <typename A, typename Fn>
        [[nodiscard]] auto map_inline(TaskHandle<A> source, Fn fn) {
            using B = typename MapInlineResult<Fn, A>::type;
            Promise<B> promise;
            source.on_done([source, promise, fn = std::move(fn)]() mutable {
                try {
                    if constexpr (std::is_void_v<A>) {
                        source.wait();
                        (void)promise.set_value(fn());
                    } else {
                        (void)promise.set_value(fn(source.wait()));
                    }
                } catch (...) {
                    (void)promise.set_exception(std::current_exception());
                }
            });
            return promise.handle();
        }

        template <typename H>
        struct HandleValue;
        template <typename R>
        struct HandleValue<TaskHandle<R>> {
            using type = R;
        };

        /// Registers `on_cancel` for a parked waiter and keeps the registration alive until the waiter's handle completes.
        template <typename T, typename Fn>
        void arm_cancellation(const CancellationToken &token, const TaskHandle<T> &handle, Fn on_cancel) {
            if (!token.can_be_cancelled()) return;
            auto registration = std::make_shared<CancellationRegistration>(token.on_cancel(std::move(on_cancel)));
            handle.on_done([registration] {});
        }
    } // namespace Detail

    // =============================================================================================
    // Semaphore
    // =============================================================================================
    class Semaphore {
        struct State;

      public:
        /// Holds `count` permits; returns them on destruction.
        class Permit {
          public:
            Permit() noexcept = default;
            ~Permit() { release(); }
            Permit(const Permit &) = delete;
            Permit &operator=(const Permit &) = delete;
            Permit(Permit &&other) noexcept : state_(std::move(other.state_)), count_(std::exchange(other.count_, 0)) {}
            Permit &operator=(Permit &&other) noexcept {
                if (this != &other) {
                    release();
                    state_ = std::move(other.state_);
                    count_ = std::exchange(other.count_, 0);
                }
                return *this;
            }
            [[nodiscard]] std::size_t count() const noexcept { return count_; }
            /// Returns the permits now.
            void release() noexcept;
            /// Gives up ownership without returning the permits (they stay consumed, as in `tokio::Semaphore::forget`).
            void forget() noexcept {
                state_.reset();
                count_ = 0;
            }

          private:
            friend class Semaphore;
            Permit(std::shared_ptr<State> state, std::size_t count) noexcept : state_(std::move(state)), count_(count) {}
            std::shared_ptr<State> state_;
            std::size_t count_ = 0;
        };

        explicit Semaphore(std::size_t permits) : state_(std::make_shared<State>(permits)) {}

        /// Waits for `count` permits. Fails with `SemaphoreClosed` if the semaphore closes first.
        [[nodiscard]] TaskHandle<Permit> acquire(std::size_t count = 1, CancellationToken cancellation = {}) const;
        [[nodiscard]] std::optional<Permit> try_acquire(std::size_t count = 1) const;
        void add_permits(std::size_t count) const;
        /// Fails every waiter and every future acquire.
        void close() const;
        [[nodiscard]] std::size_t available_permits() const;
        [[nodiscard]] bool is_closed() const;

      private:
        struct Waiter {
            std::size_t need;
            Promise<Permit> promise;
        };
        struct Inner {
            explicit Inner(std::size_t initial) : permits(initial) {}
            std::size_t permits;
            bool closed = false;
            std::deque<Waiter> waiters;
        };
        struct State : std::enable_shared_from_this<State> {
            explicit State(std::size_t permits) : inner(Inner{permits}) {}
            Mutex<Inner> inner;

            /// Called after permits are added: grants waiters in order while the head fits.
            void grant() {
                std::vector<std::pair<Promise<Permit>, std::size_t>> granted;
                {
                    auto guard = inner.lock();
                    while (!guard->waiters.empty() && guard->waiters.front().need <= guard->permits) {
                        guard->permits -= guard->waiters.front().need;
                        granted.emplace_back(std::move(guard->waiters.front().promise), guard->waiters.front().need);
                        guard->waiters.pop_front();
                    }
                }
                for (auto &[promise, need] : granted) (void)promise.set_value(Permit(this->shared_from_this(), need));
            }
        };
        std::shared_ptr<State> state_;
        friend class Permit;
    };

    inline void Semaphore::Permit::release() noexcept {
        if (state_ && count_ > 0) {
            try {
                {
                    auto guard = state_->inner.lock();
                    guard->permits += count_;
                }
                state_->grant();
            } catch (...) {
            }
        }
        state_.reset();
        count_ = 0;
    }

    inline TaskHandle<Semaphore::Permit> Semaphore::acquire(std::size_t count, CancellationToken cancellation) const {
        Promise<Permit> promise;
        auto handle = promise.handle();
        if (cancellation.is_cancelled()) {
            (void)promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
            return handle;
        }
        bool parked = false;
        std::optional<Permit> immediate;
        {
            auto guard = state_->inner.lock();
            if (guard->closed) {
                (void)promise.set_exception(std::make_exception_ptr(SemaphoreClosed{}));
                return handle;
            }
            if (guard->waiters.empty() && guard->permits >= count) {
                guard->permits -= count;
                immediate = Permit(state_, count);
            } else {
                guard->waiters.push_back({count, promise});
                parked = true;
            }
        }
        if (immediate) {
            (void)promise.set_value(std::move(*immediate));
        } else if (parked) {
            Detail::arm_cancellation(cancellation, handle, [state = state_, promise]() mutable {
                bool removed = false;
                {
                    auto guard = state->inner.lock();
                    removed = std::erase_if(guard->waiters, [&](const Waiter &w) { return w.promise.same_as(promise); }) > 0;
                }
                if (removed) {
                    (void)promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                    state->grant(); // the cancelled waiter may have been blocking smaller ones behind it
                }
            });
        }
        return handle;
    }

    inline std::optional<Semaphore::Permit> Semaphore::try_acquire(std::size_t count) const {
        auto guard = state_->inner.lock();
        if (guard->closed || !guard->waiters.empty() || guard->permits < count) return std::nullopt;
        guard->permits -= count;
        return Permit(state_, count);
    }

    inline void Semaphore::add_permits(std::size_t count) const {
        {
            auto guard = state_->inner.lock();
            guard->permits += count;
        }
        state_->grant();
    }

    inline void Semaphore::close() const {
        std::deque<Waiter> waiters;
        {
            auto guard = state_->inner.lock();
            guard->closed = true;
            waiters.swap(guard->waiters);
        }
        for (auto &w : waiters) (void)w.promise.set_exception(std::make_exception_ptr(SemaphoreClosed{}));
    }

    inline std::size_t Semaphore::available_permits() const { return state_->inner.lock()->permits; }
    inline bool Semaphore::is_closed() const { return state_->inner.lock()->closed; }

    // =============================================================================================
    // AsyncMutex / AsyncRwLock: the data is only reachable through a guard.
    // =============================================================================================
    template <typename T>
    class AsyncMutex {
        struct Shared {
            explicit Shared(T v) : value(std::move(v)), gate(1) {}
            T value;
            Semaphore gate;
        };

      public:
        class Guard {
          public:
            Guard(Guard &&) noexcept = default;
            Guard &operator=(Guard &&) noexcept = default;
            [[nodiscard]] T &operator*() noexcept { return shared_->value; }
            [[nodiscard]] T *operator->() noexcept { return &shared_->value; }

          private:
            friend class AsyncMutex;
            Guard(std::shared_ptr<Shared> shared, Semaphore::Permit permit) : shared_(std::move(shared)), permit_(std::move(permit)) {}
            std::shared_ptr<Shared> shared_;
            Semaphore::Permit permit_;
        };

        explicit AsyncMutex(T value = T{}) : shared_(std::make_shared<Shared>(std::move(value))) {}

        /// Waits for exclusive access without blocking a thread. Hold the guard across `co_await`s freely.
        [[nodiscard]] TaskHandle<Guard> lock(CancellationToken cancellation = {}) const {
            return Detail::map_inline(shared_->gate.acquire(1, std::move(cancellation)),
                                      [shared = shared_](Semaphore::Permit permit) { return Guard(shared, std::move(permit)); });
        }
        [[nodiscard]] std::optional<Guard> try_lock() const {
            if (auto permit = shared_->gate.try_acquire()) return Guard(shared_, std::move(*permit));
            return std::nullopt;
        }

      private:
        std::shared_ptr<Shared> shared_;
    };

    /// Many readers or one writer; waiters queue in order, so a waiting writer is not starved by later readers.
    template <typename T>
    class AsyncRwLock {
        static constexpr std::size_t kMaxReaders = std::size_t{1} << 30;
        struct Shared {
            explicit Shared(T v) : value(std::move(v)), gate(kMaxReaders) {}
            T value;
            Semaphore gate;
        };

      public:
        class ReadGuard {
          public:
            ReadGuard(ReadGuard &&) noexcept = default;
            ReadGuard &operator=(ReadGuard &&) noexcept = default;
            [[nodiscard]] const T &operator*() const noexcept { return shared_->value; }
            [[nodiscard]] const T *operator->() const noexcept { return &shared_->value; }

          private:
            friend class AsyncRwLock;
            ReadGuard(std::shared_ptr<Shared> shared, Semaphore::Permit permit) : shared_(std::move(shared)), permit_(std::move(permit)) {}
            std::shared_ptr<Shared> shared_;
            Semaphore::Permit permit_;
        };
        class WriteGuard {
          public:
            WriteGuard(WriteGuard &&) noexcept = default;
            WriteGuard &operator=(WriteGuard &&) noexcept = default;
            [[nodiscard]] T &operator*() noexcept { return shared_->value; }
            [[nodiscard]] T *operator->() noexcept { return &shared_->value; }

          private:
            friend class AsyncRwLock;
            WriteGuard(std::shared_ptr<Shared> shared, Semaphore::Permit permit) : shared_(std::move(shared)), permit_(std::move(permit)) {}
            std::shared_ptr<Shared> shared_;
            Semaphore::Permit permit_;
        };

        explicit AsyncRwLock(T value = T{}) : shared_(std::make_shared<Shared>(std::move(value))) {}

        [[nodiscard]] TaskHandle<ReadGuard> read(CancellationToken cancellation = {}) const {
            return Detail::map_inline(shared_->gate.acquire(1, std::move(cancellation)),
                                      [shared = shared_](Semaphore::Permit permit) { return ReadGuard(shared, std::move(permit)); });
        }
        [[nodiscard]] TaskHandle<WriteGuard> write(CancellationToken cancellation = {}) const {
            return Detail::map_inline(shared_->gate.acquire(kMaxReaders, std::move(cancellation)),
                                      [shared = shared_](Semaphore::Permit permit) { return WriteGuard(shared, std::move(permit)); });
        }

      private:
        std::shared_ptr<Shared> shared_;
    };

    // =============================================================================================
    // Notify: a wake-up signal with at most one stored permit (tokio::sync::Notify).
    // =============================================================================================
    class Notify {
        struct Inner {
            bool permit = false;
            std::deque<Promise<void>> waiters;
        };

      public:
        Notify() : state_(std::make_shared<Mutex<Inner>>()) {}

        /// Completes when notified. A `notify_one` that happened earlier (with nobody waiting) is consumed first.
        [[nodiscard]] TaskHandle<void> notified(CancellationToken cancellation = {}) const {
            Promise<void> promise;
            auto handle = promise.handle();
            bool parked = false;
            {
                auto guard = state_->lock();
                if (guard->permit) {
                    guard->permit = false;
                    (void)promise.set_value();
                } else {
                    guard->waiters.push_back(promise);
                    parked = true;
                }
            }
            if (parked) {
                Detail::arm_cancellation(cancellation, handle, [state = state_, promise]() mutable {
                    bool removed = false;
                    {
                        auto guard = state->lock();
                        removed = std::erase_if(guard->waiters, [&](const Promise<void> &p) { return p.same_as(promise); }) > 0;
                    }
                    if (removed) (void)promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                });
            }
            return handle;
        }

        /// Wakes the longest-waiting waiter, or stores one permit for the next `notified`.
        void notify_one() const {
            std::optional<Promise<void>> waiter;
            {
                auto guard = state_->lock();
                if (guard->waiters.empty()) {
                    guard->permit = true;
                } else {
                    waiter = std::move(guard->waiters.front());
                    guard->waiters.pop_front();
                }
            }
            if (waiter) (void)waiter->set_value();
        }

        /// Wakes everyone waiting now; stores nothing.
        void notify_waiters() const {
            std::deque<Promise<void>> waiters;
            {
                auto guard = state_->lock();
                waiters.swap(guard->waiters);
            }
            for (auto &w : waiters) (void)w.set_value();
        }

      private:
        std::shared_ptr<Mutex<Inner>> state_;
    };

    // =============================================================================================
    // Barrier
    // =============================================================================================
    class Barrier {
        struct Inner {
            explicit Inner(std::size_t count) : parties(count) {}
            std::size_t parties;
            std::size_t arrived = 0;
            std::vector<Promise<bool>> waiting;
        };

      public:
        explicit Barrier(std::size_t parties) : state_(std::make_shared<Mutex<Inner>>(Inner{(std::max)(parties, std::size_t{1})})) {}

        /// Completes when `parties` callers have arrived; resolves to true for exactly one of them (the last).
        [[nodiscard]] TaskHandle<bool> wait() const {
            Promise<bool> promise;
            auto handle = promise.handle();
            std::vector<Promise<bool>> release;
            bool leader = false;
            {
                auto guard = state_->lock();
                if (++guard->arrived >= guard->parties) {
                    release.swap(guard->waiting);
                    guard->arrived = 0;
                    leader = true;
                } else {
                    guard->waiting.push_back(promise);
                }
            }
            if (leader) {
                for (auto &p : release) (void)p.set_value(false);
                (void)promise.set_value(true);
            }
            return handle;
        }

      private:
        std::shared_ptr<Mutex<Inner>> state_;
    };

    // =============================================================================================
    // oneshot
    // =============================================================================================
    template <typename T>
    struct OneshotPair {
        Promise<T> sender;       ///< `set_value` / `set_exception` once
        TaskHandle<T> receiver;  ///< `co_await` or `wait()`
    };

    template <typename T>
    [[nodiscard]] OneshotPair<T> oneshot() {
        Promise<T> promise;
        auto handle = promise.handle();
        return {std::move(promise), std::move(handle)};
    }

    // =============================================================================================
    // watch: a single value many tasks can observe changes of (tokio::sync::watch).
    // =============================================================================================
    namespace Detail {
        template <typename T>
        struct WatchState {
            struct Waiter {
                std::shared_ptr<std::atomic<std::uint64_t>> seen; ///< the waiting receiver's position, advanced on wake
                Promise<void> promise;
            };
            struct Inner {
                explicit Inner(T initial) : value(std::move(initial)) {}
                T value;
                std::uint64_t version = 1;
                bool closed = false;
                std::deque<Waiter> waiters;
            };
            explicit WatchState(T initial) : inner(Inner{std::move(initial)}) {}
            Mutex<Inner> inner;
            std::atomic<std::size_t> senders{1};
        };
    } // namespace Detail

    template <typename T>
    class WatchReceiver {
      public:
        explicit WatchReceiver(std::shared_ptr<Detail::WatchState<T>> state)
            : state_(std::move(state)), seen_(std::make_shared<std::atomic<std::uint64_t>>(state_->inner.lock()->version)) {}
        /// A copy starts from the same "seen" position but tracks it independently.
        WatchReceiver(const WatchReceiver &other) : state_(other.state_), seen_(std::make_shared<std::atomic<std::uint64_t>>(other.seen_->load())) {}
        WatchReceiver &operator=(const WatchReceiver &other) {
            state_ = other.state_;
            seen_ = std::make_shared<std::atomic<std::uint64_t>>(other.seen_->load());
            return *this;
        }
        WatchReceiver(WatchReceiver &&) noexcept = default;
        WatchReceiver &operator=(WatchReceiver &&) noexcept = default;

        /// A copy of the current value; does not mark it as seen.
        [[nodiscard]] T borrow() const { return state_->inner.lock()->value; }
        /// A copy of the current value, marked as seen.
        [[nodiscard]] T borrow_and_update() {
            auto guard = state_->inner.lock();
            seen_->store(guard->version);
            return guard->value;
        }
        [[nodiscard]] bool has_changed() const { return state_->inner.lock()->version != seen_->load(); }

        /// Completes when the value changed since this receiver last looked, and marks that version as seen. Fails
        /// with `WatchClosed` once the sender is gone and nothing new remains.
        [[nodiscard]] TaskHandle<void> changed(CancellationToken cancellation = {}) {
            Promise<void> promise;
            auto handle = promise.handle();
            bool parked = false;
            {
                auto guard = state_->inner.lock();
                if (guard->version != seen_->load()) {
                    seen_->store(guard->version);
                    (void)promise.set_value();
                } else if (guard->closed) {
                    (void)promise.set_exception(std::make_exception_ptr(WatchClosed{}));
                } else {
                    guard->waiters.push_back({seen_, promise});
                    parked = true;
                }
            }
            if (parked) {
                Detail::arm_cancellation(cancellation, handle, [state = state_, promise]() mutable {
                    bool removed = false;
                    {
                        auto guard = state->inner.lock();
                        removed = std::erase_if(guard->waiters, [&](const typename Detail::WatchState<T>::Waiter &w) { return w.promise.same_as(promise); }) > 0;
                    }
                    if (removed) (void)promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                });
            }
            return handle;
        }

      private:
        std::shared_ptr<Detail::WatchState<T>> state_;
        std::shared_ptr<std::atomic<std::uint64_t>> seen_;
    };

    template <typename T>
    class WatchSender {
      public:
        explicit WatchSender(std::shared_ptr<Detail::WatchState<T>> state) : state_(std::move(state)) {}
        WatchSender(const WatchSender &other) : state_(other.state_) { state_->senders.fetch_add(1, std::memory_order_relaxed); }
        WatchSender(WatchSender &&other) noexcept = default;
        WatchSender &operator=(const WatchSender &) = delete;
        ~WatchSender() {
            if (state_ && state_->senders.fetch_sub(1, std::memory_order_acq_rel) == 1) close();
        }

        /// Replaces the value and wakes everyone waiting in `changed()`.
        void send(T value) const {
            std::deque<typename Detail::WatchState<T>::Waiter> woken;
            {
                auto guard = state_->inner.lock();
                guard->value = std::move(value);
                ++guard->version;
                for (auto &w : guard->waiters) w.seen->store(guard->version);
                woken.swap(guard->waiters);
            }
            for (auto &w : woken) (void)w.promise.set_value();
        }
        /// Modifies the value in place and wakes waiters.
        template <typename Fn>
        void modify(Fn fn) const {
            std::deque<typename Detail::WatchState<T>::Waiter> woken;
            {
                auto guard = state_->inner.lock();
                fn(guard->value);
                ++guard->version;
                for (auto &w : guard->waiters) w.seen->store(guard->version);
                woken.swap(guard->waiters);
            }
            for (auto &w : woken) (void)w.promise.set_value();
        }
        [[nodiscard]] WatchReceiver<T> subscribe() const { return WatchReceiver<T>(state_); }

      private:
        void close() {
            std::deque<typename Detail::WatchState<T>::Waiter> woken;
            {
                auto guard = state_->inner.lock();
                guard->closed = true;
                woken.swap(guard->waiters);
            }
            for (auto &w : woken) (void)w.promise.set_exception(std::make_exception_ptr(WatchClosed{}));
        }
        std::shared_ptr<Detail::WatchState<T>> state_;
    };

    template <typename T>
    struct WatchPair {
        WatchSender<T> sender;
        WatchReceiver<T> receiver;
    };

    template <typename T>
    [[nodiscard]] WatchPair<T> watch(T initial) {
        auto state = std::make_shared<Detail::WatchState<T>>(std::move(initial));
        return {WatchSender<T>(state), WatchReceiver<T>(state)};
    }

    // =============================================================================================
    // broadcast: every receiver sees every message (tokio::sync::broadcast). A receiver that falls more than
    // `capacity` messages behind is told how many it missed (Lagged) instead of stalling the sender.
    // =============================================================================================
    template <typename T>
    struct BroadcastRecv {
        enum class Kind { Value, Lagged, Closed } kind = Kind::Closed;
        std::optional<T> value;
        std::uint64_t skipped = 0; ///< for Lagged
    };

    namespace Detail {
        template <typename T>
        struct BroadcastState {
            struct Waiter {
                Promise<BroadcastRecv<T>> promise;
                std::shared_ptr<std::atomic<std::uint64_t>> cursor;
            };
            struct Inner {
                explicit Inner(std::size_t slots) : capacity(slots) {}
                std::size_t capacity;
                std::uint64_t head = 0; ///< sequence number of the oldest retained message
                std::deque<T> messages;
                bool closed = false;
                std::deque<Waiter> waiters;
            };
            explicit BroadcastState(std::size_t capacity) : inner(Inner{(std::max)(capacity, std::size_t{1})}) {}
            Mutex<Inner> inner;
            std::atomic<std::size_t> senders{1};
            std::atomic<std::size_t> receivers{0};
        };
    } // namespace Detail

    template <typename T>
    class BroadcastReceiver {
      public:
        BroadcastReceiver(std::shared_ptr<Detail::BroadcastState<T>> state, std::uint64_t start)
            : state_(std::move(state)), cursor_(std::make_shared<std::atomic<std::uint64_t>>(start)) {
            state_->receivers.fetch_add(1, std::memory_order_relaxed);
        }
        BroadcastReceiver(const BroadcastReceiver &other) : state_(other.state_), cursor_(std::make_shared<std::atomic<std::uint64_t>>(other.cursor_->load())) {
            state_->receivers.fetch_add(1, std::memory_order_relaxed);
        }
        BroadcastReceiver(BroadcastReceiver &&) noexcept = default;
        BroadcastReceiver &operator=(BroadcastReceiver &&) noexcept = default;
        ~BroadcastReceiver() {
            if (state_) state_->receivers.fetch_sub(1, std::memory_order_relaxed);
        }

        /// The next message for this receiver, `Lagged` (with the count skipped) if it fell behind, or `Closed`.
        [[nodiscard]] TaskHandle<BroadcastRecv<T>> recv() {
            Promise<BroadcastRecv<T>> promise;
            auto handle = promise.handle();
            {
                auto guard = state_->inner.lock();
                if (auto ready = try_take(*guard)) {
                    (void)promise.set_value(std::move(*ready));
                    return handle;
                }
                if (guard->closed) {
                    (void)promise.set_value(BroadcastRecv<T>{});
                    return handle;
                }
                guard->waiters.push_back({promise, cursor_});
            }
            return handle;
        }

      private:
        friend class BroadcastSenderAccess;
        std::optional<BroadcastRecv<T>> try_take(typename Detail::BroadcastState<T>::Inner &inner) {
            return take_for(inner, *cursor_);
        }

      public:
        static std::optional<BroadcastRecv<T>> take_for(typename Detail::BroadcastState<T>::Inner &inner, std::atomic<std::uint64_t> &cursor) {
            const std::uint64_t at = cursor.load();
            const std::uint64_t tail = inner.head + inner.messages.size();
            if (at < inner.head) {
                BroadcastRecv<T> lagged;
                lagged.kind = BroadcastRecv<T>::Kind::Lagged;
                lagged.skipped = inner.head - at;
                cursor.store(inner.head);
                return lagged;
            }
            if (at < tail) {
                BroadcastRecv<T> out;
                out.kind = BroadcastRecv<T>::Kind::Value;
                out.value = inner.messages[static_cast<std::size_t>(at - inner.head)];
                cursor.store(at + 1);
                return out;
            }
            return std::nullopt;
        }

      private:
        std::shared_ptr<Detail::BroadcastState<T>> state_;
        std::shared_ptr<std::atomic<std::uint64_t>> cursor_;
    };

    template <typename T>
    class BroadcastSender {
      public:
        explicit BroadcastSender(std::shared_ptr<Detail::BroadcastState<T>> state) : state_(std::move(state)) {}
        BroadcastSender(const BroadcastSender &other) : state_(other.state_) { state_->senders.fetch_add(1, std::memory_order_relaxed); }
        BroadcastSender(BroadcastSender &&) noexcept = default;
        BroadcastSender &operator=(const BroadcastSender &) = delete;
        ~BroadcastSender() {
            if (state_ && state_->senders.fetch_sub(1, std::memory_order_acq_rel) == 1) close();
        }

        /// Publishes `value` to every receiver; resolves to the number of live receivers.
        std::size_t send(T value) const {
            std::vector<std::pair<Promise<BroadcastRecv<T>>, BroadcastRecv<T>>> wake;
            {
                auto guard = state_->inner.lock();
                guard->messages.push_back(std::move(value));
                if (guard->messages.size() > guard->capacity) {
                    guard->messages.pop_front();
                    ++guard->head;
                }
                for (auto it = guard->waiters.begin(); it != guard->waiters.end();) {
                    if (auto ready = BroadcastReceiver<T>::take_for(*guard, *it->cursor)) {
                        wake.emplace_back(std::move(it->promise), std::move(*ready));
                        it = guard->waiters.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (auto &[promise, message] : wake) (void)promise.set_value(std::move(message));
            return state_->receivers.load(std::memory_order_relaxed);
        }
        /// A receiver that sees messages sent from now on.
        [[nodiscard]] BroadcastReceiver<T> subscribe() const {
            auto guard = state_->inner.lock();
            return BroadcastReceiver<T>(state_, guard->head + guard->messages.size());
        }

      private:
        void close() {
            std::deque<typename Detail::BroadcastState<T>::Waiter> waiters;
            {
                auto guard = state_->inner.lock();
                guard->closed = true;
                waiters.swap(guard->waiters);
            }
            for (auto &w : waiters) (void)w.promise.set_value(BroadcastRecv<T>{});
        }
        std::shared_ptr<Detail::BroadcastState<T>> state_;
    };

    template <typename T>
    [[nodiscard]] std::pair<BroadcastSender<T>, BroadcastReceiver<T>> broadcast(std::size_t capacity) {
        auto state = std::make_shared<Detail::BroadcastState<T>>(capacity);
        BroadcastSender<T> sender(state);
        auto receiver = sender.subscribe();
        return {std::move(sender), std::move(receiver)};
    }

} // namespace SFT::Async
