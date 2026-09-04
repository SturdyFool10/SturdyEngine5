#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <atomic>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#pragma endregion

#include <Async/Mutex.hpp>
#include <RHI/Error.hpp>

#include <webgpu/webgpu.h>

namespace SFT::Core::WebGpu {

    /// Wraps a `std::string_view` as the `WGPUStringView` the current webgpu.h expects everywhere a
    /// string is passed.
    ///
    /// WebGPU moved from null-terminated `const char *` to an explicit pointer+length pair, and an
    /// empty view must be spelled as `{nullptr, WGPU_STRLEN}` rather than `{"", 0}` for the
    /// "absent" cases (labels, entry points) — Dawn distinguishes "no string" from "empty string".
    ///
    /// @param text `text` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline WGPUStringView wgpu_string(std::string_view text) noexcept {
        if (text.empty()) {
            return WGPUStringView{nullptr, WGPU_STRLEN};
        }
        return WGPUStringView{text.data(), text.size()};
    }

    /// Wraps a possibly-null C string as a `WGPUStringView`.
    ///
    /// @param text `text` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline WGPUStringView wgpu_string(const char *text) noexcept {
        return text != nullptr ? wgpu_string(std::string_view{text}) : WGPUStringView{nullptr, WGPU_STRLEN};
    }

    /// Copies a `WGPUStringView` out into an owned string. Dawn returns views into storage it owns
    /// and may reuse, so anything kept past the call that produced it has to be copied.
    ///
    /// @param view `view` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] inline std::string to_string(WGPUStringView view) {
        if (view.data == nullptr || view.length == WGPU_STRLEN) {
            return view.data != nullptr ? std::string{view.data} : std::string{};
        }
        return std::string{view.data, view.length};
    }

    /// Builds the RHI error returned when a caller asks this backend for something WebGPU itself
    /// does not have.
    ///
    /// Kept as one function so every such refusal reads the same way and names the feature rather
    /// than failing generically — a caller that lands on one of these needs to know it is a
    /// property of WebGPU, not a gap in this implementation that a later patch might fill.
    ///
    /// @param what The capability the caller asked for.
    ///
    /// @return Returns the error alternative describing why the operation failed.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] RHI::RhiError unsupported_by_webgpu(std::string_view what);

    /// Builds the RHI error for a WebGPU call that failed at runtime.
    ///
    /// @param what The operation that failed.
    /// @param detail Backend-supplied detail, if any.
    ///
    /// @return Returns the error alternative describing why the operation failed.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] RHI::RhiError webgpu_error(std::string_view what, std::string_view detail = {});

    /// Maps RHI handles to the WebGPU objects behind them.
    ///
    /// The same shape as the Vulkan and D3D12 backends' pools: RHI handles are opaque integers, so
    /// each backend keeps its own table. Guarded by an `Async::Mutex` because the renderer records
    /// command buffers from several threads and every one of them resolves handles.
    template <typename HandleT, typename Stored>
    class WebGpuResourcePool {
      public:
        /// Inserts `object` and returns the handle that now names it.
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

        /// Finds the entry `handle` names.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Stored *find(HandleT handle) noexcept {
            auto storage = storage_.lock();
            auto it = storage->find(handle.value);
            return it != storage->end() ? &it->second : nullptr;
        }

        /// Finds the entry `handle` names.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const Stored *find(HandleT handle) const noexcept {
            auto storage = storage_.lock();
            auto it = storage->find(handle.value);
            return it != storage->end() ? &it->second : nullptr;
        }

        /// Removes the entry `handle` names, running `destroy` on it first.
        ///
        /// @param handle Handle identifying the target object or resource.
        /// @param destroy Callable invoked with the stored object before it is erased.
        ///
        /// @note This function does not throw exceptions.
        template <typename Destroy>
        void erase(HandleT handle, Destroy &&destroy) noexcept {
            auto storage = storage_.lock();
            auto it = storage->find(handle.value);
            if (it == storage->end()) {
                return;
            }
            destroy(it->second);
            storage->erase(it);
        }

        /// Runs `destroy` on every entry and empties the pool.
        ///
        /// @param destroy Callable invoked with each stored object.
        ///
        /// @note This function does not throw exceptions.
        template <typename Destroy>
        void drain(Destroy &&destroy) noexcept {
            auto storage = storage_.lock();
            for (auto &entry : *storage) {
                destroy(entry.second);
            }
            storage->clear();
        }

      private:
        // Starts at 1 so a default-constructed (zero) handle is never a live entry.
        std::atomic<u64> next_id_{1};
        mutable Async::Mutex<std::unordered_map<u64, Stored>> storage_;
    };

} // namespace SFT::Core::WebGpu
