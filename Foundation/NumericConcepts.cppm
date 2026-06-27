module;

#include <concepts>

export module Sturdy.Foundation:NumericConcepts;

import :Concepts;
import :Wide;

using std::floating_point;
using std::integral;
using std::same_as;
using std::unsigned_integral;

export namespace SFT::Foundation {

    namespace Detail {

        template <class T>
        concept WideUnsignedInteger = same_as<Detail::Unqualified<T>, u256>;

        template <class T>
        concept WideSignedInteger = same_as<Detail::Unqualified<T>, i256>;

        template <class T>
        concept WideFloat = same_as<Detail::Unqualified<T>, f128> || same_as<Detail::Unqualified<T>, f256>;

    } // namespace Detail

    template <class T>
    concept UnsignedInteger = !Boolean<T> &&
                              (unsigned_integral<Detail::Unqualified<T>> ||
                               same_as<Detail::Unqualified<T>, u128> || Detail::WideUnsignedInteger<T>);

    template <class T>
    concept Integer = !Boolean<T> &&
                      (integral<Detail::Unqualified<T>> || same_as<Detail::Unqualified<T>, i128> ||
                       same_as<Detail::Unqualified<T>, u128> || Detail::WideSignedInteger<T> ||
                       Detail::WideUnsignedInteger<T>);

    template <class T>
    concept Float = floating_point<Detail::Unqualified<T>> || Detail::WideFloat<T>;

    template <class T>
    concept Number = Integer<T> || Float<T>;

} // namespace SFT::Foundation
