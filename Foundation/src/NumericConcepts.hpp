#pragma once

#include <Foundation/src/Concepts.hpp>
#include <Foundation/src/Wide.hpp>

#include <concepts>



using std::floating_point;
using std::integral;
using std::same_as;
using std::unsigned_integral;

namespace SFT::Foundation {






    namespace Detail {

        /// The 256-bit unsigned wide type.
        template <class T>
        concept WideUnsignedInteger = same_as<Detail::Unqualified<T>, u256>;

        /// The 256-bit signed wide type.
        template <class T>
        concept WideSignedInteger = same_as<Detail::Unqualified<T>, i256>;

        /// The double-double / quad-double wide floats (`f128` / `f256`).
        template <class T>
        concept WideFloat = same_as<Detail::Unqualified<T>, f128> || same_as<Detail::Unqualified<T>, f256>;

    } // namespace Detail

    /// Any unsigned integer — the built-in unsigned types plus `u128` and `u256`. Excludes `bool`/`b8`.
    template <class T>
    concept UnsignedInteger = !Boolean<T> &&
                              (unsigned_integral<Detail::Unqualified<T>> ||
                               same_as<Detail::Unqualified<T>, u128> || Detail::WideUnsignedInteger<T>);

    /// Any integer, signed or unsigned — the built-in integrals plus `i128`/`u128` and `i256`/`u256`.
    /// Excludes `bool`/`b8`.
    template <class T>
    concept Integer = !Boolean<T> &&
                      (integral<Detail::Unqualified<T>> || same_as<Detail::Unqualified<T>, i128> ||
                       same_as<Detail::Unqualified<T>, u128> || Detail::WideSignedInteger<T> ||
                       Detail::WideUnsignedInteger<T>);

    /// Any floating-point type — the built-in `float`/`double` plus the wide `f128`/`f256`.
    template <class T>
    concept Float = floating_point<Detail::Unqualified<T>> || Detail::WideFloat<T>;

    /// Any number: `Integer` or `Float` (built-in or wide).
    template <class T>
    concept Number = Integer<T> || Float<T>;

} // namespace SFT::Foundation
