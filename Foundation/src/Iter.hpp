#pragma once

#include <Foundation/src/Concepts.hpp>

#include <algorithm>
#include <concepts>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <type_traits>
#include <vector>


using std::optional;
using std::predicate;

namespace SFT::Foundation {


    template <class R>
    concept Iterable = std::ranges::viewable_range<R>;

    namespace Detail {


        template <class V1, class V2>
        concept Chainable = std::ranges::input_range<V1> && std::ranges::view<V1> && std::ranges::common_range<V1> &&
                             std::ranges::input_range<V2> && std::ranges::view<V2> && std::ranges::common_range<V2> &&
                             std::common_reference_with<std::ranges::range_reference_t<V1>, std::ranges::range_reference_t<V2>>;


        template <class V1, class V2>
        class ChainIterator {
          public:
            using value_type = std::common_type_t<std::ranges::range_value_t<V1>, std::ranges::range_value_t<V2>>;
            using difference_type = std::common_type_t<std::ranges::range_difference_t<V1>, std::ranges::range_difference_t<V2>>;
            using reference = std::common_reference_t<std::ranges::range_reference_t<V1>, std::ranges::range_reference_t<V2>>;
            using iterator_concept = std::input_iterator_tag;

            /// Constructs a `ChainIterator` in its default state.
            ///
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            ChainIterator() = default;


            /// Constructs a `ChainIterator` from the supplied initialization values.
            ///
            /// @param it1 `it1` value used by the operation.
            /// @param end1 `end1` value used by the operation.
            /// @param it2 `it2` value used by the operation.
            /// @param end2 `end2` value used by the operation.
            ///
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            ChainIterator(std::ranges::iterator_t<V1> it1, std::ranges::iterator_t<V1> end1,
                          std::ranges::iterator_t<V2> it2, std::ranges::iterator_t<V2> end2)
                : it1_(std::move(it1)), end1_(std::move(end1)), it2_(std::move(it2)), end2_(std::move(end2)),
                  in_first_(it1_ != end1_) {
            }

            /// Dereferences this iterator or handle.
            ///
            /// @return Returns the value or reference currently addressed by the iterator/handle.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] reference operator*() const {
                if (in_first_) {
                    return static_cast<reference>(*it1_);
                }
                return static_cast<reference>(*it2_);
            }

            /// Advances the `ChainIterator` to its next value or element.
            ///
            /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

            /// Advances the `ChainIterator` to its next value or element.
            ///
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            void operator++(int) {
                ++*this;
            }

            /// Compares the operands for equality.
            ///
            /// @param other Other object used by the operation.
            ///
            /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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


        template <class V1, class V2>
            requires Chainable<V1, V2>
        class ChainView : public std::ranges::view_interface<ChainView<V1, V2>> {
          public:
            /// Constructs a `ChainView` in its default state.
            ///
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            ChainView() = default;

            /// Constructs a `ChainView` from the supplied initialization values.
            ///
            /// @param first First position or element included in the operation.
            /// @param second `second` value used by the operation.
            ///
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            constexpr ChainView(V1 first, V2 second)
                : first_(std::move(first)), second_(std::move(second)) {
            }

            /// Returns an iterator to the first element in the range.
            ///
            /// @return Returns an iterator referring to the first element.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] auto begin() {
                return ChainIterator<V1, V2>(std::ranges::begin(first_), std::ranges::end(first_),
                                              std::ranges::begin(second_), std::ranges::end(second_));
            }

            /// Returns the one-past-the-end iterator for the range.
            ///
            /// @return Returns the one-past-the-end iterator.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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


    template <std::ranges::view V>
    class Iter : public std::ranges::view_interface<Iter<V>> {
      public:
        /// Constructs a `Iter` from the supplied initialization values.
        ///
        /// @param view `view` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit constexpr Iter(V view) noexcept(std::is_nothrow_move_constructible_v<V>)
            : view_(std::move(view)) {
        }

        /// Returns an iterator to the first element in the range.
        ///
        /// @return Returns an iterator referring to the first element.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] constexpr auto begin() {
            return std::ranges::begin(view_);
        }

        /// Returns the one-past-the-end iterator for the range.
        ///
        /// @return Returns the one-past-the-end iterator.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] constexpr auto end() {
            return std::ranges::end(view_);
        }


        /// Maps the requested resource for access.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class Fn>
            requires SyncWork<Fn &, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto map(Fn fn) {
            auto result = std::views::transform(std::move(view_), std::move(fn));
            return Iter<decltype(result)>(std::move(result));
        }

        /// Returns the current or globally available filter value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class Pred>
            requires predicate<Pred &, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto filter(Pred pred) {
            auto result = std::views::filter(std::move(view_), std::move(pred));
            return Iter<decltype(result)>(std::move(result));
        }


        /// Enumerates the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @return Returns the current enumerate value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] auto enumerate() {
            auto result = std::views::enumerate(std::move(view_));
            return Iter<decltype(result)>(std::move(result));
        }

        /// Returns the current or globally available zip value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <std::ranges::viewable_range R2>
        [[nodiscard]] auto zip(R2 &&other) {
            auto result = std::views::zip(std::move(view_), std::views::all(std::forward<R2>(other)));
            return Iter<decltype(result)>(std::move(result));
        }


        /// Returns the current or globally available chain value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <Iterable R2>
            requires Detail::Chainable<V, std::views::all_t<R2>>
        [[nodiscard]] auto chain(R2 &&other) {
            auto other_view = std::views::all(std::forward<R2>(other));
            auto result = Detail::ChainView<V, decltype(other_view)>(std::move(view_), std::move(other_view));
            return Iter<decltype(result)>(std::move(result));
        }

        /// Performs the take operation for `Iter` using the supplied arguments.
        ///
        /// @param count Number of elements or operations to process.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] auto take(std::ranges::range_difference_t<V> count) {
            auto result = std::views::take(std::move(view_), count);
            return Iter<decltype(result)>(std::move(result));
        }

        /// Performs the skip operation for `Iter` using the supplied arguments.
        ///
        /// @param count Number of elements or operations to process.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] auto skip(std::ranges::range_difference_t<V> count) {
            auto result = std::views::drop(std::move(view_), count);
            return Iter<decltype(result)>(std::move(result));
        }

        /// Returns the current or globally available take while value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class Pred>
            requires predicate<Pred &, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto take_while(Pred pred) {
            auto result = std::views::take_while(std::move(view_), std::move(pred));
            return Iter<decltype(result)>(std::move(result));
        }

        /// Returns the current or globally available skip while value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class Pred>
            requires predicate<Pred &, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto skip_while(Pred pred) {
            auto result = std::views::drop_while(std::move(view_), std::move(pred));
            return Iter<decltype(result)>(std::move(result));
        }

        /// Returns the current or globally available rev value.
        ///
        /// @return Returns the current rev value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] auto rev()
            requires std::ranges::bidirectional_range<V>
        {
            auto result = std::views::reverse(std::move(view_));
            return Iter<decltype(result)>(std::move(result));
        }


        /// Invokes the supplied function once for each element in the input range.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class Fn>
            requires SyncWork<Fn &, std::ranges::range_reference_t<V>>
        void for_each(Fn fn) {
            std::ranges::for_each(view_, fn);
        }

        /// Returns the current or globally available fold value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class T, class Fn>
        [[nodiscard]] T fold(T init, Fn fn) {
            return std::ranges::fold_left(view_, std::move(init), fn);
        }

        /// Returns the current or globally available sum value.
        ///
        /// @return Returns the current sum value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] auto sum() {
            using T = std::ranges::range_value_t<V>;
            return std::ranges::fold_left(view_, T{}, std::plus<>{});
        }

        /// Returns the count for this `Iter`.
        ///
        /// @return Returns the current count value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize count() {
            return static_cast<usize>(std::ranges::distance(view_));
        }

        /// Returns the current or globally available any value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class Pred>
            requires predicate<Pred &, std::ranges::range_reference_t<V>>
        [[nodiscard]] bool any(Pred pred) {
            return std::ranges::any_of(view_, pred);
        }

        /// Returns the current or globally available all value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class Pred>
            requires predicate<Pred &, std::ranges::range_reference_t<V>>
        [[nodiscard]] bool all(Pred pred) {
            return std::ranges::all_of(view_, pred);
        }


        /// Finds the requested entry in the available state.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        template <class Pred>
            requires predicate<Pred &, std::ranges::range_reference_t<V>>
        [[nodiscard]] optional<std::ranges::range_value_t<V>> find(Pred pred) {
            auto it = std::ranges::find_if(view_, pred);
            if (it == std::ranges::end(view_)) {
                return std::nullopt;
            }
            return std::ranges::range_value_t<V>(*it);
        }


        /// Collects the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <template <class...> class Container = std::vector>
        [[nodiscard]] auto collect() {
            return std::ranges::to<Container>(std::move(view_));
        }

      private:
        V view_;
    };


    namespace Detail {

        struct IterFn : std::ranges::range_adaptor_closure<IterFn> {
            /// Invokes the callable behavior provided by `IterFn`.
            ///
            /// @param range Range of values to process.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            template <Iterable R>
            [[nodiscard]] constexpr auto operator()(R &&range) const {
                auto view = std::views::all(std::forward<R>(range));
                return Iter<decltype(view)>(std::move(view));
            }
        };

    } // namespace Detail

    inline constexpr Detail::IterFn iter{};

} // namespace SFT::Foundation
