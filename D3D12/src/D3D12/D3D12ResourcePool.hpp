#pragma once

#pragma region Imports
#include <atomic>
#include <unordered_map>
#include <utility>
#pragma endregion

#include <Async/src/Mutex.hpp>
#include <Foundation/src/Foundation.hpp>

namespace SFT::D3D12 {


    template <typename HandleT, typename Stored>
    class D3D12ResourcePool {
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


        /// Performs the extract operation for `D3D12ResourcePool` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
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

        /// Erases the selected element or range from the container.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void erase(HandleT handle) noexcept {
            auto storage = storage_.lock();
            storage->erase(handle.value);
        }


        /// Invokes the supplied function once for each element in the input range.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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
