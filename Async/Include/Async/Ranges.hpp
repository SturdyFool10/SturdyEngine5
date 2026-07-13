#pragma once

#include <iterator>
#include <ranges>
#include <type_traits>
#include <vector>

#include <Async/Chunk.hpp>
#include <Async/Runtime.hpp>
#include <Async/Task.hpp>

namespace SFT::Async::Ranges {

    template <AsyncRuntime Rt = DefaultRuntime, std::ranges::random_access_range R, typename Fn>
        requires std::ranges::sized_range<R> && AsyncWork<Fn, std::ranges::range_reference_t<R>>
    void for_each(R &&range, Fn fn) {
        const usize size = static_cast<usize>(std::ranges::size(range));
        if (size == 0) {
            return;
        }

        const auto chunks = Detail::chunk_bounds(size, Detail::chunk_count_for<Rt>(size));
        auto first = std::ranges::begin(range);

        std::vector<TaskHandle<void>> handles;
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

    template <AsyncRuntime Rt = DefaultRuntime, std::ranges::random_access_range In, std::ranges::random_access_range Out, typename Fn>
        requires std::ranges::sized_range<In> && AsyncWork<Fn, std::ranges::range_reference_t<In>> &&
                 std::indirectly_writable<std::ranges::iterator_t<Out>, std::invoke_result_t<Fn &, std::ranges::range_reference_t<In>>>
    void transform(In &&in, Out &&out, Fn fn) {
        const usize size = static_cast<usize>(std::ranges::size(in));
        if (size == 0) {
            return;
        }

        const auto chunks = Detail::chunk_bounds(size, Detail::chunk_count_for<Rt>(size));
        auto in_first = std::ranges::begin(in);
        auto out_first = std::ranges::begin(out);

        std::vector<TaskHandle<void>> handles;
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
