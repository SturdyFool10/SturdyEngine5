#pragma once

#include <iterator>
#include <ranges>
#include <tuple>
#include <utility>
#include <version>

/// C++23 range facilities the engine uses that not every standard library has yet (libc++ before 22 lacks
/// `views::stride` and `ranges::fold_right`; Apple ships libc++ 19). Each one defers to the standard version where it
/// exists, so a toolchain that has it behaves exactly as before.
namespace SFT::Foundation::Compat {

    /// `std::views::stride(range, n)`: every n-th element, starting with the first. `n` must be positive.
    template <std::ranges::viewable_range R>
    [[nodiscard]] auto stride(R &&range, std::ranges::range_difference_t<R> n) {
#if defined(__cpp_lib_ranges_stride)
        return std::views::stride(std::forward<R>(range), n);
#else
        using Diff = std::ranges::range_difference_t<R>;
        auto base = std::views::all(std::forward<R>(range));
        using V = decltype(base);
        if constexpr (std::ranges::random_access_range<V> && std::ranges::sized_range<V>) {
            // Index straight into the base: keeps random access, size and reference semantics.
            const Diff count = (static_cast<Diff>(std::ranges::size(base)) + n - 1) / n;
            // Captures the base's begin iterator rather than the view, so the result can be iterated through a const object
            // (std::views::stride allows that when its base does). A view's iterators stay valid across copies of it.
            return std::views::iota(Diff{0}, count) | std::views::transform([first = std::ranges::begin(base), n](Diff i) -> std::ranges::range_reference_t<V> {
                       return *(first + i * n);
                   });
        } else {
            // Any other range: pair each element with its position and keep every n-th.
            return std::views::zip(std::views::iota(Diff{0}), std::move(base)) |
                   std::views::filter([n](const auto &indexed) { return std::get<0>(indexed) % n == 0; }) |
                   std::views::transform([](auto &&indexed) -> std::ranges::range_reference_t<V> {
                       return std::get<1>(std::forward<decltype(indexed)>(indexed));
                   });
        }
#endif
    }

    /// `std::ranges::fold_right(range, init, f)`: `f(x1, f(x2, ... f(xn, init)))`.
    template <std::ranges::bidirectional_range R, class T, class F>
        requires std::ranges::common_range<R>
    [[nodiscard]] T fold_right(R &&range, T init, F f) {
        auto first = std::ranges::begin(range);
        auto it = std::ranges::end(range);
        while (it != first) {
            --it;
            init = std::invoke(f, *it, std::move(init));
        }
        return init;
    }

} // namespace SFT::Foundation::Compat
