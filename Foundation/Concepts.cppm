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

    // Engine-flavored concept vocabulary — thin `CamelCase` wrappers over the `std` concepts, with one
    // key difference: every one strips cv/ref qualifiers first (via `Detail::Unqualified`), so
    // `Copyable<const T&>` asks about `T` rather than tripping over the reference. Use these in
    // `requires` clauses and constrained templates throughout the engine.

    namespace Detail {

        // Strips `const`/`volatile`/`&`/`&&` — the normalization applied before every concept below, so
        // qualifiers on a template argument never change the answer.
        template <class T>
        using Unqualified = remove_cvref_t<T>;

    } // namespace Detail

    // `bool` or the engine's 1-byte `b8` — used to exclude booleans from the numeric concepts.
    template <class T>
    concept Boolean = same_as<Detail::Unqualified<T>, bool> || same_as<Detail::Unqualified<T>, b8>;

    // Any object type (has size/storage) — excludes references, functions, and `void`.
    template <class T>
    concept Object = is_object_v<Detail::Unqualified<T>>;

    // Any enumeration type (scoped or unscoped).
    template <class T>
    concept Enum = is_enum_v<Detail::Unqualified<T>>;

    // Can be destroyed without throwing.
    template <class T>
    concept Destructible = destructible<Detail::Unqualified<T>>;

    // Constructible from the given `Args...`.
    template <class T, class... Args>
    concept ConstructibleFrom = constructible_from<Detail::Unqualified<T>, Args...>;

    // Default-constructible (value-initializable).
    template <class T>
    concept DefaultConstructible = default_initializable<Detail::Unqualified<T>>;

    // Movable-from-a-prvalue construction.
    template <class T>
    concept MoveConstructible = move_constructible<Detail::Unqualified<T>>;

    // Copy-constructible.
    template <class T>
    concept CopyConstructible = copy_constructible<Detail::Unqualified<T>>;

    // Move-constructible, move-assignable, and swappable — i.e. has value-like move semantics.
    template <class T>
    concept Movable = movable<Detail::Unqualified<T>>;

    // `Movable` plus copy construction/assignment — full value-type copy semantics.
    template <class T>
    concept Copyable = copyable<Detail::Unqualified<T>>;

    // An lvalue `T` can be assigned from a `U`.
    template <class T, class U>
    concept AssignableFrom = assignable_from<Detail::Unqualified<T> &, U>;

    // `swap(a, b)` is valid for two `T`s.
    template <class T>
    concept Swappable = swappable<Detail::Unqualified<T>>;

    // A `T` and a `U` can be swapped with each other.
    template <class T, class U>
    concept SwappableWith = swappable_with<Detail::Unqualified<T> &, Detail::Unqualified<U> &>;

    // Copyable + default-constructible (a "regular-ish" value type, without requiring `==`).
    template <class T>
    concept Semiregular = semiregular<Detail::Unqualified<T>>;

    // `Semiregular` + `EqualityComparable` — the gold standard well-behaved value type.
    template <class T>
    concept Regular = regular<Detail::Unqualified<T>>;

    // Supports `==` / `!=` with itself.
    template <class T>
    concept EqualityComparable = equality_comparable<Detail::Unqualified<T>>;

    // A `T` and a `U` are `==`-comparable with each other.
    template <class T, class U>
    concept EqualityComparableWith = equality_comparable_with<Detail::Unqualified<T>, Detail::Unqualified<U>>;

    // Supports the full set of relational operators (`< <= > >=` and `==`) as a total order.
    template <class T>
    concept TotallyOrdered = totally_ordered<Detail::Unqualified<T>>;

    // A `T` and a `U` are totally ordered with each other.
    template <class T, class U>
    concept TotallyOrderedWith = totally_ordered_with<Detail::Unqualified<T>, Detail::Unqualified<U>>;

    // Has `operator<=>` yielding at least `Category` (defaults to `partial_ordering`).
    template <class T, class Category = partial_ordering>
    concept ThreeWayComparable = three_way_comparable<Detail::Unqualified<T>, Category>;

    // A `T` and a `U` are `<=>`-comparable with each other, yielding at least `Category`.
    template <class T, class U, class Category = partial_ordering>
    concept ThreeWayComparableWith = three_way_comparable_with<Detail::Unqualified<T>, Detail::Unqualified<U>, Category>;

    // Has a `std::hash` specialization whose result converts to `usize` — usable as a hash-map key.
    template <class T>
    concept Hashable = requires(const Detail::Unqualified<T> &value) {
        { hash<Detail::Unqualified<T>>{}(value) } -> convertible_to<usize>;
    };

} // namespace SFT::Foundation
