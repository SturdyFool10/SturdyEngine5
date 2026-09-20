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
    /// The read path (`fire`/`size`) is one relaxed atomic pointer load and a predicted-likely
    /// branch when nobody has ever subscribed — the common case for every reflected type a mod
    /// never touches, and (unlike the eager `Multicast()`-constructs-an-`Impl` design this
    /// replaced) the *only* cost paid in that case: no heap allocation happens merely because a
    /// method/event got reflected, only because something actually subscribed to it. Once a
    /// subscriber exists, `fire` pays one additional relaxed load of the subscriber count, then
    /// (only past that) the acquire-load of an immutable snapshot (a copy-on-write
    /// `std::vector`, published via `std::atomic<std::shared_ptr<const std::vector<Entry>>>`,
    /// C++20's atomic shared_ptr specialization) — an RCU-style read path that never blocks a
    /// concurrent subscribe/fire. Writers (`subscribe`/`unsubscribe`) are serialized by an
    /// internal mutex and are expected to be rare (mod install/uninstall), never called from the
    /// hot path.
    ///
    /// Copying a `Multicast` copies a handle to shared state (like `std::shared_ptr`), which is
    /// what lets it live as a plain member of a move-only-by-necessity struct like `MethodInfo`
    /// without needing custom move machinery of its own beyond the explicit special members
    /// below (needed only because `std::atomic<Impl *>` itself has none).
    class Multicast {
      public:
        using ListenerFn = void (*)(void *object, const void *const *args, void *user_data) noexcept;

        struct Subscription {
            u64 id = 0;

            [[nodiscard]] constexpr explicit operator bool() const noexcept {
                return id != 0;
            }
        };

        Multicast() noexcept = default;

        /// Copies the handle to `other`'s backing state (if any has been created yet) — like
        /// copying a `shared_ptr`, this is a plain atomic pointer load/store, not a deep copy of
        /// subscriber lists.
        /// @note This function does not throw exceptions.
        Multicast(const Multicast &other) noexcept : impl_(other.impl_.load(std::memory_order_acquire)) {}
        /// Assigns a new value to this `Multicast` by copying the handle to `other`'s backing state.
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        Multicast &operator=(const Multicast &other) noexcept {
            impl_.store(other.impl_.load(std::memory_order_acquire), std::memory_order_release);
            return *this;
        }
        /// Constructs a `Multicast` by moving from `other`. Identical to the copy constructor —
        /// `Multicast` is a handle to (at most) one process-lifetime `Impl`, so "moving" it and
        /// "copying" it leave both handles pointing at the same backing state either way.
        /// @note This function does not throw exceptions.
        Multicast(Multicast &&other) noexcept : impl_(other.impl_.load(std::memory_order_acquire)) {}
        /// Assigns a new value to this `Multicast` by moving from `other`. See the move constructor.
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        Multicast &operator=(Multicast &&other) noexcept {
            impl_.store(other.impl_.load(std::memory_order_acquire), std::memory_order_release);
            return *this;
        }

        /// Adds a listener, called (in registration order) every time `fire` runs. Lazily
        /// allocates this `Multicast`'s backing state on the first call (see `ensure_impl`).
        ///
        /// @return Returns a `Subscription` usable with `unsubscribe`.
        [[nodiscard]] Subscription subscribe(ListenerFn fn, void *user_data) const {
            Impl &impl = ensure_impl();
            std::lock_guard lock(impl.mutation_mutex);
            const auto current = impl.subscribers.load(std::memory_order_acquire);
            auto next = std::make_shared<std::vector<Entry>>(current != nullptr ? *current : std::vector<Entry>{});
            const u64 id = impl.next_id.fetch_add(1, std::memory_order_relaxed);
            next->push_back(Entry{.id = id, .fn = fn, .user_data = user_data});
            const usize new_size = next->size();
            impl.subscribers.store(std::move(next), std::memory_order_release);
            impl.count.store(new_size, std::memory_order_relaxed);
            return Subscription{.id = id};
        }

        /// Removes a previously installed listener. A no-op returning `false` when nobody has
        /// ever subscribed (backing state was never created) — never allocates.
        ///
        /// @return Returns `true` when `subscription` was found and removed; `false` otherwise.
        [[nodiscard]] bool unsubscribe(Subscription subscription) const noexcept {
            Impl *impl = impl_.load(std::memory_order_acquire);
            if (impl == nullptr) {
                return false;
            }
            std::lock_guard lock(impl->mutation_mutex);
            const auto current = impl->subscribers.load(std::memory_order_acquire);
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
            impl->count.store(next->size(), std::memory_order_relaxed);
            impl->subscribers.store(std::move(next), std::memory_order_release);
            return true;
        }

        /// Calls every subscribed listener with `(object, args, listener's user_data)`, in
        /// registration order. A no-op — one relaxed atomic pointer load (plus, once backing
        /// state exists, one relaxed atomic integer load) behind a `[[likely]]` branch — when
        /// nobody is subscribed.
        void fire(void *object, const void *const *args) const noexcept {
            Impl *impl = impl_.load(std::memory_order_relaxed);
            if (impl == nullptr) [[likely]] {
                return;
            }
            if (impl->count.load(std::memory_order_relaxed) == 0) [[likely]] {
                return;
            }
            const auto snapshot = impl->subscribers.load(std::memory_order_acquire);
            if (snapshot == nullptr) {
                return;
            }
            for (const Entry &entry : *snapshot) {
                entry.fn(object, args, entry.user_data);
            }
        }

        /// Returns the current subscriber count. `0` when nobody has ever subscribed, with no
        /// allocation performed to answer that.
        ///
        /// @return Returns the current size value.
        [[nodiscard]] usize size() const noexcept {
            Impl *impl = impl_.load(std::memory_order_acquire);
            return impl != nullptr ? impl->count.load(std::memory_order_relaxed) : 0;
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

        /// Lazily creates this `Multicast`'s backing `Impl` on first `subscribe()`, via a
        /// compare-exchange race rather than a mutex — so the (overwhelmingly common)
        /// zero-subscriber `fire()`/`size()` fast path never has to synchronize against a
        /// creation-in-progress writer, only ever perform a single relaxed load. A losing
        /// racer's freshly allocated `Impl` is simply discarded (`subscribe`/hook installation is
        /// a rare, install-time-only operation — install-order jitter between two racing mods is
        /// already unspecified, so this adds no new hazard). Once created, `impl_` is never
        /// reassigned or freed: it lives for the remainder of the process, the same contract
        /// `TypeRegistry::unregister_type`'s doc comment already establishes for `TypeInfo` and
        /// everything it owns.
        [[nodiscard]] Impl &ensure_impl() const {
            Impl *current = impl_.load(std::memory_order_acquire);
            if (current != nullptr) {
                return *current;
            }
            auto *fresh = new Impl();
            Impl *expected = nullptr;
            if (impl_.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return *fresh;
            }
            delete fresh;
            return *expected;
        }

        mutable std::atomic<Impl *> impl_{nullptr};
    };


} // namespace SFT::Reflection
