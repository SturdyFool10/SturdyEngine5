module;

#pragma region Imports
#include <concepts>
#include <functional>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>
#pragma endregion

export module Sturdy.Async:ParIter;

import Sturdy.Foundation;
import :Runtime;
import :Task;
import :Chunk;
import :Ranges;

using std::convertible_to;
using std::invoke_result_t;
using std::vector;

export namespace SFT::Async {

    // Anything `par_iter()`/`par_iter_on<Rt>()` (below) can wrap: a `viewable_range` (see
    // `Foundation::Iterable`) that's also `random_access_range` and `sized_range` — what every
    // terminal operation on `ParIter` needs to split the range into equal contiguous chunks up front
    // (see `ParIter`'s own docs for why that rules out, say, a `filter_view`).
    template <class R>
    concept ParIterable = std::ranges::viewable_range<R> && std::ranges::random_access_range<R> && std::ranges::sized_range<R>;

    // A Rayon-flavored parallel iterator: "each task becomes a task for the runtime." `.map()`
    // builds the same kind of lazy `std::views::transform` chain `Foundation::Iter` does (see
    // Foundation/Iter.cppm) — no element is touched yet — but every terminal operation below
    // (`for_each()`, `reduce()`, `sum()`, `collect()`) splits the chain into one contiguous chunk per
    // hardware thread and submits each chunk as its own task to `Rt` (defaults to `DefaultRuntime`),
    // reusing the exact chunk-splitting `Ranges::for_each`/`Ranges::transform` (see :Ranges) already
    // use for their own chunking, and blocks until every chunk's task has finished.
    //
    // Deliberately narrower than `Foundation::Iter`: no `.filter()`/`.take_while()`/`.skip_while()`/
    // `.chain()` here, because those change how many elements come out (and how expensive it is to
    // reach a given one) in a way that isn't knowable ahead of time — `filter_view` is never
    // random-access, since skipping non-matching elements isn't O(1) — and knowing the size and being
    // able to jump straight to any element up front is exactly what lets this type split work into
    // equal contiguous chunks rather than needing a smarter recursive splitter (what a full
    // Rayon-style `IndexedParallelIterator`/`ParallelIterator` split would take to handle those cases
    // correctly). Filter with `Foundation::Iter` first, or filter inside a `.for_each()` callback,
    // then `par_iter()` what's left.
    template <AsyncRuntime Rt, class V>
        requires std::ranges::view<V> && ParIterable<V>
    class ParIter {
      public:
        explicit constexpr ParIter(V view) noexcept(std::is_nothrow_move_constructible_v<V>)
            : view_(std::move(view)) {
        }

        // Lazy — same `std::views::transform` machinery as `Foundation::Iter::map()`, and preserves
        // random access/size, so the result is still splittable by every operation below. `fn` is
        // `AsyncWork` (see :Runtime) because, unlike a `Ranges::for_each`/`Ranges::transform` callable
        // (each chunk's task gets its own copy), this `fn` is folded into the shared view chain itself
        // — every chunk's task reads through the one instance stored there, concurrently — so it must
        // tolerate that (no `mutable` lambdas / no unsynchronized mutable captures).
        template <class Fn>
            requires AsyncWork<Fn, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto map(Fn fn) {
            auto result = std::views::transform(std::move(view_), std::move(fn));
            return ParIter<Rt, decltype(result)>(std::move(result));
        }

        [[nodiscard]] usize count() const noexcept {
            return static_cast<usize>(std::ranges::size(view_));
        }

        // Runs `fn` over every element, split across `Rt` — literally `Ranges::for_each<Rt>` (see
        // :Ranges) with this iterator's chain as the range; each chunk's task gets its own copy of
        // `fn`.
        template <class Fn>
            requires AsyncWork<Fn, std::ranges::range_reference_t<V>>
        void for_each(Fn fn) {
            Ranges::for_each<Rt>(view_, std::move(fn));
        }

        // Fork-join reduction: each chunk folds its slice down to one `T` (starting from its own copy
        // of `identity`) on its own task, then the (few) partial results are combined, in order, on
        // the calling thread with the same `combine`. `combine` must be associative for a
        // deterministic result — the same requirement Rayon's `reduce()` places on its combining
        // function — since chunk order is preserved but each chunk's internal fold already happened
        // independently.
        template <class T, class Fn>
            requires AsyncWork<Fn, T, std::ranges::range_reference_t<V>> &&
                     convertible_to<invoke_result_t<const Fn &, T, std::ranges::range_reference_t<V>>, T>
        [[nodiscard]] T reduce(T identity, Fn combine) {
            const usize size = count();
            if (size == 0) {
                return identity;
            }

            const auto chunks = Detail::chunk_bounds(size, Detail::chunk_count_for<Rt>(size));
            auto first = std::ranges::begin(view_);
            using Diff = std::ranges::range_difference_t<V>;

            vector<TaskHandle<T>> handles;
            handles.reserve(chunks.size());
            for (const Detail::ChunkBounds &chunk : chunks) {
                auto chunk_begin = first + static_cast<Diff>(chunk.begin);
                auto chunk_end = first + static_cast<Diff>(chunk.end);
                handles.push_back(Rt::spawn([chunk_begin, chunk_end, identity, combine]() {
                    T acc = identity;
                    for (auto it = chunk_begin; it != chunk_end; ++it) {
                        acc = combine(std::move(acc), *it);
                    }
                    return acc;
                }));
            }

            T result = identity;
            for (TaskHandle<T> &handle : handles) {
                result = combine(std::move(result), handle.wait());
            }
            return result;
        }

        // `reduce(T{}, std::plus<>{})` for the range's own value type.
        [[nodiscard]] auto sum() {
            using T = std::ranges::range_value_t<V>;
            return reduce(T{}, std::plus<>{});
        }

        // Gathers every (already `map()`-ped) element into a `Container`, in order — each chunk
        // writes directly into its own slice of the up-front-sized result via `Ranges::transform<Rt>`
        // (see :Ranges), so this is that function with an identity `fn`.
        template <template <class...> class Container = vector>
        [[nodiscard]] auto collect() {
            using T = std::ranges::range_value_t<V>;
            Container<T> out(count());
            Ranges::transform<Rt>(view_, out, [](auto &&value) -> T { return std::forward<decltype(value)>(value); });
            return out;
        }

      private:
        V view_;
    };

    namespace Detail {

        template <AsyncRuntime Rt>
        struct ParIterFn : std::ranges::range_adaptor_closure<ParIterFn<Rt>> {
            template <ParIterable R>
            [[nodiscard]] constexpr auto operator()(R &&range) const {
                auto view = std::views::all(std::forward<R>(range));
                return ParIter<Rt, decltype(view)>(std::move(view));
            }
        };

    } // namespace Detail

    // Wraps a random-access, sized range for parallel consumption on `DefaultRuntime` — the "each
    // task becomes a task for the runtime" entry point. A `std::ranges::range_adaptor_closure`, so it
    // works both as an ordinary call, `par_iter(range)`, and piped, `range | par_iter` — the same two
    // forms `Foundation::iter` supports, and for the same reason (see its docs).
    inline constexpr Detail::ParIterFn<DefaultRuntime> par_iter{};

    // `par_iter`, but on a specific runtime rather than `DefaultRuntime` — e.g. `par_iter_on<
    // SynchronousRuntime>` to force serial, inline execution in a test. Also supports both forms:
    // `par_iter_on<Rt>(range)` and `range | par_iter_on<Rt>`.
    template <AsyncRuntime Rt = DefaultRuntime>
    inline constexpr Detail::ParIterFn<Rt> par_iter_on{};

} // namespace SFT::Async
