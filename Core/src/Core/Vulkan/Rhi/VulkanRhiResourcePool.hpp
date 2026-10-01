#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <atomic>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#pragma endregion

namespace SFT::Core::Vulkan {

    /// Handle table behind every Vulkan RHI resource type (buffers, textures, pipelines, bind groups, ...).
    ///
    /// `find()` runs at least once per bound resource per draw call, so it is the single hottest call in this
    /// backend's command-recording path. Command recording on any one encoder is single-threaded, but several
    /// encoders (one per window's render thread, plus async compute) can record concurrently, all calling
    /// `find()` on the *same* pools (materials/meshes/textures are shared across windows) while creation and
    /// destruction happen elsewhere. A plain mutex serializes every one of those concurrent, read-only lookups
    /// against each other for no reason; a `std::shared_mutex` lets them all proceed together and only blocks
    /// a lookup against a genuine `insert`/`erase`/`drain` (which are comparatively rare: once per resource
    /// lifetime, not once per draw).
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
            std::unique_lock lock{mutex_};
            storage_.emplace(id, std::move(object));
            return HandleT{id};
        }

        /// Finds the requested entry in the available state.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions. Takes only a shared (read) lock: concurrent lookups
        /// from different recording threads do not block each other.
        [[nodiscard]] Stored *find(HandleT handle) noexcept {
            std::shared_lock lock{mutex_};
            auto it = storage_.find(handle.value);
            return it != storage_.end() ? &it->second : nullptr;
        }

        /// Finds the requested entry in the available state.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions. Takes only a shared (read) lock: concurrent lookups
        /// from different recording threads do not block each other.
        [[nodiscard]] const Stored *find(HandleT handle) const noexcept {
            std::shared_lock lock{mutex_};
            auto it = storage_.find(handle.value);
            return it != storage_.end() ? &it->second : nullptr;
        }

        /// Erases the selected element or range from the container.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void erase(HandleT handle) noexcept {
            std::unique_lock lock{mutex_};
            storage_.erase(handle.value);
        }


        /// Drains the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @note This function does not throw exceptions.
        template <typename Destroy>
        void drain(Destroy &&destroy) noexcept {
            std::unique_lock lock{mutex_};
            for (auto &[id, object] : storage_) {
                (void)id;
                destroy(object);
            }
            storage_.clear();
        }

      private:
        std::atomic<u64> next_id_ = 1;
        mutable std::shared_mutex mutex_;
        std::unordered_map<u64, Stored> storage_;
    };

} // namespace SFT::Core::Vulkan
