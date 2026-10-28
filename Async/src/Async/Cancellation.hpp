#pragma once

#include <Async/Mutex.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace SFT::Async {

    class IoOperationCancelled final : public std::runtime_error {
      public:
        IoOperationCancelled() : std::runtime_error("Async IO operation was cancelled") {}
    };

    namespace Detail {
        struct CancellationState {
            std::atomic<bool> cancelled{false};
            struct Callback {
                std::uint64_t id;
                std::function<void()> fn;
            };
            Mutex<std::vector<Callback>> callbacks;
            std::atomic<std::uint64_t> next_id{1};
        };
    } // namespace Detail

    /// Keeps a cancellation callback registered; destroying it unregisters (and, if the callback is
    /// running on another thread right now, does not wait for it).
    class CancellationRegistration {
      public:
        CancellationRegistration() noexcept = default;
        ~CancellationRegistration() { reset(); }
        CancellationRegistration(const CancellationRegistration &) = delete;
        CancellationRegistration &operator=(const CancellationRegistration &) = delete;
        CancellationRegistration(CancellationRegistration &&other) noexcept
            : state_(std::move(other.state_)), id_(std::exchange(other.id_, 0)) {}
        CancellationRegistration &operator=(CancellationRegistration &&other) noexcept {
            if (this != &other) {
                reset();
                state_ = std::move(other.state_);
                id_ = std::exchange(other.id_, 0);
            }
            return *this;
        }

        void reset() noexcept;

      private:
        friend class CancellationToken;
        CancellationRegistration(std::shared_ptr<Detail::CancellationState> state, std::uint64_t id) noexcept
            : state_(std::move(state)), id_(id) {}
        std::shared_ptr<Detail::CancellationState> state_;
        std::uint64_t id_ = 0;
    };

    class CancellationToken {
      public:
        CancellationToken() noexcept = default;

        [[nodiscard]] bool is_cancelled() const noexcept;
        /// False for default-constructed tokens, which can never become cancelled.
        [[nodiscard]] bool can_be_cancelled() const noexcept { return state_ != nullptr; }
        void throw_if_cancelled() const;

        /// Runs `fn` when the token is cancelled -- immediately, on this thread, if it already is --
        /// otherwise on the thread that calls `CancellationSource::cancel`. `fn` must be short and must
        /// not throw. Keep the returned registration alive for as long as the callback should stay armed.
        [[nodiscard]] CancellationRegistration on_cancel(std::function<void()> fn) const;

      private:
        explicit CancellationToken(std::shared_ptr<Detail::CancellationState> state) noexcept : state_(std::move(state)) {}
        std::shared_ptr<Detail::CancellationState> state_;
        friend class CancellationSource;
    };

    class CancellationSource {
      public:
        CancellationSource();

        [[nodiscard]] CancellationToken token() const noexcept;
        /// Sets the cancellation state once and runs registered callbacks. Returns true only for the first call.
        [[nodiscard]] bool cancel() noexcept;
        [[nodiscard]] bool is_cancelled() const noexcept;

      private:
        std::shared_ptr<Detail::CancellationState> state_;
    };

} // namespace SFT::Async
