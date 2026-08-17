#pragma once

#include <algorithm>
#include <thread>
#include <vector>

#include <Async/src/Runtime.hpp>

namespace SFT::Async::Detail {

    struct ChunkBounds {
        usize begin;
        usize end;
    };

    /// Performs the chunk bounds operation using the supplied arguments.
    ///
    /// @param size Requested or available size for the operation.
    /// @param chunk_count Number of elements or operations to process.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] std::vector<ChunkBounds> chunk_bounds(usize size, usize chunk_count);

    /// Resolves the chunk count associated with the supplied key, handle, or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    template <AsyncRuntime Rt>
    [[nodiscard]] inline usize chunk_count_for([[maybe_unused]] usize size) noexcept {
        if constexpr (Rt::is_parallel) {
            return std::min<usize>(size, std::max<usize>(1, std::thread::hardware_concurrency()));
        } else {
            return 1;
        }
    }

} // namespace SFT::Async::Detail
