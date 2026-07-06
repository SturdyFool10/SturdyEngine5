module;

#pragma region Imports
#include <algorithm>
#include <concepts>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <type_traits>
#include <vector>
#pragma endregion

export module Sturdy.Foundation:Iter;

import :Concepts;
import :Types;

using std::optional;
using std::predicate;

export namespace SFT::Foundation {

    // Anything `iter()` (below) can wrap — exactly `std::ranges::viewable_range`, the standard's own
    // name for "usable with view adaptors" (covers an lvalue range, borrowed, and an rvalue one,
    // owned by the result). Name a template parameter this way to accept "anything iterable" without
    // repeating that constraint by hand — e.g. `template <Iterable R> void process(R &&range);`.
    template <class R>
    concept Iterable = std::ranges::viewable_range<R>;

    namespace Detail {

        // Two ranges are `Chainable` if `Iter::chain()` (below) can walk one after the other:
        // both have to be `view`s with a fixed end (`common_range` — sentinel and iterator are the
        // same type, true of virtually every container/view in practice), and their elements need a
        // `common_reference_t` so dereferencing mid-chain always yields one consistent type
        // regardless of which side the cursor is currently on.
        template <class V1, class V2>
        concept Chainable = std::ranges::input_range<V1> && std::ranges::view<V1> && std::ranges::common_range<V1> &&
                             std::ranges::input_range<V2> && std::ranges::view<V2> && std::ranges::common_range<V2> &&
                             std::common_reference_with<std::ranges::range_reference_t<V1>, std::ranges::range_reference_t<V2>>;

        // A single-pass cursor over "every element of `V1`, then every element of `V2`" — the
        // iterator behind `ChainView` below. Deliberately input-only (`iterator_concept =
        // input_iterator_tag`) rather than forward/bidirectional/random-access: `Iter` is already a
        // consuming, single-pass, Rust-flavored wrapper (see its class docs), so a chained iterator
        // doesn't need to support revisiting a position, only advancing through it once.
        template <class V1, class V2>
        class ChainIterator {
          public:
            using value_type = std::common_type_t<std::ranges::range_value_t<V1>, std::ranges::range_value_t<V2>>;
            using difference_type = std::common_type_t<std::ranges::range_difference_t<V1>, std::ranges::range_difference_t<V2>>;
            using reference = std::common_reference_t<std::ranges::range_reference_t<V1>, std::ranges::range_reference_t<V2>>;
            using iterator_concept = std::input_iterator_tag;

            ChainIterator() = default;

            // Constructing directly from `[it1, end1)`/`[it2, end2)` decides which side we start on:
            // if `it1 == end1` already (the first range is empty, or this is being used to build the
            // "end" sentinel with `it1 == end1` on purpose), we start in the second range immediately.
            ChainIterator(std::ranges::iterator_t<V1> it1, std::ranges::iterator_t<V1> end1,
                          std::ranges::iterator_t<V2> it2, std::ranges::iterator_t<V2> end2)
                : it1_(std::move(it1)), end1_(std::move(end1)), it2_(std::move(it2)), end2_(std::move(end2)),
                  in_first_(it1_ != end1_) {
            }

            [[nodiscard]] reference operator*() const {
                if (in_first_) {
                    return static_cast<reference>(*it1_);
                }
                return static_cast<reference>(*it2_);
            }

            ChainIterator &operator++() {
                if (in_first_) {
                    ++it1_;
                    if (it1_ == end1_) {
                        in_first_ = false;
                    }
                } else {
                    ++it2_;
                }
                return *this;
            }

            void operator++(int) {
                ++*this;
            }

            [[nodiscard]] bool operator==(const ChainIterator &other) const {
                if (in_first_ != other.in_first_) {
                    return false;
                }
                return in_first_ ? (it1_ == other.it1_) : (it2_ == other.it2_);
            }

          private:
            std::ranges::iterator_t<V1> it1_{};
            std::ranges::iterator_t<V1> end1_{};
            std::ranges::iterator_t<V2> it2_{};
            std::ranges::iterator_t<V2> end2_{};
            bool in_first_ = false;
        };

        // The view behind `Iter::chain()`: `first`'s elements, then `second`'s. `begin()`/`end()` are
        // both `ChainIterator`s (a `common_range`) — `end()` is built with `it == end` on both sides,
        // which `ChainIterator`'s constructor already reads as "start in the second range", so it
        // doubles as the finished/sentinel state with no special-casing needed.
        template <class V1, class V2>
            requires Chainable<V1, V2>
        class ChainView : public std::ranges::view_interface<ChainView<V1, V2>> {
          public:
            ChainView() = default;

            constexpr ChainView(V1 first, V2 second)
                : first_(std::move(first)), second_(std::move(second)) {
            }

            [[nodiscard]] auto begin() {
                return ChainIterator<V1, V2>(std::ranges::begin(first_), std::ranges::end(first_),
                                              std::ranges::begin(second_), std::ranges::end(second_));
            }

            [[nodiscard]] auto end() {
                auto end1 = std::ranges::end(first_);
                auto end2 = std::ranges::end(second_);
                return ChainIterator<V1, V2>(end1, end1, end2, end2);
            }

          private:
            V1 first_;
            V2 second_;
        };

    } // namespace Detail

    // A Rust-flavored, lazily-chained wrapper over any `std::ranges` range — `.map()`/`.filter()`/
    // `.enumerate()`/`.zip()`/`.chain()`/`.take()`/`.skip()`/`.take_while()`/`.skip_while()`/`.rev()`
    // build up an adaptor chain without touching a single element (each is a thin `std::views::*`
    // call underneath, `.chain()` aside — see `Detail::ChainView` above, since `std::views::concat`
    // doesn't exist before C++26); `.for_each()`/`.fold()`/`.sum()`/`.count()`/`.any()`/`.all()`/
    // `.find()`/`.collect<Container>()` actually walk it. Get one via `iter()` below, not this
    // constructor directly.
    //
    // `Iter<V>` is itself a `std::ranges::view` (derives from `view_interface`), so it can be handed
    // to anything that takes a range — including back into `iter()`, `Sturdy.Async`'s `par_iter()`
    // (see Async/ParIter.cppm), or a plain range-`for`.
    //
    // Every method below consumes `view_` (moves out of it) to build the next stage, matching Rust's
    // `self`-by-value adaptor methods — chain them in one expression (`iter(v).map(f).filter(p)...`)
    // the way you normally would; if you do keep an intermediate in a named variable, `std::move` it
    // into the next call, since the original is left holding a moved-from view afterward.
    template <std::ranges::view V>
    class Iter : public std::ranges::view_interface<Iter<V>> {
      public:
        explicit constexpr Iter(V view) noexcept(std::is_nothrow_move_constructible_v<V>)
            : view_(std::move(view)) {
        }

        [[nodiscard]] constexpr auto begin() {
            return std::ranges::begin(view_);
        }

        [[nodiscard]] constexpr auto end() {
            return std::ranges::end(view_);
        }

        // --- Lazy adaptors -------------------------------------------------------------------

        template <class Fn>
            requires SyncWork<Fn &, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto map(Fn fn) {
            auto result = std::views::transform(std::move(view_), std::move(fn));
            return Iter<decltype(result)>(std::move(result));
        }

        template <class Pred>
            requires predicate<Pred &, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto filter(Pred pred) {
            auto result = std::views::filter(std::move(view_), std::move(pred));
            return Iter<decltype(result)>(std::move(result));
        }

        // Pairs each element with its index: `(usize, T)`.
        [[nodiscard]] auto enumerate() {
            auto result = std::views::enumerate(std::move(view_));
            return Iter<decltype(result)>(std::move(result));
        }

        template <std::ranges::viewable_range R2>
        [[nodiscard]] auto zip(R2 &&other) {
            auto result = std::views::zip(std::move(view_), std::views::all(std::forward<R2>(other)));
            return Iter<decltype(result)>(std::move(result));
        }

        // This iterator's elements, then `other`'s — see `Detail::ChainView` above for why this needs
        // its own hand-rolled view rather than a one-line `std::views::*` call like the other
        // adaptors here.
        template <Iterable R2>
            requires Detail::Chainable<V, std::views::all_t<R2>>
        [[nodiscard]] auto chain(R2 &&other) {
            auto other_view = std::views::all(std::forward<R2>(other));
            auto result = Detail::ChainView<V, decltype(other_view)>(std::move(view_), std::move(other_view));
            return Iter<decltype(result)>(std::move(result));
        }

        [[nodiscard]] auto take(std::ranges::range_difference_t<V> count) {
            auto result = std::views::take(std::move(view_), count);
            return Iter<decltype(result)>(std::move(result));
        }

        [[nodiscard]] auto skip(std::ranges::range_difference_t<V> count) {
            auto result = std::views::drop(std::move(view_), count);
            return Iter<decltype(result)>(std::move(result));
        }

        template <class Pred>
            requires predicate<Pred &, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto take_while(Pred pred) {
            auto result = std::views::take_while(std::move(view_), std::move(pred));
            return Iter<decltype(result)>(std::move(result));
        }

        template <class Pred>
            requires predicate<Pred &, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto skip_while(Pred pred) {
            auto result = std::views::drop_while(std::move(view_), std::move(pred));
            return Iter<decltype(result)>(std::move(result));
        }

        [[nodiscard]] auto rev()
            requires std::ranges::bidirectional_range<V>
        {
            auto result = std::views::reverse(std::move(view_));
            return Iter<decltype(result)>(std::move(result));
        }

        // --- Terminal consumers ----------------------------------------------------------------

        template <class Fn>
            requires SyncWork<Fn &, std::ranges::range_reference_t<V>>
        void for_each(Fn fn) {
            std::ranges::for_each(view_, fn);
        }

        template <class T, class Fn>
        [[nodiscard]] T fold(T init, Fn fn) {
            return std::ranges::fold_left(view_, std::move(init), fn);
        }

        [[nodiscard]] auto sum() {
            using T = std::ranges::range_value_t<V>;
            return std::ranges::fold_left(view_, T{}, std::plus<>{});
        }

        [[nodiscard]] usize count() {
            return static_cast<usize>(std::ranges::distance(view_));
        }

        template <class Pred>
            requires predicate<Pred &, std::ranges::range_reference_t<V>>
        [[nodiscard]] bool any(Pred pred) {
            return std::ranges::any_of(view_, pred);
        }

        template <class Pred>
            requires predicate<Pred &, std::ranges::range_reference_t<V>>
        [[nodiscard]] bool all(Pred pred) {
            return std::ranges::all_of(view_, pred);
        }

        // First element satisfying `pred`, or `nullopt` if none does.
        template <class Pred>
            requires predicate<Pred &, std::ranges::range_reference_t<V>>
        [[nodiscard]] optional<std::ranges::range_value_t<V>> find(Pred pred) {
            auto it = std::ranges::find_if(view_, pred);
            if (it == std::ranges::end(view_)) {
                return std::nullopt;
            }
            return std::ranges::range_value_t<V>(*it);
        }

        // `std::ranges::to<Container>()` under the hood — `Container` is a template-template
        // parameter (defaults to `std::vector`) so the element type is deduced, not repeated.
        template <template <class...> class Container = std::vector>
        [[nodiscard]] auto collect() {
            return std::ranges::to<Container>(std::move(view_));
        }

      private:
        V view_;
    };

    // Wraps any range in an `Iter` — the general "turn this into an iterator" entry point, and the
    // `Iterable` concept above is exactly its parameter constraint. A `std::ranges::range_adaptor_
    // closure`, so it works both as an ordinary call, `iter(range)`, and piped, `range | iter` —
    // the same two forms `std::views::all`/`std::views::common` support, and for the same reason:
    // there's no extra argument to bind first (unlike, say, `std::views::transform(fn)`), so the one
    // object serves as both the adaptor and its own closure.
    namespace Detail {

        struct IterFn : std::ranges::range_adaptor_closure<IterFn> {
            template <Iterable R>
            [[nodiscard]] constexpr auto operator()(R &&range) const {
                auto view = std::views::all(std::forward<R>(range));
                return Iter<decltype(view)>(std::move(view));
            }
        };

    } // namespace Detail

    inline constexpr Detail::IterFn iter{};

} // namespace SFT::Foundation
