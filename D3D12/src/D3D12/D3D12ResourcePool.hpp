#pragma once

#pragma region Imports
#include <atomic>
#include <unordered_map>
#include <utility>
#pragma endregion

#include <Async/src/Mutex.hpp>
#include <Foundation/src/Foundation.hpp>

namespace SFT::D3D12 {

    /// Maps an opaque Sturdy.RHI handle (see Sturdy.RHI :Handles — `Handle<Tag>{u64 value}`) onto the
    /// move-only D3D12 record that backs it. One instance per resource kind in D3D12Device. Handles are
    /// minted from a monotonically increasing counter and never reused; there is no generation check
    /// because the RHI documents destroying a resource still referenced by in-flight work as caller
    /// error, not something the pool needs to catch.
    ///
    /// Mutex-guarded so multiple windows' render calls can create/look up/destroy resources
    /// concurrently — each call's critical section is a single map operation, never cross-pool, so one
    /// mutex per pool instance is sufficient. `Async::Mutex<T>` rather than a bare std::mutex + map so
    /// the map is simply unreachable without holding the lock. find()'s returned pointer is only valid
    /// while some lock on this pool is held (or under trusted single-threaded use) — the same contract
    /// a bare mutex + map would have given, enforced by construction instead of by convention.
    ///
    /// Deliberately a near-copy of Core::Vulkan::VulkanRhiResourcePool rather than a shared base: the
    /// two backends are independent implementations of the same contract, and a common pool type would
    /// have to live in the RHI, which must not grow backend-implementation machinery.
    template <typename HandleT, typename Stored>
    class D3D12ResourcePool {
      public:
        [[nodiscard]] HandleT insert(Stored &&object) {
            const u64 id = next_id_.fetch_add(1, std::memory_order_relaxed);
            auto storage = storage_.lock();
            storage->emplace(id, std::move(object));
            return HandleT{id};
        }

        [[nodiscard]] Stored *find(HandleT handle) noexcept {
            auto storage = storage_.lock();
            auto it = storage->find(handle.value);
            return it != storage->end() ? &it->second : nullptr;
        }

        [[nodiscard]] const Stored *find(HandleT handle) const noexcept {
            auto storage = storage_.lock();
            auto it = storage->find(handle.value);
            return it != storage->end() ? &it->second : nullptr;
        }

        /// Removes `handle`'s record and hands it back to the caller instead of destroying it in place.
        /// Needed wherever teardown has to run outside the pool's lock — releasing a descriptor range
        /// reaches back into the device's descriptor allocators, and doing that while still holding a
        /// resource pool's lock is the one lock-ordering hazard this design can produce.
        [[nodiscard]] std::optional<Stored> extract(HandleT handle) noexcept {
            auto storage = storage_.lock();
            auto it = storage->find(handle.value);
            if (it == storage->end()) {
                return std::nullopt;
            }
            std::optional<Stored> record(std::move(it->second));
            storage->erase(it);
            return record;
        }

        void erase(HandleT handle) noexcept {
            auto storage = storage_.lock();
            storage->erase(handle.value);
        }

        /// Applies `fn` to every live record. Used only by teardown paths that must release resources
        /// in a specific order before the D3D12 device itself goes away.
        template <typename Fn>
        void for_each(Fn &&fn) {
            auto storage = storage_.lock();
            for (auto &[id, record] : *storage) {
                fn(record);
            }
        }

      private:
        std::atomic<u64> next_id_ = 1;
        mutable Async::Mutex<std::unordered_map<u64, Stored>> storage_;
    };

} // namespace SFT::D3D12
