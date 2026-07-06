module;

#pragma region Imports
#include <algorithm>
#include <thread>
#include <vector>
#pragma endregion

export module Sturdy.Async:Chunk;

import Sturdy.Foundation;
import :Runtime;

using std::vector;

// Not exported from the top-level `Sturdy.Async` module (see Async.cppm) — this is the shared
// chunk-splitting logic behind both :Ranges and :ParIter's "split a random-access, sized range into
// one piece per task" behavior, not something a caller of the library needs directly.
export namespace SFT::Async::Detail {

    // A contiguous half-open index range `[begin, end)` — one task's share of a chunked range.
    struct ChunkBounds {
        usize begin;
        usize end;
    };

    // Splits `[0, size)` into up to `chunk_count` pieces, spreading the remainder across the first
    // pieces one element at a time (rather than dumping it all on the last one) so no single task
    // ends up with meaningfully more work than the rest.
    [[nodiscard]] inline vector<ChunkBounds> chunk_bounds(usize size, usize chunk_count) {
        vector<ChunkBounds> chunks;
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

    // How many pieces to split a range of `size` elements into: on a parallel runtime, one per
    // hardware thread (never more than there are elements); on a synchronous one (Web — see
    // :Runtime), always exactly one, since there's no second thread to gain from splitting.
    template <AsyncRuntime Rt>
    [[nodiscard]] inline usize chunk_count_for([[maybe_unused]] usize size) noexcept {
        if constexpr (Rt::is_parallel) {
            return std::min<usize>(size, std::max<usize>(1, std::thread::hardware_concurrency()));
        } else {
            return 1;
        }
    }

} // namespace SFT::Async::Detail
