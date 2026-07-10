module;

#pragma region Imports
#include <unordered_map>
#include <utility>
#pragma endregion

export module Sturdy.Core:VulkanRhiResourcePool;

import Sturdy.Foundation;

export namespace SFT::Core::Vulkan {

    // Maps an opaque Sturdy.RHI handle (see Sturdy.RHI :Handles — `Handle<Tag>{u64 value}`) onto the
    // move-only Vulkan RAII object that backs it. One instance per resource kind in
    // VulkanRhiDeviceBridge. Handles are minted from a monotonically increasing counter and never
    // reused; there is no generation check because the RHI documents destroying a resource still
    // referenced by in-flight work as caller error, not something the pool needs to catch.
    template <typename HandleT, typename Stored>
    class VulkanRhiResourcePool {
      public:
        [[nodiscard]] HandleT insert(Stored &&object) {
            const u64 id = next_id_++;
            storage_.emplace(id, std::move(object));
            return HandleT{id};
        }

        [[nodiscard]] Stored *find(HandleT handle) noexcept {
            auto it = storage_.find(handle.value);
            return it != storage_.end() ? &it->second : nullptr;
        }

        [[nodiscard]] const Stored *find(HandleT handle) const noexcept {
            auto it = storage_.find(handle.value);
            return it != storage_.end() ? &it->second : nullptr;
        }

        void erase(HandleT handle) noexcept {
            storage_.erase(handle.value);
        }

      private:
        u64 next_id_ = 1;
        std::unordered_map<u64, Stored> storage_;
    };

} // namespace SFT::Core::Vulkan
