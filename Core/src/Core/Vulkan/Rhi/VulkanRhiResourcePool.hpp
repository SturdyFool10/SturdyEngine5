#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <atomic>
#include <unordered_map>
#include <utility>
#pragma endregion

#include <Async/src/Mutex.hpp>

namespace SFT::Core::Vulkan {

    // Maps an opaque Sturdy.RHI handle (see Sturdy.RHI :Handles — `Handle<Tag>{u64 value}`) onto the
    // move-only Vulkan RAII object that backs it. One instance per resource kind in
    // VulkanRhiDeviceBridge. Handles are minted from a monotonically increasing counter and never
    // reused; there is no generation check because the RHI documents destroying a resource still
    // referenced by in-flight work as caller error, not something the pool needs to catch.
    //
    // Mutex-guarded so multiple windows' render calls can create/look up/destroy resources
    // concurrently (see plans/parallel-renderer-submission.md) — each call's critical section is a
    // single map operation, never cross-pool, so one mutex per pool instance is sufficient.
    // Async::Mutex<T> rather than a bare std::mutex + map so the map is simply unreachable without
    // holding the lock. find()'s returned pointer is only valid while some lock on this pool is held
    // (or trusted single-threaded use) — same contract a bare mutex + map would have given no
    // generation check, just enforced by construction here instead of by convention.
    template <typename HandleT, typename Stored>
    class VulkanRhiResourcePool {
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

        void erase(HandleT handle) noexcept {
            auto storage = storage_.lock();
            storage->erase(handle.value);
        }

      private:
        std::atomic<u64> next_id_ = 1;
        mutable Async::Mutex<std::unordered_map<u64, Stored>> storage_;
    };

} // namespace SFT::Core::Vulkan
