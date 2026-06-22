#pragma once

#include "Concepts.hpp"
#include "Wide.hpp"

#include <concepts>

// Public numeric concepts for templates that should accept Sturdy's built-in scalar aliases and
// wide numeric types uniformly. bool/b8 are intentionally excluded from integer/number concepts.

namespace SFT::Foundation {

    namespace Detail {

        template <class T>
        concept WideUnsignedInteger = std::same_as<Unqualified<T>, u256>;

        template <class T>
        concept WideSignedInteger = std::same_as<Unqualified<T>, i256>;

        template <class T>
        concept WideFloat = std::same_as<Unqualified<T>, f128> || std::same_as<Unqualified<T>, f256>;

    } // namespace Detail

    template <class T>
    concept UnsignedInteger = !Boolean<T> &&
                              (std::unsigned_integral<Detail::Unqualified<T>> ||
                               std::same_as<Detail::Unqualified<T>, u128> || Detail::WideUnsignedInteger<T>);

    template <class T>
    concept Integer = !Boolean<T> &&
                      (std::integral<Detail::Unqualified<T>> || std::same_as<Detail::Unqualified<T>, i128> ||
                       std::same_as<Detail::Unqualified<T>, u128> || Detail::WideSignedInteger<T> ||
                       Detail::WideUnsignedInteger<T>);

    template <class T>
    concept Float = std::floating_point<Detail::Unqualified<T>> || Detail::WideFloat<T>;

    template <class T>
    concept Number = Integer<T> || Float<T>;

} // namespace SFT::Foundation
