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


    template <std::size_t N, class... Ts>
    using PackElement = Ts...[N];


    template <class... Ts>
    using LastPackElement = Ts...[sizeof...(Ts) - 1];


    /// Returns the current or globally available nth arg value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    template <std::size_t N, class... Args>
    [[nodiscard]] constexpr decltype(auto) nth_arg(Args &&...args) noexcept {
        return std::forward<Args...[N]>(args...[N]);
    }


    /// Returns the current or globally available last arg value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    template <class... Args>
    [[nodiscard]] constexpr decltype(auto) last_arg(Args &&...args) noexcept {
        return nth_arg<sizeof...(Args) - 1>(std::forward<Args>(args)...);
    }

    namespace Detail {

        /// Packs indexing smoke test using the supplied arguments and current state.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] consteval bool pack_indexing_smoke_test() noexcept {
            static_assert(std::same_as<PackElement<0, int, float, char>, int>);
            static_assert(std::same_as<PackElement<2, int, float, char>, char>);
            static_assert(std::same_as<LastPackElement<int, float, char>, char>);
            return nth_arg<1>(10, 20, 30) == 20 && last_arg(10, 20, 30) == 30;
        }
        static_assert(pack_indexing_smoke_test());

    } // namespace Detail

} // namespace SFT::Foundation
