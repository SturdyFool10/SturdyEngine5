#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <atomic>
#include <unordered_map>
#include <utility>
#pragma endregion

#include <Async/src/Mutex.hpp>

namespace SFT::Core::Vulkan {


    template <typename HandleT, typename Stored>
    class VulkanRhiResourcePool {
      public:
        /// Inserts the supplied value or range at the requested position.
        ///
        /// @param object `object` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] HandleT insert(Stored &&object) {
            const u64 id = next_id_.fetch_add(1, std::memory_order_relaxed);
            auto storage = storage_.lock();
            storage->emplace(id, std::move(object));
            return HandleT{id};
        }

        /// Finds the requested entry in the available state.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Stored *find(HandleT handle) noexcept {
            auto storage = storage_.lock();
            auto it = storage->find(handle.value);
            return it != storage->end() ? &it->second : nullptr;
        }

        /// Finds the requested entry in the available state.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const Stored *find(HandleT handle) const noexcept {
            auto storage = storage_.lock();
            auto it = storage->find(handle.value);
            return it != storage->end() ? &it->second : nullptr;
        }

        /// Erases the selected element or range from the container.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void erase(HandleT handle) noexcept {
            auto storage = storage_.lock();
            storage->erase(handle.value);
        }


        /// Drains the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @note This function does not throw exceptions.
        template <typename Destroy>
        void drain(Destroy &&destroy) noexcept {
            auto storage = storage_.lock();
            for (auto &[id, object] : *storage) {
                (void)id;
                destroy(object);
            }
            storage->clear();
        }

      private:
        std::atomic<u64> next_id_ = 1;
        mutable Async::Mutex<std::unordered_map<u64, Stored>> storage_;
    };

} // namespace SFT::Core::Vulkan
