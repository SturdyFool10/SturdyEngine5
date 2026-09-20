#pragma once

#include <Reflection/FixedString.hpp>
#include <Reflection/Macros.hpp>
#include <Reflection/StaticTypeId.hpp>
#include <Reflection/TypeId.hpp>

#include <Foundation/Foundation.hpp>

#include <string_view>
#include <type_traits>
#include <utility>

/// Compile-time-only "property" support: a getter/setter pair exposed as a single reflected
/// value, without ever making the backing field itself public or reflected.
///
/// This is deliberately a standalone mechanism, not something threaded through
/// `SFT_REFLECT_FIELD`/`TypeTraits<T>::for_each_member` (`StaticReflection.hpp`,
/// `Macros.hpp`). A field has exactly one member pointer of one consistent type, which is what
/// lets `for_each_member` enumerate a homogeneous sequence of them generically and lets
/// `StaticFieldInfo` describe one uniformly. A property has *two* independent, differently-typed
/// pointers-to-member-function (or, for a read-only property, one) — there is no single "member
/// pointer" value that could be stored in a `for_each_member`-driven table entry the way a
/// field's can. Nor does a property need to be discoverable that way to be useful: exactly like
/// `get<T, &T::field>`/`set<T, &T::field>` in `StaticReflection.hpp`, a property is named
/// explicitly at the call site — `property<Player, &Player::get_health, &Player::set_health>()`
/// — by whoever already knows which getter/setter pair they mean. Generic enumeration
/// ("what properties does `Player` have") is a reasonable future extension, but it would need its
/// own opt-in registration step (the caller listing which method pairs count as properties), not
/// a change to how plain data fields are declared.
namespace SFT::Reflection {

    /// A compile-time-only description of one getter/setter pair, exposed as a single reflected
    /// property. `Getter` is a pointer to a zero-argument, non-void-returning member function;
    /// `Setter`, when not `nullptr`, is a pointer to a one-argument member function taking the
    /// property's value type. `Name` is an optional compile-time label (see `Detail::FixedString`)
    /// — purely descriptive, never used for lookup here (there is no name-based property table;
    /// see this header's top-of-file doc comment for why).
    ///
    /// Stateless (an empty type) and fully `consteval`/`constexpr`, mirroring `StaticFieldInfo`/
    /// `get`/`set` in `StaticReflection.hpp`: a `Player.health` property that is only ever used
    /// through this API costs nothing beyond what a hand-written `player.get_health()`/
    /// `player.set_health(v)` call pair would have.
    template <class T, auto Getter, auto Setter = nullptr, Detail::FixedString Name = "">
    struct StaticProperty {
        using GetterTraits = Detail::MemberFunctionTraits<decltype(Getter)>;

        /// Returns the property's compile-time-known name, as supplied to `property<...>()`.
        [[nodiscard]] static consteval std::string_view name() noexcept {
            return Name.view();
        }

        /// Returns the property's value type identity, derived from `Getter`'s return type.
        /// Reference/pointer/cv qualifiers on that return type are stripped first (a getter
        /// returning `const int&` and one returning `int` describe the same property value
        /// type) — the same `remove_cv_t<remove_pointer_t<remove_reference_t<T>>>` pattern
        /// `StaticTypeId.hpp`'s own `type_ref<T>()` uses for exactly this reason.
        [[nodiscard]] static consteval TypeId type() noexcept {
            using Return = typename GetterTraits::Return;
            using Bare = std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<Return>>>;
            return type_id<Bare>();
        }

        /// Reports whether this property was constructed with a setter (`Setter != nullptr`), a
        /// compile-time check on the `Setter` non-type template parameter itself — a read-only
        /// property (default `Setter`) reports `false` here, and its `set()` overload is absent
        /// from overload resolution entirely (see `set()`'s doc comment) rather than merely
        /// throwing or asserting if called.
        [[nodiscard]] static consteval bool has_setter() noexcept {
            return !std::is_null_pointer_v<decltype(Setter)>;
        }

        /// Reads the property off `object` — exactly `(object.*Getter)()`, nothing else.
        ///
        /// @return Returns whatever `Getter` returns.
        [[nodiscard]] static constexpr decltype(auto) get(const T &object) noexcept(noexcept((object.*Getter)())) {
            return (object.*Getter)();
        }

        /// Writes `value` into the property on `object` — exactly `(object.*Setter)(value)`.
        ///
        /// Only a viable overload when `has_setter()` is `true`: the `requires` clause (rather
        /// than an internal `static_assert`) is what lets a read-only `StaticProperty` (default
        /// `Setter = nullptr`) simply not offer `set()` at all — a call site attempting to write
        /// through a read-only property gets an ordinary "no matching function" error, not a
        /// confusing instantiation failure deep inside a body that dereferences a null member
        /// pointer.
        template <class Value>
        static constexpr void set(T &object, Value &&value) noexcept(noexcept((object.*Setter)(std::forward<Value>(value))))
            requires(has_setter())
        {
            (object.*Setter)(std::forward<Value>(value));
        }
    };

    /// Returns a compile-time-only view of a `Getter`/`Setter` pair as a single named property.
    /// `T` need not be `SFT_REFLECT_TYPE`-annotated at all — unlike `reflect<T>()`, this never
    /// touches `TypeTraits<T>`/`for_each_member`, it only ever needs the two member-function
    /// pointers supplied directly as template arguments (see this header's top-of-file doc
    /// comment for why properties are not enumerated through `for_each_member` the way fields
    /// are).
    ///
    /// @return Returns a stateless `StaticProperty<T, Getter, Setter, Name>`, usable directly in a
    /// `static_assert`, e.g.
    /// `static_assert(property<Player, &Player::get_health, &Player::set_health>().type() ==
    /// type_id<int>());`. Omit `Setter` for a read-only property; supply `Name` (a string literal)
    /// for a labeled one, e.g. `property<Player, &Player::get_health, &Player::set_health,
    /// "health">()`.
    template <class T, auto Getter, auto Setter = nullptr, Detail::FixedString Name = "">
    [[nodiscard]] consteval StaticProperty<T, Getter, Setter, Name> property() noexcept {
        return StaticProperty<T, Getter, Setter, Name>{};
    }

} // namespace SFT::Reflection
