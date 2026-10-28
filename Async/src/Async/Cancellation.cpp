#include <Async/Cancellation.hpp>

#include <algorithm>
#include <utility>

namespace SFT::Async {

    void CancellationRegistration::reset() noexcept {
        if (state_ && id_ != 0) {
            auto guard = state_->callbacks.lock();
            std::erase_if(*guard, [id = id_](const Detail::CancellationState::Callback &cb) { return cb.id == id; });
        }
        state_.reset();
        id_ = 0;
    }

    bool CancellationToken::is_cancelled() const noexcept {
        return state_ && state_->cancelled.load(std::memory_order_acquire);
    }

    void CancellationToken::throw_if_cancelled() const {
        if (is_cancelled()) {
            throw IoOperationCancelled{};
        }
    }

    CancellationRegistration CancellationToken::on_cancel(std::function<void()> fn) const {
        if (!state_) {
            return {};
        }
        std::uint64_t id = 0;
        {
            auto guard = state_->callbacks.lock();
            // Checked under the lock: cancel() sets the flag first and then drains under the same lock,
            // so either we see the flag here or cancel() sees our callback.
            if (!state_->cancelled.load(std::memory_order_acquire)) {
                id = state_->next_id.fetch_add(1, std::memory_order_relaxed);
                guard->push_back({id, std::move(fn)});
                return CancellationRegistration(state_, id);
            }
        }
        fn();
        return {};
    }

    CancellationSource::CancellationSource() : state_(std::make_shared<Detail::CancellationState>()) {}

    CancellationToken CancellationSource::token() const noexcept {
        return CancellationToken{state_};
    }

    bool CancellationSource::cancel() noexcept {
        bool expected = false;
        if (!state_->cancelled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return false;
        }
        std::vector<Detail::CancellationState::Callback> pending;
        {
            auto guard = state_->callbacks.lock();
            pending.swap(*guard);
        }
        for (auto &callback : pending) {
            callback.fn();
        }
        return true;
    }

    bool CancellationSource::is_cancelled() const noexcept {
        return state_->cancelled.load(std::memory_order_acquire);
    }

} // namespace SFT::Async
