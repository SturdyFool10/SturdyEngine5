#pragma once

#include <iterator>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>
#include <optional>
#include <algorithm>
#include <functional>

#include <Async/Chunk.hpp>
#include <Async/Runtime.hpp>
#include <Async/Task.hpp>

namespace SFT::Async::Ranges {

    template <AsyncRuntime Rt = DefaultRuntime, std::ranges::random_access_range R, typename Fn>
        requires std::ranges::sized_range<R> && AsyncWork<Fn, std::ranges::range_reference_t<R>>
    void for_each(R &&range, Fn fn) {
        const usize size = static_cast<usize>(std::ranges::size(range));
        if (!size) return;
        const auto chunks = Detail::chunk_bounds(size, Detail::chunk_count_for<Rt>(size));
        auto first = std::ranges::begin(range);
        using Diff = std::ranges::range_difference_t<R>;
        std::vector<TaskHandle<void>> handles;
        handles.reserve(chunks.size());
        for (const auto &chunk : chunks) {
            auto b = first + static_cast<Diff>(chunk.begin);
            auto e = first + static_cast<Diff>(chunk.end);
            handles.push_back(Rt::spawn([b, e, fn] { for (auto it = b; it != e; ++it) fn(*it); }));
        }
        for (auto &h : handles) h.wait();
    }

    template <AsyncRuntime Rt = DefaultRuntime, std::ranges::random_access_range In, std::ranges::random_access_range Out, typename Fn>
        requires std::ranges::sized_range<In> && std::ranges::sized_range<Out> &&
                 AsyncWork<Fn, std::ranges::range_reference_t<In>> &&
                 std::indirectly_writable<std::ranges::iterator_t<Out>, std::invoke_result_t<Fn &, std::ranges::range_reference_t<In>>>
    void transform(In &&in, Out &&out, Fn fn) {
        const usize size = static_cast<usize>(std::ranges::size(in));
        if (!size) return;
        const auto chunks = Detail::chunk_bounds(size, Detail::chunk_count_for<Rt>(size));
        auto in_first = std::ranges::begin(in);
        auto out_first = std::ranges::begin(out);
        using InDiff = std::ranges::range_difference_t<In>;
        using OutDiff = std::ranges::range_difference_t<Out>;
        std::vector<TaskHandle<void>> handles;
        handles.reserve(chunks.size());
        for (const auto &chunk : chunks) {
            auto ib = in_first + static_cast<InDiff>(chunk.begin);
            auto ie = in_first + static_cast<InDiff>(chunk.end);
            auto ob = out_first + static_cast<OutDiff>(chunk.begin);
            handles.push_back(Rt::spawn([ib, ie, ob, fn] mutable { auto o = ob; for (auto i = ib; i != ie; ++i, ++o) *o = fn(*i); }));
        }
        for (auto &h : handles) h.wait();
    }

    template <AsyncRuntime Rt = DefaultRuntime, std::ranges::random_access_range R, typename Pred>
        requires std::ranges::sized_range<R> && std::predicate<Pred &, std::ranges::range_reference_t<R>>
    [[nodiscard]] auto filter(R &&range, Pred pred) {
        using T = std::ranges::range_value_t<R>;
        const usize size = static_cast<usize>(std::ranges::size(range));
        if (!size) return std::vector<T>{};
        const auto chunks = Detail::chunk_bounds(size, Detail::chunk_count_for<Rt>(size));
        auto first = std::ranges::begin(range);
        using Diff = std::ranges::range_difference_t<R>;
        std::vector<TaskHandle<std::vector<T>>> handles;
        handles.reserve(chunks.size());
        for (const auto &chunk : chunks) {
            auto b = first + static_cast<Diff>(chunk.begin);
            auto e = first + static_cast<Diff>(chunk.end);
            handles.push_back(Rt::spawn([b, e, pred] { std::vector<T> out; for (auto i = b; i != e; ++i) if (pred(*i)) out.emplace_back(*i); return out; }));
        }
        std::vector<T> out;
        for (auto &h : handles) { auto local = h.wait(); out.insert(out.end(), std::make_move_iterator(local.begin()), std::make_move_iterator(local.end())); }
        return out;
    }

    template <AsyncRuntime Rt = DefaultRuntime, std::ranges::random_access_range R, typename Fn>
        requires std::ranges::sized_range<R> && std::invocable<Fn &, std::ranges::range_reference_t<R>>
    [[nodiscard]] auto map(R &&range, Fn fn) {
        using T = std::remove_cvref_t<std::invoke_result_t<Fn &, std::ranges::range_reference_t<R>>>;
        std::vector<T> out(static_cast<usize>(std::ranges::size(range)));
        transform<Rt>(std::forward<R>(range), out, std::move(fn));
        return out;
    }

    template <AsyncRuntime Rt = DefaultRuntime, std::ranges::random_access_range R, typename Pred>
        requires std::ranges::sized_range<R> && std::predicate<Pred &, std::ranges::range_reference_t<R>>
    [[nodiscard]] bool any(R &&range, Pred pred) {
        auto values = filter<Rt>(std::forward<R>(range), std::move(pred));
        return !values.empty();
    }

    template <AsyncRuntime Rt = DefaultRuntime, std::ranges::random_access_range R, typename Pred>
        requires std::ranges::sized_range<R> && std::predicate<Pred &, std::ranges::range_reference_t<R>>
    [[nodiscard]] bool all(R &&range, Pred pred) {
        using T = std::ranges::range_value_t<R>;
        const usize size = static_cast<usize>(std::ranges::size(range));
        if (!size) return true;
        const auto chunks = Detail::chunk_bounds(size, Detail::chunk_count_for<Rt>(size));
        auto first = std::ranges::begin(range); using Diff = std::ranges::range_difference_t<R>;
        std::vector<TaskHandle<bool>> handles;
        for (const auto &c : chunks) { auto b=first+static_cast<Diff>(c.begin); auto e=first+static_cast<Diff>(c.end); handles.push_back(Rt::spawn([b,e,pred]{for(auto i=b;i!=e;++i)if(!pred(*i))return false;return true;})); }
        for(auto &h:handles) if(!h.wait()) return false; return true;
    }

    template <AsyncRuntime Rt = DefaultRuntime, std::ranges::random_access_range R, typename Pred>
        requires std::ranges::sized_range<R> && std::predicate<Pred &, std::ranges::range_reference_t<R>>
    [[nodiscard]] auto find(R &&range, Pred pred) -> std::optional<std::ranges::range_value_t<R>> {
        const usize size = static_cast<usize>(std::ranges::size(range)); if (!size) return std::nullopt;
        const auto chunks=Detail::chunk_bounds(size,Detail::chunk_count_for<Rt>(size)); auto first=std::ranges::begin(range); using Diff=std::ranges::range_difference_t<R>;
        std::vector<TaskHandle<std::optional<std::ranges::range_value_t<R>>>> handles;
        for(const auto &c:chunks){auto b=first+static_cast<Diff>(c.begin);auto e=first+static_cast<Diff>(c.end);handles.push_back(Rt::spawn([b,e,pred]{for(auto i=b;i!=e;++i)if(pred(*i))return std::optional<std::ranges::range_value_t<R>>{*i};return std::nullopt;}));}
        for(auto &h:handles){auto r=h.wait();if(r)return r;} return std::nullopt;
    }

} // namespace SFT::Async::Ranges
