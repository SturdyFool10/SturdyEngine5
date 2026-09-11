#pragma once

#include <Foundation/Foundation.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace SFT::Reflection {


    /// A lock-light multi-subscriber listener list, shared by `EventInfo` (mod-added event
    /// listeners) and `MethodInfo` (mod-added before/after hooks around a call).
    ///
    /// The read path (`fire`/`size`) is one relaxed atomic integer load and a predicted-likely
    /// branch when nobody is subscribed — the common case for every reflected type a mod never
    /// touches. Only once a subscriber exists does `fire` pay for the acquire-load of an
    /// immutable snapshot (a copy-on-write `std::vector`, published via
    /// `std::atomic<std::shared_ptr<const std::vector<Entry>>>`, C++20's atomic shared_ptr
    /// specialization) — an RCU-style read path that never blocks a concurrent subscribe/fire.
    /// Writers (`subscribe`/`unsubscribe`) are serialized by an internal mutex and are expected
    /// to be rare (mod install/uninstall), never called from the hot path.
    ///
    /// Copying a `Multicast` copies a handle to shared state (like `std::shared_ptr`), which is
    /// what lets it live as a plain member of a move-only-by-necessity struct like `MethodInfo`
    /// without needing custom move machinery of its own.
    class Multicast {
      public:
        using ListenerFn = void (*)(void *object, const void *const *args, void *user_data) noexcept;

        struct Subscription {
            u64 id = 0;

            [[nodiscard]] constexpr explicit operator bool() const noexcept {
                return id != 0;
            }
        };

        Multicast() : impl_(std::make_shared<Impl>()) {}

        /// Adds a listener, called (in registration order) every time `fire` runs.
        ///
        /// @return Returns a `Subscription` usable with `unsubscribe`.
        [[nodiscard]] Subscription subscribe(ListenerFn fn, void *user_data) const {
            std::lock_guard lock(impl_->mutation_mutex);
            const auto current = impl_->subscribers.load(std::memory_order_acquire);
            auto next = std::make_shared<std::vector<Entry>>(current != nullptr ? *current : std::vector<Entry>{});
            const u64 id = impl_->next_id.fetch_add(1, std::memory_order_relaxed);
            next->push_back(Entry{.id = id, .fn = fn, .user_data = user_data});
            const usize new_size = next->size();
            impl_->subscribers.store(std::move(next), std::memory_order_release);
            impl_->count.store(new_size, std::memory_order_relaxed);
            return Subscription{.id = id};
        }

        /// Removes a previously installed listener.
        ///
        /// @return Returns `true` when `subscription` was found and removed; `false` otherwise.
        [[nodiscard]] bool unsubscribe(Subscription subscription) const noexcept {
            std::lock_guard lock(impl_->mutation_mutex);
            const auto current = impl_->subscribers.load(std::memory_order_acquire);
            if (current == nullptr) {
                return false;
            }
            auto next = std::make_shared<std::vector<Entry>>();
            next->reserve(current->size());
            bool removed = false;
            for (const Entry &entry : *current) {
                if (entry.id == subscription.id) {
                    removed = true;
                    continue;
                }
                next->push_back(entry);
            }
            if (!removed) {
                return false;
            }
            impl_->count.store(next->size(), std::memory_order_relaxed);
            impl_->subscribers.store(std::move(next), std::memory_order_release);
            return true;
        }

        /// Calls every subscribed listener with `(object, args, listener's user_data)`, in
        /// registration order. A no-op — a single relaxed atomic load plus a `[[likely]]`
        /// branch — when nobody is subscribed.
        void fire(void *object, const void *const *args) const noexcept {
            if (impl_->count.load(std::memory_order_relaxed) == 0) [[likely]] {
                return;
            }
            const auto snapshot = impl_->subscribers.load(std::memory_order_acquire);
            if (snapshot == nullptr) {
                return;
            }
            for (const Entry &entry : *snapshot) {
                entry.fn(object, args, entry.user_data);
            }
        }

        /// Returns the current subscriber count.
        ///
        /// @return Returns the current size value.
        [[nodiscard]] usize size() const noexcept {
            return impl_->count.load(std::memory_order_relaxed);
        }

      private:
        struct Entry {
            u64 id = 0;
            ListenerFn fn = nullptr;
            void *user_data = nullptr;
        };

        struct Impl {
            std::mutex mutation_mutex;
            std::atomic<std::shared_ptr<const std::vector<Entry>>> subscribers{};
            std::atomic<usize> count{0};
            std::atomic<u64> next_id{1};
        };

        std::shared_ptr<Impl> impl_;
    };


} // namespace SFT::Reflection
