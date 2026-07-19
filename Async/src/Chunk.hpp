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

    [[nodiscard]] inline std::vector<ChunkBounds> chunk_bounds(usize size, usize chunk_count) {
        std::vector<ChunkBounds> chunks;
        if (size == 0) {
            return chunks;
        }

        chunk_count = std::max<usize>(1, std::min(chunk_count, size));
        const usize base = size / chunk_count;
        const usize remainder = size % chunk_count;

        chunks.reserve(chunk_count);
        usize offset = 0;
        for (usize i = 0; i < chunk_count; ++i) {
            const usize count = base + (i < remainder ? 1 : 0);
            chunks.push_back(ChunkBounds{offset, offset + count});
            offset += count;
        }
        return chunks;
    }

    template <AsyncRuntime Rt>
    [[nodiscard]] inline usize chunk_count_for([[maybe_unused]] usize size) noexcept {
        if constexpr (Rt::is_parallel) {
            return std::min<usize>(size, std::max<usize>(1, std::thread::hardware_concurrency()));
        } else {
            return 1;
        }
    }

} // namespace SFT::Async::Detail
