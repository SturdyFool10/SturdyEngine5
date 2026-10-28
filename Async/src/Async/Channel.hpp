#pragma once

#include <Async/Cancellation.hpp>
#include <Async/Mutex.hpp>
#include <Async/Promise.hpp>
#include <Async/Task.hpp>

#include <atomic>
#include <cstddef>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace SFT::Async {

    /// Failure of a channel send: `send` on a closed channel resolves to this; `try_send` reports it as a value.
    class ChannelClosed final : public std::runtime_error {
      public:
        ChannelClosed() : std::runtime_error("Async channel is closed") {}
    };

    enum class TrySendError { Full, Closed };
    enum class TryRecvError { Empty, Closed };

    namespace Detail {

        template <typename T>
        struct ChannelState {
            struct ParkedSender {
                T value;
                Promise<void> done;
            };
            struct Inner {
                explicit Inner(std::size_t max_buffered) : capacity(max_buffered) {}
                std::size_t capacity;
                bool closed = false;
                std::deque<T> queue;
                std::deque<Promise<std::optional<T>>> receivers;
                std::deque<ParkedSender> senders;
            };

            explicit ChannelState(std::size_t capacity) : inner(Inner{capacity == 0 ? 1 : capacity}) {}

            Mutex<Inner> inner;
            std::atomic<std::size_t> sender_count{0};

            /// Closes the channel: no new sends succeed, buffered values stay receivable, and everything
            /// parked is released (receivers see end-of-stream, senders see `ChannelClosed`).
            void close() {
                std::deque<Promise<std::optional<T>>> receivers;
                std::deque<ParkedSender> senders;
                {
                    auto guard = inner.lock();
                    if (guard->closed) {
                        return;
                    }
                    guard->closed = true;
                    // Parked receivers imply an empty queue, so end-of-stream is accurate for them.
                    receivers.swap(guard->receivers);
                    senders.swap(guard->senders);
                }
                for (auto &r : receivers) {
                    (void)r.set_value(std::optional<T>{});
                }
                for (auto &s : senders) {
                    (void)s.done.set_exception(std::make_exception_ptr(ChannelClosed{}));
                }
            }
        };

    } // namespace Detail

    /// Receiving end of a bounded multi-producer channel. Cheap to copy; copies share the same queue and
    /// compete for values (each value is delivered once). Dropping every `Receiver` is not detected; call
    /// `close()` to release blocked senders.
    template <typename T>
    class Receiver {
      public:
        Receiver() = default;
        explicit Receiver(std::shared_ptr<Detail::ChannelState<T>> state) : state_(std::move(state)) {}

        /// Resolves to the next value, or to an empty optional once the channel is closed and drained.
        /// Never blocks a thread; pending receives are served in FIFO order.
        /// Cancelling `cancellation` removes a waiting receive from the queue (it fails with `IoOperationCancelled`)
        /// without consuming a value, so no message is ever lost to a cancelled receive.
        [[nodiscard]] TaskHandle<std::optional<T>> recv(CancellationToken cancellation = {}) const {
            Promise<std::optional<T>> promise;
            std::optional<Promise<void>> unblocked;
            bool parked = false;
            {
                auto guard = state_->inner.lock();
                auto &s = *guard;
                if (!s.queue.empty()) {
                    T value = std::move(s.queue.front());
                    s.queue.pop_front();
                    if (!s.senders.empty()) {
                        s.queue.push_back(std::move(s.senders.front().value));
                        unblocked = std::move(s.senders.front().done);
                        s.senders.pop_front();
                    }
                    (void)promise.set_value(std::optional<T>(std::move(value)));
                } else if (s.closed) {
                    (void)promise.set_value(std::optional<T>{});
                } else {
                    s.receivers.push_back(promise);
                    parked = true;
                }
            }
            if (unblocked) {
                (void)unblocked->set_value();
            }
            auto handle = promise.handle();
            if (parked && cancellation.can_be_cancelled()) {
                auto registration = std::make_shared<CancellationRegistration>(cancellation.on_cancel([state = state_, promise]() mutable {
                    bool removed = false;
                    {
                        auto guard = state->inner.lock();
                        removed = std::erase_if(guard->receivers, [&](const Promise<std::optional<T>> &p) { return p.same_as(promise); }) > 0;
                    }
                    if (removed) (void)promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                }));
                handle.on_done([registration] {}); // keeps the registration armed until the receive completes
            }
            return handle;
        }

        [[nodiscard]] std::expected<T, TryRecvError> try_recv() const {
            std::optional<Promise<void>> unblocked;
            std::optional<T> value;
            bool closed = false;
            {
                auto guard = state_->inner.lock();
                auto &s = *guard;
                if (!s.queue.empty()) {
                    value = std::move(s.queue.front());
                    s.queue.pop_front();
                    if (!s.senders.empty()) {
                        s.queue.push_back(std::move(s.senders.front().value));
                        unblocked = std::move(s.senders.front().done);
                        s.senders.pop_front();
                    }
                } else {
                    closed = s.closed;
                }
            }
            if (unblocked) {
                (void)unblocked->set_value();
            }
            if (value) {
                return std::move(*value);
            }
            return std::unexpected(closed ? TryRecvError::Closed : TryRecvError::Empty);
        }

        void close() const { state_->close(); }

      private:
        std::shared_ptr<Detail::ChannelState<T>> state_;
    };

    /// Sending end. The channel closes automatically when the last `Sender` is destroyed, so receivers
    /// drain what remains and then see end-of-stream.
    template <typename T>
    class Sender {
      public:
        Sender() = default;
        explicit Sender(std::shared_ptr<Detail::ChannelState<T>> state) : state_(std::move(state)) { retain(); }
        Sender(const Sender &other) : state_(other.state_) { retain(); }
        Sender(Sender &&other) noexcept : state_(std::move(other.state_)) {}
        Sender &operator=(Sender other) noexcept {
            std::swap(state_, other.state_);
            return *this;
        }
        ~Sender() { release(); }

        /// Resolves once the value is queued or handed to a receiver. When the buffer is full it waits for
        /// space (backpressure) without holding a thread. Fails with `ChannelClosed` if the channel closed.
        [[nodiscard]] TaskHandle<void> send(T value) const {
            Promise<void> promise;
            std::optional<Promise<std::optional<T>>> receiver;
            {
                auto guard = state_->inner.lock();
                auto &s = *guard;
                if (s.closed) {
                    (void)promise.set_exception(std::make_exception_ptr(ChannelClosed{}));
                    return promise.handle();
                }
                if (!s.receivers.empty()) {
                    receiver = std::move(s.receivers.front());
                    s.receivers.pop_front();
                } else if (s.queue.size() < s.capacity) {
                    s.queue.push_back(std::move(value));
                    (void)promise.set_value();
                    return promise.handle();
                } else {
                    s.senders.push_back({std::move(value), promise});
                    return promise.handle();
                }
            }
            (void)receiver->set_value(std::optional<T>(std::move(value)));
            (void)promise.set_value();
            return promise.handle();
        }

        /// Non-waiting send. On `Full` or `Closed` the value is dropped; use `send` when it must not be lost.
        [[nodiscard]] std::expected<void, TrySendError> try_send(T value) const {
            std::optional<Promise<std::optional<T>>> receiver;
            {
                auto guard = state_->inner.lock();
                auto &s = *guard;
                if (s.closed) {
                    return std::unexpected(TrySendError::Closed);
                }
                if (!s.receivers.empty()) {
                    receiver = std::move(s.receivers.front());
                    s.receivers.pop_front();
                } else if (s.queue.size() < s.capacity) {
                    s.queue.push_back(std::move(value));
                    return {};
                } else {
                    return std::unexpected(TrySendError::Full);
                }
            }
            (void)receiver->set_value(std::optional<T>(std::move(value)));
            return {};
        }

        void close() const { state_->close(); }

      private:
        void retain() noexcept {
            if (state_) {
                state_->sender_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
        void release() noexcept {
            if (state_ && state_->sender_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                state_->close();
            }
        }

        std::shared_ptr<Detail::ChannelState<T>> state_;
    };

    template <typename T>
    struct ChannelPair {
        Sender<T> sender;
        Receiver<T> receiver;
    };

    /// Creates a bounded channel buffering up to `capacity` values (at least 1).
    template <typename T>
    [[nodiscard]] ChannelPair<T> channel(std::size_t capacity) {
        auto state = std::make_shared<Detail::ChannelState<T>>(capacity);
        return {Sender<T>(state), Receiver<T>(state)};
    }

} // namespace SFT::Async
