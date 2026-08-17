#pragma once










#if !defined(__cpp_pack_indexing)
    #error "SturdyEngine5 requires a C++26 pack-indexing implementation (P2662, __cpp_pack_indexing). \
This toolchain accepts -std=c++26 but hasn't implemented it yet -- see Foundation/src/Cxx26.hpp."
#endif
#if !defined(__cpp_placeholder_variables)
    #error "SturdyEngine5 requires a C++26 placeholder-variables implementation (P2169, \
__cpp_placeholder_variables). This toolchain accepts -std=c++26 but hasn't implemented it yet -- see \
Foundation/src/Cxx26.hpp."
#endif

#include <concepts>
#include <cstddef>
#include <utility>

namespace SFT::Foundation {

    /// The Nth type in `Ts...`, via pack indexing rather than the classic `tuple_element_t<N,
    /// tuple<Ts...>>` trick — which has to instantiate a whole `tuple` type just to answer one lookup.
    template <std::size_t N, class... Ts>
    using PackElement = Ts...[N];

    /// The last type in `Ts...`. Common in variadic template code (a trailing "options"/context
    /// argument, a builder's final step, ...) that would otherwise need `PackElement<sizeof...(Ts) - 1,
    /// Ts...>` spelled out by hand at every call site.
    template <class... Ts>
    using LastPackElement = Ts...[sizeof...(Ts) - 1];

    /// The Nth argument of a call-site parameter pack, forwarded with its original value category — the
    /// value-level counterpart to `PackElement`. Picks the element directly rather than building a
    /// `std::tuple` just to call `std::get<N>` on it.
    template <std::size_t N, class... Args>
    [[nodiscard]] constexpr decltype(auto) nth_arg(Args &&...args) noexcept {
        return std::forward<Args...[N]>(args...[N]);
    }

    /// The last argument of a call-site parameter pack, forwarded with its original value category.
    template <class... Args>
    [[nodiscard]] constexpr decltype(auto) last_arg(Args &&...args) noexcept {
        return nth_arg<sizeof...(Args) - 1>(std::forward<Args>(args)...);
    }

    namespace Detail {

        [[nodiscard]] consteval bool pack_indexing_smoke_test() noexcept {
            static_assert(std::same_as<PackElement<0, int, float, char>, int>);
            static_assert(std::same_as<PackElement<2, int, float, char>, char>);
            static_assert(std::same_as<LastPackElement<int, float, char>, char>);
            return nth_arg<1>(10, 20, 30) == 20 && last_arg(10, 20, 30) == 30;
        }
        static_assert(pack_indexing_smoke_test());

    } // namespace Detail

} // namespace SFT::Foundation
