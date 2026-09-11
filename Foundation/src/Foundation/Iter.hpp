#pragma once

#include <Foundation/Concepts.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <type_traits>
#include <tuple>
#include <utility>
#include <vector>

#if defined(__has_include)
#  if __has_include(<expected>)
#    include <expected>
#    if defined(__cpp_lib_expected)
#      define ST_FOUNDATION_HAS_STD_EXPECTED 1
#    else
#      define ST_FOUNDATION_HAS_STD_EXPECTED 0
#    endif
#  else
#    define ST_FOUNDATION_HAS_STD_EXPECTED 0
#  endif
#else
#  define ST_FOUNDATION_HAS_STD_EXPECTED 0
#endif

namespace SFT::Foundation {

    template <class R>
    concept Iterable = std::ranges::viewable_range<R>;

    template <class T>
    using Option = std::optional<T>;

#if ST_FOUNDATION_HAS_STD_EXPECTED
    template <class T, class E>
    using Result = std::expected<T, E>;
#else
    template <class T, class E>
    class Result;

    template <class E>
    class Result<void, E>;
#endif

    namespace Detail {
#if !ST_FOUNDATION_HAS_STD_EXPECTED
        template <class T, class E>
        class Result {
          public:
            using value_type = T;
            using error_type = E;

            Result(const T &value) : value_(value), has_value_(true) {}
            Result(T &&value) : value_(std::move(value)), has_value_(true) {}
            Result(const E &error, std::in_place_t) : error_(error), has_value_(false) {}
            Result(E &&error, std::in_place_t) : error_(std::move(error)), has_value_(false) {}

            static Result success(T value) { return Result(std::move(value)); }
            static Result failure(E error) { return Result(std::move(error), std::in_place); }

            [[nodiscard]] explicit operator bool() const noexcept { return has_value_; }
            [[nodiscard]] bool has_value() const noexcept { return has_value_; }
            [[nodiscard]] T &value() & { return *value_; }
            [[nodiscard]] const T &value() const & { return *value_; }
            [[nodiscard]] T &&value() && { return std::move(*value_); }
            [[nodiscard]] E &error() & { return *error_; }
            [[nodiscard]] const E &error() const & { return *error_; }
            [[nodiscard]] E &&error() && { return std::move(*error_); }

          private:
            std::optional<T> value_;
            std::optional<E> error_;
            bool has_value_ = false;
        };

        template <class E>
        class Result<void, E> {
          public:
            using value_type = void;
            using error_type = E;

            static Result success() { return Result(true); }
            static Result failure(E error) { return Result(std::move(error), false); }
            [[nodiscard]] explicit operator bool() const noexcept { return has_value_; }
            [[nodiscard]] bool has_value() const noexcept { return has_value_; }
            void value() const noexcept {}
            [[nodiscard]] E &error() & { return *error_; }
            [[nodiscard]] const E &error() const & { return *error_; }

          private:
            explicit Result(bool success) : has_value_(success) {}
            Result(E error, bool) : error_(std::move(error)), has_value_(false) {}
            std::optional<E> error_;
            bool has_value_ = false;
        };
#endif

        template <class T>
        using Stored = std::remove_cvref_t<T>;

        template <class R>
        using RangeValue = std::ranges::range_value_t<R>;

        template <class V1, class V2>
        concept Chainable = std::ranges::input_range<V1> && std::ranges::view<V1> &&
                            std::ranges::input_range<V2> && std::ranges::view<V2> &&
                            std::common_reference_with<std::ranges::range_reference_t<V1>,
                                                       std::ranges::range_reference_t<V2>>;

        template <class V1, class V2>
            requires Chainable<V1, V2>
        class ChainView : public std::ranges::view_interface<ChainView<V1, V2>> {
            V1 first_;
            V2 second_;

            template <bool Const>
            class Iterator {
                using P1 = std::conditional_t<Const, const V1, V1>;
                using P2 = std::conditional_t<Const, const V2, V2>;
                using I1 = std::ranges::iterator_t<P1>;
                using I2 = std::ranges::iterator_t<P2>;

              public:
                using difference_type = std::common_type_t<std::ranges::range_difference_t<P1>,
                                                           std::ranges::range_difference_t<P2>>;
                using value_type = std::common_type_t<std::ranges::range_value_t<P1>,
                                                      std::ranges::range_value_t<P2>>;
                using reference = std::common_reference_t<std::ranges::range_reference_t<P1>,
                                                          std::ranges::range_reference_t<P2>>;
                using iterator_concept = std::input_iterator_tag;

                Iterator() = default;
                Iterator(I1 a, I1 ae, I2 b, I2 be) : a_(std::move(a)), ae_(std::move(ae)), b_(std::move(b)), be_(std::move(be)), first_(a_ != ae_) {}
                reference operator*() const { return first_ ? static_cast<reference>(*a_) : static_cast<reference>(*b_); }
                Iterator &operator++() { if (first_) { ++a_; if (a_ == ae_) first_ = false; } else { ++b_; } return *this; }
                void operator++(int) { ++*this; }
                friend bool operator==(const Iterator &x, const Iterator &y) { return x.first_ == y.first_ && (x.first_ ? x.a_ == y.a_ : x.b_ == y.b_); }

              private:
                I1 a_{}; I1 ae_{}; I2 b_{}; I2 be_{}; bool first_ = false;
            };

          public:
            ChainView() = default;
            constexpr ChainView(V1 first, V2 second) : first_(std::move(first)), second_(std::move(second)) {}
            auto begin() { return Iterator<false>(std::ranges::begin(first_), std::ranges::end(first_), std::ranges::begin(second_), std::ranges::end(second_)); }
            auto end() { auto a = std::ranges::end(first_); auto b = std::ranges::end(second_); return Iterator<false>(a, a, b, b); }
            auto begin() const requires std::ranges::range<const V1> && std::ranges::range<const V2> { return Iterator<true>(std::ranges::begin(first_), std::ranges::end(first_), std::ranges::begin(second_), std::ranges::end(second_)); }
            auto end() const requires std::ranges::range<const V1> && std::ranges::range<const V2> { auto a = std::ranges::end(first_); auto b = std::ranges::end(second_); return Iterator<true>(a, a, b, b); }
        };

        template <std::ranges::view V>
        class IntersperseView : public std::ranges::view_interface<IntersperseView<V>> {
            V base_;
            std::ranges::range_value_t<V> separator_;

            class Iterator {
              public:
                using difference_type = std::ranges::range_difference_t<V>;
                using value_type = std::ranges::range_value_t<V>;
                using iterator_concept = std::input_iterator_tag;
                using reference = std::ranges::range_value_t<V>;
                Iterator() = default;
                Iterator(std::ranges::iterator_t<V> it, std::ranges::sentinel_t<V> end, std::ranges::range_value_t<V> sep)
                    : it_(std::move(it)), end_(std::move(end)), sep_(std::move(sep)) {}
                value_type operator*() const { return separator_pending_ ? sep_ : static_cast<value_type>(*it_); }
                Iterator &operator++() { if (separator_pending_) { separator_pending_ = false; ++it_; } else if (it_ != end_) { auto next = it_; ++next; separator_pending_ = next != end_; } return *this; }
                void operator++(int) { ++*this; }
                friend bool operator==(const Iterator &x, const Iterator &y) { return x.it_ == y.it_ && x.separator_pending_ == y.separator_pending_; }
              private:
                std::ranges::iterator_t<V> it_{}; std::ranges::sentinel_t<V> end_{}; value_type sep_{}; bool separator_pending_ = false;
            };
          public:
            IntersperseView(V base, std::ranges::range_value_t<V> separator) : base_(std::move(base)), separator_(std::move(separator)) {}
            auto begin() { return Iterator(std::ranges::begin(base_), std::ranges::end(base_), separator_); }
            auto end() { return Iterator(std::ranges::end(base_), std::ranges::end(base_), separator_); }
        };

        template <std::ranges::view V, class F>
        class ScanView : public std::ranges::view_interface<ScanView<V, F>> {
            V base_; F fn_;
            using State = std::remove_cvref_t<std::invoke_result_t<F &, std::ranges::range_value_t<V>>>;
          public:
            ScanView(V base, F fn) : base_(std::move(base)), fn_(std::move(fn)) {}
            class Iterator {
                std::ranges::iterator_t<V> it_{}; std::ranges::sentinel_t<V> end_{}; F *fn_{}; std::optional<State> state_{}; bool done_ = false;
              public:
                using value_type = State; using difference_type = std::ranges::range_difference_t<V>; using iterator_concept = std::input_iterator_tag;
                Iterator() = default;
                Iterator(std::ranges::iterator_t<V> it, std::ranges::sentinel_t<V> end, F *fn) : it_(std::move(it)), end_(std::move(end)), fn_(fn) { if (it_ == end_) done_ = true; else ++*this; }
                const State &operator*() const { return *state_; }
                Iterator &operator++() { if (done_) return *this; auto next = std::invoke(*fn_, std::move(*state_), *it_); ++it_; if (it_ == end_) { done_ = true; state_.reset(); } else state_ = std::move(next); return *this; }
                void operator++(int) { ++*this; }
                friend bool operator==(const Iterator &x, const Iterator &y) { return x.done_ == y.done_ && (x.done_ || x.it_ == y.it_); }
            };
            auto begin() { return Iterator(std::ranges::begin(base_), std::ranges::end(base_), &fn_); }
            auto end() { return Iterator(std::ranges::end(base_), std::ranges::end(base_), &fn_); }
        };

        template <std::ranges::view V>
        class FromFnView : public std::ranges::view_interface<FromFnView<V>> { };
    }

#if !ST_FOUNDATION_HAS_STD_EXPECTED
    template <class T, class E>
    using Result = Detail::Result<T, E>;
#endif

    template <std::ranges::view V>
    class Iter : public std::ranges::view_interface<Iter<V>> {
        V view_;

      public:
        explicit constexpr Iter(V view) noexcept(std::is_nothrow_move_constructible_v<V>) : view_(std::move(view)) {}
        [[nodiscard]] constexpr auto begin() { return std::ranges::begin(view_); }
        [[nodiscard]] constexpr auto end() { return std::ranges::end(view_); }
        [[nodiscard]] constexpr auto begin() const requires std::ranges::range<const V> { return std::ranges::begin(view_); }
        [[nodiscard]] constexpr auto end() const requires std::ranges::range<const V> { return std::ranges::end(view_); }

        template <class Fn> requires std::invocable<Fn &, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto map(Fn fn) { auto r = std::views::transform(std::move(view_), std::move(fn)); return Iter<decltype(r)>(std::move(r)); }
        template <class Fn> requires std::invocable<Fn &, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto map_while(Fn fn) { auto mapped = std::views::transform(std::move(view_), std::move(fn)); auto limited = std::views::take_while(std::move(mapped), [](const auto &x) { return x.has_value(); }); auto r = std::views::transform(std::move(limited), [](auto &&x) -> decltype(auto) { return *std::forward<decltype(x)>(x); }); return Iter<decltype(r)>(std::move(r)); }
        template <class Pred> requires std::predicate<Pred &, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto filter(Pred pred) { auto r = std::views::filter(std::move(view_), std::move(pred)); return Iter<decltype(r)>(std::move(r)); }
        template <class Fn> requires std::invocable<Fn &, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto filter_map(Fn fn) { auto mapped = std::views::transform(std::move(view_), std::move(fn)); auto filtered = std::views::filter(std::move(mapped), [](const auto &x) { return x.has_value(); }); auto r = std::views::transform(std::move(filtered), [](auto &&x) -> decltype(auto) { return *std::forward<decltype(x)>(x); }); return Iter<decltype(r)>(std::move(r)); }
        template <class Fn> requires std::invocable<Fn &, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto flat_map(Fn fn) { auto r = std::views::join(std::views::transform(std::move(view_), std::move(fn))); return Iter<decltype(r)>(std::move(r)); }
        [[nodiscard]] auto flatten() requires std::ranges::range<std::ranges::range_reference_t<V>> { auto r = std::views::join(std::move(view_)); return Iter<decltype(r)>(std::move(r)); }
        template <class Fn> requires std::invocable<Fn &, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto inspect(Fn fn) { auto r = std::views::transform(std::move(view_), [fn = std::move(fn)](auto &&x) mutable -> decltype(auto) { std::invoke(fn, x); return std::forward<decltype(x)>(x); }); return Iter<decltype(r)>(std::move(r)); }
        [[nodiscard]] auto take(std::ranges::range_difference_t<V> n) { auto r = std::views::take(std::move(view_), n); return Iter<decltype(r)>(std::move(r)); }
        [[nodiscard]] auto skip(std::ranges::range_difference_t<V> n) { auto r = std::views::drop(std::move(view_), n); return Iter<decltype(r)>(std::move(r)); }
        template <class Pred> requires std::predicate<Pred &, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto take_while(Pred p) { auto r = std::views::take_while(std::move(view_), std::move(p)); return Iter<decltype(r)>(std::move(r)); }
        template <class Pred> requires std::predicate<Pred &, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto skip_while(Pred p) { auto r = std::views::drop_while(std::move(view_), std::move(p)); return Iter<decltype(r)>(std::move(r)); }
        [[nodiscard]] auto step_by(std::ranges::range_difference_t<V> step) { auto r = std::views::stride(std::move(view_), step); return Iter<decltype(r)>(std::move(r)); }
        [[nodiscard]] auto enumerate() { auto r = std::views::zip(std::views::iota(std::ranges::range_difference_t<V>{0}), std::move(view_)); return Iter<decltype(r)>(std::move(r)); }
        template <std::ranges::viewable_range R2> [[nodiscard]] auto zip(R2 &&other) { auto r = std::views::zip(std::move(view_), std::views::all(std::forward<R2>(other))); return Iter<decltype(r)>(std::move(r)); }
        template <std::ranges::viewable_range R2> requires Detail::Chainable<V, std::views::all_t<R2>>
        [[nodiscard]] auto chain(R2 &&other) { auto ov = std::views::all(std::forward<R2>(other)); auto r = Detail::ChainView<V, decltype(ov)>(std::move(view_), std::move(ov)); return Iter<decltype(r)>(std::move(r)); }
        [[nodiscard]] auto rev() requires std::ranges::bidirectional_range<V> { auto r = std::views::reverse(std::move(view_)); return Iter<decltype(r)>(std::move(r)); }
        [[nodiscard]] auto intersperse(std::ranges::range_value_t<V> separator) { auto r = Detail::IntersperseView<V>(std::move(view_), std::move(separator)); return Iter<decltype(r)>(std::move(r)); }
        template <class F> [[nodiscard]] auto scan(F fn) { auto r = Detail::ScanView<V, F>(std::move(view_), std::move(fn)); return Iter<decltype(r)>(std::move(r)); }
        [[nodiscard]] auto copied() requires std::is_pointer_v<std::remove_reference_t<std::ranges::range_reference_t<V>>> || std::is_copy_constructible_v<std::ranges::range_value_t<V>> { auto r = std::views::transform(std::move(view_), [](auto &&x) { return std::remove_cvref_t<decltype(x)>(x); }); return Iter<decltype(r)>(std::move(r)); }
        [[nodiscard]] auto cloned() requires std::copy_constructible<std::ranges::range_value_t<V>> { auto r = std::views::transform(std::move(view_), [](const auto &x) { return x; }); return Iter<decltype(r)>(std::move(r)); }
        [[nodiscard]] auto by_ref() & { auto r = std::ranges::subrange(std::ranges::begin(view_), std::ranges::end(view_)); return Iter<decltype(r)>(std::move(r)); }

        template <class Fn> requires std::invocable<Fn &, std::ranges::range_reference_t<V>>
        void for_each(Fn fn) { std::ranges::for_each(view_, std::move(fn)); }
        template <class T, class Fn> [[nodiscard]] T fold(T init, Fn fn) { return std::ranges::fold_left(view_, std::move(init), std::move(fn)); }
        template <class T, class Fn> [[nodiscard]] T fold_right(T init, Fn fn) requires std::ranges::bidirectional_range<V> && std::ranges::common_range<V> { return std::ranges::fold_right(view_, std::move(init), std::move(fn)); }
        [[nodiscard]] auto sum() { using T = std::ranges::range_value_t<V>; return std::ranges::fold_left(view_, T{}, std::plus<>{}); }
        [[nodiscard]] auto product() { using T = std::ranges::range_value_t<V>; return std::ranges::fold_left(view_, T{1}, std::multiplies<>{}); }
        [[nodiscard]] usize count() { return static_cast<usize>(std::ranges::distance(view_)); }
        [[nodiscard]] bool is_empty() { return std::ranges::begin(view_) == std::ranges::end(view_); }
        template <class Pred> requires std::predicate<Pred &, std::ranges::range_reference_t<V>> [[nodiscard]] bool any(Pred p) { return std::ranges::any_of(view_, std::move(p)); }
        template <class Pred> requires std::predicate<Pred &, std::ranges::range_reference_t<V>> [[nodiscard]] bool all(Pred p) { return std::ranges::all_of(view_, std::move(p)); }
        template <class Pred> requires std::predicate<Pred &, std::ranges::range_reference_t<V>> [[nodiscard]] auto find(Pred p) -> Option<std::ranges::range_value_t<V>> { auto it = std::ranges::find_if(view_, std::move(p)); if (it == std::ranges::end(view_)) return std::nullopt; return *it; }
        template <class Fn> [[nodiscard]] auto find_map(Fn fn) { for (auto &&x : view_) { auto y = std::invoke(fn, x); if (y) return y; } using R = std::remove_cvref_t<std::invoke_result_t<Fn &, std::ranges::range_reference_t<V>>>; return R{}; }
        template <class Pred> [[nodiscard]] Option<usize> position(Pred p) { usize i = 0; for (auto &&x : view_) { if (std::invoke(p, x)) return i; ++i; } return std::nullopt; }
        template <class Pred> [[nodiscard]] Option<usize> rposition(Pred p) requires std::ranges::bidirectional_range<V> && std::ranges::common_range<V> { usize n = count(); while (n) { --n; auto it = std::ranges::begin(view_); std::ranges::advance(it, static_cast<std::ranges::range_difference_t<V>>(n)); if (std::invoke(p, *it)) return n; } return std::nullopt; }
        template <class Pred> [[nodiscard]] Option<std::ranges::range_value_t<V>> find_value(Pred p) { auto it = std::ranges::find_if(view_, std::move(p)); if (it == std::ranges::end(view_)) return std::nullopt; return *it; }
        [[nodiscard]] auto min() requires std::ranges::forward_range<V> { return std::ranges::min_element(view_) == std::ranges::end(view_) ? Option<std::ranges::range_value_t<V>>{} : Option<std::ranges::range_value_t<V>>{*std::ranges::min_element(view_)}; }
        [[nodiscard]] auto max() requires std::ranges::forward_range<V> { return std::ranges::max_element(view_) == std::ranges::end(view_) ? Option<std::ranges::range_value_t<V>>{} : Option<std::ranges::range_value_t<V>>{*std::ranges::max_element(view_)}; }
        template <class Pred> [[nodiscard]] bool is_sorted(Pred pred) { return std::ranges::is_sorted(view_, std::move(pred)); }
        [[nodiscard]] bool is_sorted() { return std::ranges::is_sorted(view_); }
        [[nodiscard]] bool eq(const auto &other) { return std::ranges::equal(view_, other); }
        template <class Pred> [[nodiscard]] bool eq_by(const auto &other, Pred pred) { return std::ranges::equal(view_, other, std::move(pred)); }
        template <class R2> [[nodiscard]] auto cmp(const R2 &other) requires std::three_way_comparable_with<std::ranges::range_value_t<V>, std::ranges::range_value_t<R2>> { return std::lexicographical_compare_three_way(begin(), end(), std::ranges::begin(other), std::ranges::end(other)); }
        template <class Pred> [[nodiscard]] auto min_by(Pred pred) { auto it = std::ranges::min_element(view_, std::move(pred)); if (it == std::ranges::end(view_)) return Option<std::ranges::range_value_t<V>>{}; return Option<std::ranges::range_value_t<V>>{*it}; }
        template <class Pred> [[nodiscard]] auto max_by(Pred pred) { auto it = std::ranges::max_element(view_, std::move(pred)); if (it == std::ranges::end(view_)) return Option<std::ranges::range_value_t<V>>{}; return Option<std::ranges::range_value_t<V>>{*it}; }
        template <class Pred> [[nodiscard]] bool lt(const auto &other, Pred pred) { return std::ranges::lexicographical_compare(view_, other, std::move(pred)); }
        template <template <class...> class Container = std::vector> [[nodiscard]] auto collect() { return std::ranges::to<Container>(std::move(view_)); }
        template <class Container> void collect_into(Container &out) { for (auto &&x : view_) out.emplace_back(std::forward<decltype(x)>(x)); }
        template <class Pred> [[nodiscard]] auto partition(Pred pred) { using T=std::ranges::range_value_t<V>; std::pair<std::vector<T>,std::vector<T>> out; for(auto &&x:view_) (pred(x)?out.first:out.second).emplace_back(x); return out; }
        [[nodiscard]] auto unzip() { using A=std::remove_cvref_t<std::tuple_element_t<0,std::ranges::range_value_t<V>>>; using B=std::remove_cvref_t<std::tuple_element_t<1,std::ranges::range_value_t<V>>>; std::pair<std::vector<A>,std::vector<B>> out; for(auto &&x:view_){out.first.emplace_back(std::get<0>(x));out.second.emplace_back(std::get<1>(x));} return out; }
        [[nodiscard]] auto last() { auto it = std::ranges::end(view_); if (it == std::ranges::begin(view_)) return Option<std::ranges::range_value_t<V>>{}; if constexpr (std::ranges::common_range<V>) { return Option<std::ranges::range_value_t<V>>{*--it}; } else { return Option<std::ranges::range_value_t<V>>{std::ranges::fold_left(view_, Option<std::ranges::range_value_t<V>>{}, [](auto, auto x) { return Option<std::ranges::range_value_t<V>>{x}; })}; } }

        template <class T, class Fn> requires std::invocable<Fn &, T, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto reduce(T init, Fn fn) { for (auto &&x : view_) init = std::invoke(fn, std::move(init), x); return init; }

        // Rust-style Option helpers for iterator-produced optional values.
        template <class T, class Fn> [[nodiscard]] static auto map_some(std::optional<T> value, Fn fn) { using U = std::remove_cvref_t<std::invoke_result_t<Fn &, T>>; if (value) return std::optional<U>{std::invoke(fn, std::move(*value))}; return std::optional<U>{}; }
        template <class T, class Fn> [[nodiscard]] static auto map_none(std::optional<T> value, Fn fn) { using U = std::remove_cvref_t<std::invoke_result_t<Fn &>>; if (value) return std::optional<U>{}; return std::optional<U>{std::invoke(fn)}; }

#if ST_FOUNDATION_HAS_STD_EXPECTED
        template <class T, class E, class Fn> [[nodiscard]] static auto map_ok(std::expected<T, E> value, Fn fn) { using U = std::remove_cvref_t<std::invoke_result_t<Fn &, T>>; if (value) return std::expected<U, E>{std::in_place, std::invoke(fn, std::move(*value))}; return std::expected<U, E>{std::unexpect, std::move(value.error())}; }
        template <class T, class E, class Fn> [[nodiscard]] static auto map_err(std::expected<T, E> value, Fn fn) { using U = std::remove_cvref_t<std::invoke_result_t<Fn &, E>>; if (value) return std::expected<T, U>{std::in_place, std::move(*value)}; return std::expected<T, U>{std::unexpect, std::invoke(fn, std::move(value.error()))}; }
        template <class E, class Fn> [[nodiscard]] static auto map_ok(std::expected<void, E> value, Fn fn) { using U = std::remove_cvref_t<std::invoke_result_t<Fn &>>; if (value) return std::expected<U, E>{std::in_place, std::invoke(fn)}; return std::expected<U, E>{std::unexpect, std::move(value.error())}; }
        template <class E, class Fn> [[nodiscard]] static auto map_err(std::expected<void, E> value, Fn fn) { using U = std::remove_cvref_t<std::invoke_result_t<Fn &, E>>; if (value) return std::expected<void, U>{}; return std::expected<void, U>{std::unexpect, std::invoke(fn, std::move(value.error()))}; }
#endif
    };

    template <class T>
    [[nodiscard]] auto empty() {
        return Iter<std::ranges::empty_view<T>>(std::ranges::empty_view<T>{});
    }

    template <class T>
    [[nodiscard]] auto once(T value) {
        auto v = std::views::single(std::move(value));
        return Iter<decltype(v)>(std::move(v));
    }

    template <class F>
    [[nodiscard]] auto once_with(F fn) {
        return once(std::invoke(std::move(fn)));
    }

    template <class T>
    [[nodiscard]] auto repeat(T value) {
        auto v = std::views::repeat(std::move(value));
        return Iter<decltype(v)>(std::move(v));
    }

    template <class T>
    [[nodiscard]] auto repeat_n(T value, usize n) {
        auto v = std::views::repeat(std::move(value), static_cast<std::ranges::range_difference_t<std::ranges::repeat_view<T, usize>>>(n));
        return Iter<decltype(v)>(std::move(v));
    }

    template <class F>
    [[nodiscard]] auto repeat_with(F fn) {
        auto v = std::views::iota(usize{0}) | std::views::transform([fn = std::move(fn)](usize) mutable { return std::invoke(fn); });
        return Iter<decltype(v)>(std::move(v));
    }

    namespace Detail {
        struct IterFn : std::ranges::range_adaptor_closure<IterFn> {
            template <Iterable R> [[nodiscard]] constexpr auto operator()(R &&range) const { auto view = std::views::all(std::forward<R>(range)); return Iter<decltype(view)>(std::move(view)); }
        };
    }

    inline constexpr Detail::IterFn iter{};

    template <class T> [[nodiscard]] constexpr Option<T> some(T value) { return std::optional<T>{std::move(value)}; }
    [[nodiscard]] constexpr std::nullopt_t none() noexcept { return std::nullopt; }

    template <class T, class Fn>
    [[nodiscard]] auto map_some(std::optional<T> value, Fn fn) {
        return Iter<std::ranges::empty_view<int>>::map_some(std::move(value), std::move(fn));
    }

    template <class T, class Fn>
    [[nodiscard]] auto map_none(std::optional<T> value, Fn fn) {
        return Iter<std::ranges::empty_view<int>>::map_none(std::move(value), std::move(fn));
    }

#if ST_FOUNDATION_HAS_STD_EXPECTED
    template <class T, class E, class Fn>
    [[nodiscard]] auto map_ok(std::expected<T, E> value, Fn fn) {
        return Iter<std::ranges::empty_view<int>>::map_ok(std::move(value), std::move(fn));
    }
    template <class T, class E, class Fn>
    [[nodiscard]] auto map_err(std::expected<T, E> value, Fn fn) {
        return Iter<std::ranges::empty_view<int>>::map_err(std::move(value), std::move(fn));
    }
#endif

} // namespace SFT::Foundation
