#pragma once

#include <Reflection/Attribute.hpp>
#include <Reflection/ConstructorInfo.hpp>
#include <Reflection/EventInfo.hpp>
#include <Reflection/FieldInfo.hpp>
#include <Reflection/MethodInfo.hpp>
#include <Reflection/TypeId.hpp>

#include <Foundation/Foundation.hpp>

#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace SFT::Reflection {


    enum class TypeFlags : u32 {
        None = 0,
        /// Set on types reflected from a `*GameLogic`-convention package (the modding surface).
        GameLayer = 1u << 0u,
        /// Set on types reflected from engine-internal code, opted in explicitly rather than by
        /// convention. Informational only in v1 (nothing enforces it at access time).
        EngineInternal = 1u << 1u,
        TriviallyCopyable = 1u << 2u,
    };

    /// Combines the operands with bitwise OR.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr TypeFlags operator|(TypeFlags lhs, TypeFlags rhs) noexcept {
        return static_cast<TypeFlags>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
    }

    /// Reports whether flag is available.
    ///
    /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr bool has_flag(TypeFlags value, TypeFlags flag) noexcept {
        return (static_cast<u32>(value) & static_cast<u32>(flag)) != 0;
    }

    using TypeDefaultConstructFn = void (*)(void *destination, void *user_data) noexcept;
    using TypeCopyConstructFn = void (*)(void *destination, const void *source, void *user_data) noexcept;
    using TypeMoveConstructFn = void (*)(void *destination, void *source, void *user_data) noexcept;
    using TypeDestroyFn = void (*)(void *object, void *user_data) noexcept;


    /// Adjusts a `const void *` known to point at some derived object into a pointer to one of
    /// its secondary base subobjects. Always a `static_cast<const Base *>(static_cast<const
    /// Derived *>(derived))` baked in by the compiler at registration time (see
    /// `Detail::make_type_info`) — never manual offset arithmetic. This is deliberate: a
    /// hand-rolled `offsetof`-style computation for a non-standard-layout base is not portable and
    /// this codebase already avoids exactly that class of trick elsewhere (`Detail::member_offset`'s
    /// doc comment explains the same reasoning for why it needs a real, compiler-performed
    /// operation rather than raw pointer arithmetic). Letting the compiler perform the `static_cast`
    /// once, inside a function template instantiated for the exact `(Derived, Base)` pair, gets a
    /// correct adjustment for free — including virtual bases, multiple levels of inheritance, and
    /// whatever else the ABI actually does, none of which this package needs to know about.
    using BaseCastFn = const void *(*)(const void *derived) noexcept;

    /// One additional (non-primary) reflected base — see `TypeInfo::secondary_bases`. Unlike the
    /// single `base_type`, whose subobject this package assumes sits at offset `0` (true for the
    /// first base in every ABI this engine targets), a secondary base's subobject is generally at
    /// some other offset within the derived object, so reaching a field/method declared on it
    /// requires adjusting the object pointer first via `cast`.
    struct BaseInfo {
        TypeId type{};
        BaseCastFn cast = nullptr;
    };

    struct TypeInfo {
        TypeId key{};
        UString canonical_name;
        u32 schema_version = 1;
        usize size = 0;
        usize align = 0;
        TypeFlags flags = TypeFlags::None;
        /// Invalid (default-constructed) `TypeId` when this type has no reflected base. This is
        /// the *primary* base: its subobject is always at offset `0` within the derived object
        /// (matches how every existing `FieldInfo::offset`/`MethodInfo` reached through it was
        /// computed — directly against the derived type, per `Detail::member_offset`'s doc
        /// comment), so no pointer adjustment is ever needed to use a primary-base-inherited
        /// field/method against a derived object pointer. See `secondary_bases` for additional
        /// bases, which do not get this guarantee.
        TypeId base_type{};
        /// Additional reflected bases beyond the primary one — real multiple inheritance, or
        /// (the far more common case) several unrelated "interface" types a type implements. See
        /// `BaseInfo`'s doc comment for why these need pointer adjustment where `base_type` does
        /// not, and `TypeRegistry::find_field_adjusted`/`find_method_adjusted` for the lookup path
        /// that actually applies it.
        std::vector<BaseInfo> secondary_bases;
        void *user_data = nullptr;
        TypeDefaultConstructFn default_construct = nullptr;
        TypeCopyConstructFn copy_construct = nullptr;
        TypeMoveConstructFn move_construct = nullptr;
        TypeDestroyFn destroy = nullptr;
        std::vector<FieldInfo> fields;
        std::vector<MethodInfo> methods;
        std::vector<EventInfo> events;
        /// Static data members (`SFT_REFLECT_STATIC_FIELD`) — see `FieldFlags::Static`. Searched
        /// together with `fields` by `find_field`, so callers don't need to know ahead of time
        /// whether a named field is static; check `has_flag(field->flags, FieldFlags::Static)` to
        /// tell, and dispatch to `copy_static_field_out`/`_in` instead of `copy_field_out`/`_in`.
        std::vector<FieldInfo> static_fields;
        /// Static member functions (`SFT_REFLECT_STATIC_METHOD`) — see `MethodInfo::is_static`.
        /// Searched together with `methods` by `find_method`, so overrides/hooks
        /// (`TypeRegistry::set_method_override`, `add_method_before_hook`/`_after_hook`) work on
        /// static methods with no separate API. Check `method->is_static` and dispatch to
        /// `invoke_static_method` instead of `invoke_method`.
        std::vector<MethodInfo> static_methods;
        /// Parameterized constructors (`SFT_REFLECT_CONSTRUCTOR`). Unlike fields/methods/events,
        /// searched by signature (`find_constructor`), not by name — constructors have none.
        std::vector<ConstructorInfo> constructors;
        /// Arbitrary tooling/mod-facing metadata for the type itself. See `find_attribute`.
        std::vector<Attribute> attributes;

        /// Finds the requested field by key, searching both `fields` and `static_fields`.
        /// Declared-only (does not walk `base_type`); see `TypeRegistry::find_field` for the
        /// inheritance-aware equivalent.
        ///
        /// @return Returns a pointer to the requested field, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const FieldInfo *find_field(TypeId field_key) const noexcept;
        /// Finds the requested field by name. Declared-only; see `TypeRegistry::find_field`.
        ///
        /// @return Returns a pointer to the requested field, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const FieldInfo *find_field(std::string_view field_name) const noexcept;
        /// Finds the requested method by key, searching both `methods` and `static_methods`.
        /// Declared-only; see `TypeRegistry::find_method`.
        ///
        /// @return Returns a pointer to the requested method, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const MethodInfo *find_method(TypeId method_key) const noexcept;
        /// Finds the requested method by key, with mutable access (needed to install/clear
        /// overrides through `MethodInfo::override_fn`). Declared-only.
        ///
        /// @return Returns a pointer to the requested method, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] MethodInfo *find_method(TypeId method_key) noexcept;
        /// Finds the requested method by name. Declared-only; see `TypeRegistry::find_method`.
        ///
        /// When more than one method shares `method_name` (an overload set — see
        /// `Detail::compute_method_key`), this returns whichever was declared first; use the
        /// `param_types` overload below to disambiguate.
        ///
        /// @return Returns a pointer to the requested method, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const MethodInfo *find_method(std::string_view method_name) const noexcept;
        /// Finds the method named `method_name` whose parameter types match `param_types` exactly,
        /// in order — the disambiguating lookup for an overload set, the same way
        /// `find_constructor` matches constructors by signature rather than name. Declared-only.
        ///
        /// @return Returns a pointer to the requested method, or `nullptr` when no method with
        /// that name and exact signature is declared.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const MethodInfo *find_method(std::string_view method_name, std::span<const TypeId> param_types) const noexcept;
        /// Finds the requested event by key. Declared-only; see `TypeRegistry::find_event`.
        ///
        /// @return Returns a pointer to the requested event, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const EventInfo *find_event(TypeId event_key) const noexcept;
        /// Finds the requested event by name. Declared-only; see `TypeRegistry::find_event`.
        ///
        /// @return Returns a pointer to the requested event, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const EventInfo *find_event(std::string_view event_name) const noexcept;
        /// Finds a constructor whose parameter list matches `param_types` exactly, in order.
        ///
        /// @return Returns a pointer to the requested constructor, or `nullptr` when no
        /// constructor with that exact signature is declared.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const ConstructorInfo *find_constructor(std::span<const TypeId> param_types) const noexcept;
    };

    /// Default-constructs an instance of `type` into `destination` (which must be at least
    /// `type.size` bytes, aligned to `type.align`). The Java-reflection analog of
    /// `Class.newInstance()`.
    ///
    /// @return Returns `true` on success; `false` when `type` has no default constructor
    /// (e.g. it declares a non-defaulted constructor with required parameters).
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool default_construct_instance(const TypeInfo &type, void *destination) noexcept {
        if (type.default_construct == nullptr) {
            return false;
        }
        type.default_construct(destination, type.user_data);
        return true;
    }

    /// Copy-constructs an instance of `type` into `destination` from `source`.
    ///
    /// @return Returns `true` on success; `false` when `type` has no copy constructor.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool copy_construct_instance(const TypeInfo &type, void *destination, const void *source) noexcept {
        if (type.copy_construct == nullptr) {
            return false;
        }
        type.copy_construct(destination, source, type.user_data);
        return true;
    }

    /// Move-constructs an instance of `type` into `destination` from `source`. Never null once a
    /// type is registered — `Detail::make_type_info` requires nothrow-move-constructibility.
    ///
    /// @note This function does not throw exceptions.
    inline void move_construct_instance(const TypeInfo &type, void *destination, void *source) noexcept {
        type.move_construct(destination, source, type.user_data);
    }

    /// Destroys an instance of `type` in place. Never null once a type is registered.
    ///
    /// @note This function does not throw exceptions.
    inline void destroy_instance(const TypeInfo &type, void *object) noexcept {
        type.destroy(object, type.user_data);
    }

    /// Distinguishes what's being visited within a single ordered
    /// `TypeTraits<T>::for_each_member` walk (see `Macros.hpp`). `Constructor` (like `Event`) has
    /// no backing member pointer — its `Member` template argument is always `nullptr`, and its
    /// parameter types are carried through the same type-argument pack `Event` uses.
    enum class MemberKind {
        Field,
        Method,
        Event,
        StaticField,
        StaticMethod,
        Constructor,
    };

    /// Specialized per reflected type via `SFT_REFLECT_TYPE`/`SFT_REFLECT_TYPE_VERSIONED`
    /// (`Macros.hpp`). The primary template intentionally declares only `name`, so
    /// `Detail::type_name<T>()`'s `static_assert` is the diagnostic an unreflected `T` hits first.
    template <class T>
    struct TypeTraits {
        static constexpr std::string_view name{};
    };


} // namespace SFT::Reflection
