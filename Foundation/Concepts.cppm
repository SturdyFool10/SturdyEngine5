module;

#include <compare>
#include <concepts>
#include <functional>
#include <type_traits>

export module Sturdy.Foundation:Concepts;

import :Types;

using std::assignable_from;
using std::constructible_from;
using std::convertible_to;
using std::copy_constructible;
using std::copyable;
using std::default_initializable;
using std::destructible;
using std::equality_comparable;
using std::equality_comparable_with;
using std::hash;
using std::is_enum_v;
using std::is_object_v;
using std::movable;
using std::move_constructible;
using std::partial_ordering;
using std::predicate;
using std::regular;
using std::relation;
using std::remove_cvref_t;
using std::same_as;
using std::semiregular;
using std::swappable;
using std::swappable_with;
using std::three_way_comparable;
using std::three_way_comparable_with;
using std::totally_ordered;
using std::totally_ordered_with;

export namespace SFT::Foundation {

    namespace Detail {

        template <class T>
        using Unqualified = remove_cvref_t<T>;

    } // namespace Detail

    template <class T>
    concept Boolean = same_as<Detail::Unqualified<T>, bool> || same_as<Detail::Unqualified<T>, b8>;

    template <class T>
    concept Object = is_object_v<Detail::Unqualified<T>>;

    template <class T>
    concept Enum = is_enum_v<Detail::Unqualified<T>>;

    template <class T>
    concept Destructible = destructible<Detail::Unqualified<T>>;

    template <class T, class... Args>
    concept ConstructibleFrom = constructible_from<Detail::Unqualified<T>, Args...>;

    template <class T>
    concept DefaultConstructible = default_initializable<Detail::Unqualified<T>>;

    template <class T>
    concept MoveConstructible = move_constructible<Detail::Unqualified<T>>;

    template <class T>
    concept CopyConstructible = copy_constructible<Detail::Unqualified<T>>;

    template <class T>
    concept Movable = movable<Detail::Unqualified<T>>;

    template <class T>
    concept Copyable = copyable<Detail::Unqualified<T>>;

    template <class T, class U>
    concept AssignableFrom = assignable_from<Detail::Unqualified<T> &, U>;

    template <class T>
    concept Swappable = swappable<Detail::Unqualified<T>>;

    template <class T, class U>
    concept SwappableWith = swappable_with<Detail::Unqualified<T> &, Detail::Unqualified<U> &>;

    template <class T>
    concept Semiregular = semiregular<Detail::Unqualified<T>>;

    template <class T>
    concept Regular = regular<Detail::Unqualified<T>>;

    template <class T>
    concept EqualityComparable = equality_comparable<Detail::Unqualified<T>>;

    template <class T, class U>
    concept EqualityComparableWith = equality_comparable_with<Detail::Unqualified<T>, Detail::Unqualified<U>>;

    template <class T>
    concept TotallyOrdered = totally_ordered<Detail::Unqualified<T>>;

    template <class T, class U>
    concept TotallyOrderedWith = totally_ordered_with<Detail::Unqualified<T>, Detail::Unqualified<U>>;

    template <class T, class Category = partial_ordering>
    concept ThreeWayComparable = three_way_comparable<Detail::Unqualified<T>, Category>;

    template <class T, class U, class Category = partial_ordering>
    concept ThreeWayComparableWith = three_way_comparable_with<Detail::Unqualified<T>, Detail::Unqualified<U>, Category>;

    template <class T>
    concept Hashable = requires(const Detail::Unqualified<T> &value) {
        { hash<Detail::Unqualified<T>>{}(value) } -> convertible_to<usize>;
    };

} // namespace SFT::Foundation
