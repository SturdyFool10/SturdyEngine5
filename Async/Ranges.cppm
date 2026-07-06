module;

#pragma region Imports
#include <iterator>
#include <ranges>
#include <type_traits>
#include <vector>
#pragma endregion

export module Sturdy.Async:Ranges;

import Sturdy.Foundation;
import :Runtime;
import :Task;
import :Chunk;

using std::invoke_result_t;
using std::vector;

export namespace SFT::Async::Ranges {

    // `std::ranges::for_each`, but each chunk of `range` runs as its own task on `Rt` (defaults to
    // `DefaultRuntime` — a `Scheduler` worker natively, inline on Web) instead of the whole range
    // running on the calling thread. Blocks the caller until every chunk has finished.
    //
    // `range` must be a `random_access_range` (so it can be split into contiguous chunks in O(1))
    // and a `sized_range` (so the chunk count/bounds can be computed up front). `fn` is `AsyncWork`
    // (see :Runtime): each chunk's task gets its own copy of it, so `fn` itself never needs to
    // tolerate concurrent calls — only whatever it's copying (captures with reference semantics,
    // e.g. a raw pointer to shared state) does. Capture a `Mutex<T>` (see :Mutex) or an `Arc<Mutex<T>>`
    // (see :Arc) rather than a bare shared variable if `fn` needs to accumulate state across chunks.
    template <AsyncRuntime Rt = DefaultRuntime, std::ranges::random_access_range R, typename Fn>
        requires std::ranges::sized_range<R> && AsyncWork<Fn, std::ranges::range_reference_t<R>>
    void for_each(R &&range, Fn fn) {
        const usize size = static_cast<usize>(std::ranges::size(range));
        if (size == 0) {
            return;
        }

        const auto chunks = Detail::chunk_bounds(size, Detail::chunk_count_for<Rt>(size));
        auto first = std::ranges::begin(range);

        vector<TaskHandle<void>> handles;
        handles.reserve(chunks.size());
        for (const Detail::ChunkBounds &chunk : chunks) {
            using Diff = std::ranges::range_difference_t<R>;
            auto chunk_begin = first + static_cast<Diff>(chunk.begin);
            auto chunk_end = first + static_cast<Diff>(chunk.end);
            handles.push_back(Rt::spawn([chunk_begin, chunk_end, fn]() {
                for (auto it = chunk_begin; it != chunk_end; ++it) {
                    fn(*it);
                }
            }));
        }

        for (TaskHandle<void> &handle : handles) {
            handle.wait();
        }
    }

    // `std::ranges::transform`, split across `Rt` the same way `for_each` above is: `fn(in[i])` is
    // written to `out[i]` for every `i`, with each chunk running as its own task (its own copy of
    // `fn` — see `for_each`'s docs on `AsyncWork`). `out` must be at least as large as `in` — the
    // caller sizes it up front (e.g. `std::vector<R>(in.size())`), since splitting the write side the
    // same way as the read side needs `out` to already have that many slots to index into, not merely
    // a back-inserter to append to.
    template <AsyncRuntime Rt = DefaultRuntime, std::ranges::random_access_range In, std::ranges::random_access_range Out, typename Fn>
        requires std::ranges::sized_range<In> && AsyncWork<Fn, std::ranges::range_reference_t<In>> &&
                 std::indirectly_writable<std::ranges::iterator_t<Out>, invoke_result_t<Fn &, std::ranges::range_reference_t<In>>>
    void transform(In &&in, Out &&out, Fn fn) {
        const usize size = static_cast<usize>(std::ranges::size(in));
        if (size == 0) {
            return;
        }

        const auto chunks = Detail::chunk_bounds(size, Detail::chunk_count_for<Rt>(size));
        auto in_first = std::ranges::begin(in);
        auto out_first = std::ranges::begin(out);

        vector<TaskHandle<void>> handles;
        handles.reserve(chunks.size());
        for (const Detail::ChunkBounds &chunk : chunks) {
            using InDiff = std::ranges::range_difference_t<In>;
            using OutDiff = std::ranges::range_difference_t<Out>;
            auto chunk_in_begin = in_first + static_cast<InDiff>(chunk.begin);
            auto chunk_in_end = in_first + static_cast<InDiff>(chunk.end);
            auto chunk_out_begin = out_first + static_cast<OutDiff>(chunk.begin);
            handles.push_back(Rt::spawn([chunk_in_begin, chunk_in_end, chunk_out_begin, fn]() {
                auto out_it = chunk_out_begin;
                for (auto it = chunk_in_begin; it != chunk_in_end; ++it, ++out_it) {
                    *out_it = fn(*it);
                }
            }));
        }

        for (TaskHandle<void> &handle : handles) {
            handle.wait();
        }
    }

} // namespace SFT::Async::Ranges
