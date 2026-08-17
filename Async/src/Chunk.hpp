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

    [[nodiscard]] std::vector<ChunkBounds> chunk_bounds(usize size, usize chunk_count);

    template <AsyncRuntime Rt>
    [[nodiscard]] inline usize chunk_count_for([[maybe_unused]] usize size) noexcept {
        if constexpr (Rt::is_parallel) {
            return std::min<usize>(size, std::max<usize>(1, std::thread::hardware_concurrency()));
        } else {
            return 1;
        }
    }

} // namespace SFT::Async::Detail
