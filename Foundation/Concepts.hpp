#pragma once

#include "Types.hpp"

#include <compare>
#include <concepts>
#include <cstddef>
#include <functional>
#include <type_traits>

// Public generic concepts for readable template requirements.
//
// Curation rule: every concept here earns its place by doing something a single standard concept
// does not — normalizing cv/ref qualifiers (so `const T&` and `T` satisfy the same object-level
// requirement), composing several requirements, folding in Sturdy's own types, or stating a brand
// new requirement. A concept that would be nothing but a rename of one std concept is intentionally
// absent; reach for the std concept directly (std::invocable, std::predicate, std::ranges::range,
// std::relation, ...). Negating or otherwise transforming a std concept is fair game.

namespace SFT::Foundation {

    namespace Detail {

        template <class T>
        using Unqualified = std::remove_cvref_t<T>;

    } // namespace Detail

    template <class T>
    concept Boolean = std::same_as<Detail::Unqualified<T>, bool> || std::same_as<Detail::Unqualified<T>, b8>;

    template <class T>
    concept Object = std::is_object_v<Detail::Unqualified<T>>;

    template <class T>
    concept Enum = std::is_enum_v<Detail::Unqualified<T>>;

    template <class T>
    concept Destructible = std::destructible<Detail::Unqualified<T>>;

    template <class T, class... Args>
    concept ConstructibleFrom = std::constructible_from<Detail::Unqualified<T>, Args...>;

    template <class T>
    concept DefaultConstructible = std::default_initializable<Detail::Unqualified<T>>;

    template <class T>
    concept MoveConstructible = std::move_constructible<Detail::Unqualified<T>>;

    template <class T>
    concept CopyConstructible = std::copy_constructible<Detail::Unqualified<T>>;

    template <class T>
    concept Movable = std::movable<Detail::Unqualified<T>>;

    template <class T>
    concept Copyable = std::copyable<Detail::Unqualified<T>>;

    template <class T, class U>
    concept AssignableFrom = std::assignable_from<Detail::Unqualified<T> &, U>;

    template <class T>
    concept Swappable = std::swappable<Detail::Unqualified<T>>;

    template <class T, class U>
    concept SwappableWith = std::swappable_with<Detail::Unqualified<T> &, Detail::Unqualified<U> &>;

    template <class T>
    concept Semiregular = std::semiregular<Detail::Unqualified<T>>;

    template <class T>
    concept Regular = std::regular<Detail::Unqualified<T>>;

    template <class T>
    concept EqualityComparable = std::equality_comparable<Detail::Unqualified<T>>;

    template <class T, class U>
    concept EqualityComparableWith = std::equality_comparable_with<Detail::Unqualified<T>, Detail::Unqualified<U>>;

    template <class T>
    concept TotallyOrdered = std::totally_ordered<Detail::Unqualified<T>>;

    template <class T, class U>
    concept TotallyOrderedWith = std::totally_ordered_with<Detail::Unqualified<T>, Detail::Unqualified<U>>;

    template <class T, class Category = std::partial_ordering>
    concept ThreeWayComparable = std::three_way_comparable<Detail::Unqualified<T>, Category>;

    template <class T, class U, class Category = std::partial_ordering>
    concept ThreeWayComparableWith = std::three_way_comparable_with<Detail::Unqualified<T>, Detail::Unqualified<U>, Category>;

    template <class T>
    concept Hashable = requires(const Detail::Unqualified<T> &value) {
        { std::hash<Detail::Unqualified<T>>{}(value) } -> std::convertible_to<std::size_t>;
    };

} // namespace SFT::Foundation
