#pragma once

#include <Reflection/Attribute.hpp>
#include <Reflection/FixedString.hpp>
#include <Reflection/Macros.hpp>
#include <Reflection/StaticTypeId.hpp>
#include <Reflection/StructuralTypeId.hpp>
#include <Reflection/TypeId.hpp>
#include <Reflection/TypeInfo.hpp>
#include <Reflection/TypeRef.hpp>

#include <Foundation/Foundation.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

/// The compile-time-first reflection layer (`StaticTypeInfo<T>`/`StaticFieldInfo`/`reflect<T>()`/
/// `get`/`set`/`invoke`/`construct`), sitting *alongside* the runtime `TypeInfo`/`FieldInfo`/
/// `MethodInfo`/`TypeRegistry` machinery in `TypeInfo.hpp`/`Macros.hpp`/`TypeRegistry.hpp` — it
/// does not replace it.
///
/// Everything in this file is `consteval`/`constexpr`: it never touches `TypeRegistry`, never
/// allocates, and never materializes a runtime `TypeInfo`. A game that only ever calls
/// `get<Player, &Player::health>(player)` or `static_assert(reflect<Player>().has_field("health"))`
/// pays nothing beyond what hand-written `player.health` would have cost — the "unmodded game"
/// execution model this package's design doc calls for. Runtime modding (overrides, hooks, dynamic
/// types, the `TypeRegistry` singleton) remains exactly where it already lived; this file adds a
/// zero-cost front door for the fully-compile-time-known case, it does not touch that machinery.
namespace SFT::Reflection {


    /// How many attributes a `StaticFieldInfo`/`StaticMethodInfo` records inline. Same rationale
    /// as `max_static_parameter_count`: the descriptor table must be homogeneous, so attributes
    /// live in a fixed-capacity array, and overflowing it is a `static_assert` rather than silent
    /// truncation.
    inline constexpr usize max_static_attribute_count = 8;

    /// A compile-time-only description of one reflected data member — the `StaticFieldInfo` the
    /// design doc calls for. Deliberately contains only compile-time-friendly, `consteval`-
    /// constructible data (no `UString`, no `std::vector`, no member pointer — member pointers of
    /// different fields have different C++ types, so they cannot live in a homogeneous
    /// `std::array<StaticFieldInfo, N>` the way `name`/`type`/`size`/`align` can). For a
    /// zero-indirection, member-pointer-based accessor to a *specific, compile-time-named* field,
    /// use the free functions `get<T, &T::field>`/`set<T, &T::field>` below instead — those take
    /// the member pointer directly from the caller and compile to a plain `object.*Member`, no
    /// lookup involved. `StaticFieldInfo` exists for the complementary case: introspection
    /// (`has_field`, `field_count`, iterating `StaticTypeInfo<T>::fields()`) and name-based
    /// compile-time lookup (`StaticTypeInfo<T>::field<"name">()`), all resolved during
    /// compilation with zero runtime trace.
    struct StaticFieldInfo {
        std::string_view name;
        TypeRef field_type{};
        usize size = 0;
        usize align = 0;
        usize attribute_count = 0;
        std::array<StaticAttribute, max_static_attribute_count> attribute_storage{};

        /// Returns just this field's declared attributes (see `SFT_ATTR_BOOL` and friends),
        /// without the unused tail of `attribute_storage`.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr std::span<const StaticAttribute> attributes() const noexcept {
            return std::span<const StaticAttribute>{attribute_storage.data(), attribute_count};
        }

        /// Finds a declared attribute by name, or `nullptr` when the field has none such — the
        /// compile-time counterpart of `find_attribute(FieldInfo, ...)`.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr const StaticAttribute *find_attribute(std::string_view attribute_name) const noexcept {
            for (const StaticAttribute &attribute : attributes()) {
                if (attribute.name == attribute_name) {
                    return &attribute;
                }
            }
            return nullptr;
        }

        /// Reports whether this field declares an attribute named `attribute_name`.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr bool has_attribute(std::string_view attribute_name) const noexcept {
            return find_attribute(attribute_name) != nullptr;
        }

        /// Returns the field's base type identity (qualifiers stripped — a plain field
        /// declaration has none in the overwhelming common case). See `type_ref()` for the full,
        /// qualifier-preserving reference.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr TypeId type() const noexcept {
            return field_type.base;
        }

        /// Returns the field's full type reference, including any cv/ref/pointer qualifiers the
        /// declared field type carried (see `TypeRef`).
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr TypeRef type_ref() const noexcept {
            return field_type;
        }

        /// Compares the operands for equality.
        ///
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(const StaticFieldInfo &, const StaticFieldInfo &) noexcept = default;
    };

    /// How many parameters a `StaticMethodInfo`/`StaticConstructorInfo` records inline.
    ///
    /// A compile-time descriptor table has to be homogeneous (`std::array<StaticMethodInfo, N>`),
    /// but methods have differing arities — so parameters live in a fixed-capacity inline array
    /// rather than a variable-length one. The cap is a pragmatic bound, not a fundamental limit:
    /// exceeding it is a `static_assert`, never silent truncation, and raising it costs only
    /// compile-time table size (these descriptors never exist at runtime).
    inline constexpr usize max_static_parameter_count = 12;

    /// A compile-time-only description of one reflected member function — the `StaticMethodInfo`
    /// the design doc calls for. Like `StaticFieldInfo`, it holds no member pointer (pointers to
    /// different methods have different types and cannot share a homogeneous array); for a direct,
    /// zero-indirection call use `invoke<T, &T::method>(object, args...)` below, which takes the
    /// member pointer straight from the caller.
    ///
    /// Unlike the runtime `MethodInfo`, this preserves the *full* signature: parameter and return
    /// types keep their cv/ref/pointer qualifiers (`MethodInfo::param_types` erases them through
    /// `remove_cvref_t`), and the implicit object parameter's own `const`/`volatile`/ref
    /// qualification is recorded too — enough to reconstruct `int aim(const Target &) const
    /// noexcept` exactly, which is what a script binding, RPC stub, or generated header needs.
    struct StaticMethodInfo {
        std::string_view name;
        TypeRef return_type{};
        usize arity = 0;
        std::array<TypeRef, max_static_parameter_count> parameters{};
        /// `true` for `SFT_REFLECT_STATIC_METHOD` members, which have no receiver.
        bool is_static = false;
        bool is_const = false;
        bool is_volatile = false;
        bool is_noexcept = false;
        Detail::MethodRefQualifier ref_qualifier = Detail::MethodRefQualifier::None;
        usize attribute_count = 0;
        std::array<StaticAttribute, max_static_attribute_count> attribute_storage{};

        /// Returns just this method's declared attributes.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr std::span<const StaticAttribute> attributes() const noexcept {
            return std::span<const StaticAttribute>{attribute_storage.data(), attribute_count};
        }

        /// Finds a declared attribute by name, or `nullptr` when absent.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr const StaticAttribute *find_attribute(std::string_view attribute_name) const noexcept {
            for (const StaticAttribute &attribute : attributes()) {
                if (attribute.name == attribute_name) {
                    return &attribute;
                }
            }
            return nullptr;
        }

        /// Returns just this method's declared parameters, without the unused tail of
        /// `parameters`.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr std::span<const TypeRef> parameter_types() const noexcept {
            return std::span<const TypeRef>{parameters.data(), arity};
        }

        /// Returns the return type's base identity, qualifiers stripped.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr TypeId returns() const noexcept {
            return return_type.base;
        }

        /// Compares the operands for equality.
        ///
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(const StaticMethodInfo &, const StaticMethodInfo &) noexcept = default;

        /// The compile-time equivalent of `Detail::compute_method_key` (`Macros.hpp`) — same
        /// identity, derived the same way (`name` plus each parameter's *unqualified* `TypeId`,
        /// composed into one string and hashed), just evaluated at translation time instead of at
        /// `SFT_REFLECT_METHOD`/`_OVERLOAD` registration time.
        ///
        /// This has to reproduce `compute_method_key`'s string format byte-for-byte
        /// (`"<name>#<high>:<low>#<high>:<low>#..."`, one `#high:low` segment per parameter, in
        /// declaration order) rather than hash some other, equally-valid encoding of the same
        /// inputs — the whole point of a "key" is that two independently-computed ones for the
        /// same signature must agree, and `TypeId::from_name` has no algebraic structure that
        /// would make two different encodings collapse to the same hash. `compute_method_key`
        /// itself cannot be called directly from here: it builds its string with
        /// `std::to_string`, which is not usable during constant evaluation (see
        /// `Detail::append_structural_extent`'s doc comment in `StructuralTypeId.hpp` for the same
        /// constraint hit there) — so this reuses that file's `append_structural_extent` digit
        /// writer instead of `std::to_string`, everything else identical.
        ///
        /// Only `parameters[i].base` (the unqualified identity) feeds the key, never
        /// `parameters[i].qualifiers` — matching `compute_method_key`, which hashes over the
        /// runtime `MethodInfo::param_types` (themselves `remove_cvref_t`-erased). Two overloads
        /// differing only by parameter qualifiers (e.g. `void f(Item)` vs `void f(Item &)`, were
        /// such a pair ever declared) would therefore collide on this key exactly as they would
        /// on the runtime one — this mirrors an existing runtime limitation, not a new one.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr TypeId key() const noexcept {
            std::string signature{name};
            for (usize i = 0; i < arity; ++i) {
                signature.push_back('#');
                Detail::append_structural_extent(signature, static_cast<usize>(parameters[i].base.hash.high));
                signature.push_back(':');
                Detail::append_structural_extent(signature, static_cast<usize>(parameters[i].base.hash.low));
            }
            return TypeId::from_name(signature);
        }
    };

    /// A compile-time-only description of one reflected parameterized constructor — the
    /// `StaticConstructorInfo` the design doc calls for. Constructors have no name in C++, so
    /// (exactly like the runtime `ConstructorInfo`) they are identified by signature alone.
    struct StaticConstructorInfo {
        usize arity = 0;
        std::array<TypeRef, max_static_parameter_count> parameters{};

        /// Returns just this constructor's declared parameters.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr std::span<const TypeRef> parameter_types() const noexcept {
            return std::span<const TypeRef>{parameters.data(), arity};
        }

        /// Compares the operands for equality.
        ///
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(const StaticConstructorInfo &, const StaticConstructorInfo &) noexcept = default;
    };

    /// A compile-time-only description of one declared event (`SFT_REFLECT_EVENT`) — the
    /// `StaticEventInfo` the design doc calls for, closing the one remaining gap where every
    /// other member kind (field/method/constructor/enum) already had a static counterpart but
    /// events did not. Like `StaticConstructorInfo`, an event has no backing member pointer
    /// (`SFT_REFLECT_EVENT` supplies its parameter types directly as a template argument pack),
    /// so — unlike `StaticFieldInfo`/`StaticMethodInfo` — this carries no attributes either:
    /// `SFT_REFLECT_EVENT` has no trailing `SFT_ATTR_*` argument slot today (see
    /// `Detail::build_event_info`'s doc comment in `Macros.hpp`), so there is nothing for a
    /// `StaticEventInfo` to record there without inventing API surface the runtime side doesn't
    /// use either.
    struct StaticEventInfo {
        std::string_view name;
        usize arity = 0;
        std::array<TypeRef, max_static_parameter_count> parameters{};

        /// Returns just this event's declared parameters, without the unused tail of
        /// `parameters`.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr std::span<const TypeRef> parameter_types() const noexcept {
            return std::span<const TypeRef>{parameters.data(), arity};
        }

        /// Compares the operands for equality.
        ///
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(const StaticEventInfo &, const StaticEventInfo &) noexcept = default;
    };

    namespace Detail {

        /// Reads each `SFT_ATTR_*` factory's compile-time form into a fixed-capacity array.
        ///
        /// `factories` are the trailing arguments `for_each_member` forwards; `static_value()` is
        /// `consteval`, so this never touches the non-`constexpr` runtime `Attribute` construction
        /// path (`operator()`) that the same factories also offer.
        template <class... Factories>
        [[nodiscard]] consteval std::array<StaticAttribute, max_static_attribute_count> static_attribute_array() noexcept {
            static_assert(sizeof...(Factories) <= max_static_attribute_count,
                          "This member declares more attributes than StaticFieldInfo/StaticMethodInfo "
                          "record inline; raise SFT::Reflection::max_static_attribute_count.");
            std::array<StaticAttribute, max_static_attribute_count> result{};
            usize index = 0;
            ((result[index++] = std::remove_cvref_t<Factories>::static_value()), ...);
            return result;
        }

        /// Copies `Traits`' parameter `TypeRef`s into a fixed-capacity array, rejecting a signature
        /// too wide to record rather than silently dropping its tail.
        template <class Traits>
        [[nodiscard]] consteval std::array<TypeRef, max_static_parameter_count> static_parameter_array() noexcept {
            static_assert(Traits::arity <= max_static_parameter_count,
                          "This method/constructor has more parameters than StaticMethodInfo records "
                          "inline; raise SFT::Reflection::max_static_parameter_count.");
            std::array<TypeRef, max_static_parameter_count> result{};
            const auto declared = Traits::param_type_refs();
            for (usize i = 0; i < Traits::arity; ++i) {
                result[i] = declared[i];
            }
            return result;
        }

        /// The `Args...`-pack equivalent of `static_parameter_array`, for constructors/events —
        /// which, having no function pointer to inspect, supply their parameter types directly as
        /// a template argument pack instead of through `MemberFunctionTraits`.
        template <class... Args>
        [[nodiscard]] consteval std::array<TypeRef, max_static_parameter_count> static_parameter_array_from_pack() noexcept {
            static_assert(sizeof...(Args) <= max_static_parameter_count,
                          "This constructor has more parameters than StaticConstructorInfo records "
                          "inline; raise SFT::Reflection::max_static_parameter_count.");
            std::array<TypeRef, max_static_parameter_count> result{};
            usize index = 0;
            ((result[index++] = structural_type_ref<Args>()), ...);
            return result;
        }

        /// Counts how many `MemberKind::Field` entries `TypeTraits<T>::for_each_member` declares —
        /// the first of the two `for_each_member` passes `StaticTypeInfo<T>::fields()` needs
        /// (`std::array` requires its size up front, so the count must be known before the second
        /// pass can fill anything in).
        template <class T>
        [[nodiscard]] consteval usize static_field_count() noexcept {
            usize count = 0;
            TypeTraits<std::remove_cv_t<T>>::for_each_member(
                [&count]<auto Member, MemberKind Kind, class... MemberTypeArgs>(std::string_view, auto &&...) {
                    if constexpr (Kind == MemberKind::Field) {
                        ++count;
                    }
                });
            return count;
        }

        /// Builds the `N`-element `StaticFieldInfo` table for `T` (`N` must equal
        /// `static_field_count<T>()`) by re-walking `for_each_member`, this time recording each
        /// field's compile-time-known shape. `decltype(std::declval<T &>().*Member)` (not
        /// `FieldT` erased through a trampoline, unlike the runtime `build_field_info` in
        /// `Macros.hpp`) is used directly so `type_ref<>()` sees the field's exact declared type.
        template <class T, usize N>
        [[nodiscard]] consteval std::array<StaticFieldInfo, N> build_static_fields() noexcept {
            std::array<StaticFieldInfo, N> result{};
            usize index = 0;
            TypeTraits<std::remove_cv_t<T>>::for_each_member(
                [&result, &index]<auto Member, MemberKind Kind, class... MemberTypeArgs>(std::string_view name, auto &&...attrs) {
                    if constexpr (Kind == MemberKind::Field) {
                        using FieldT = std::remove_reference_t<decltype(std::declval<T &>().*Member)>;
                        // `structural_type_ref`, not `type_ref`: a field's type is very often a
                        // container/wrapper (`std::vector<Item>`, `std::optional<i32>`) that has no
                        // canonical name of its own, and `type_ref` only accepts types that do.
                        // Composing the name from parts (see `StructuralTypeId.hpp`) is what lets
                        // `fields()` work on real game types instead of only on all-scalar structs;
                        // for any field type that *does* have its own name, the two agree exactly.
                        result[index++] = StaticFieldInfo{
                            .name = name,
                            .field_type = structural_type_ref<FieldT>(),
                            .size = sizeof(FieldT),
                            .align = alignof(FieldT),
                            .attribute_count = sizeof...(attrs),
                            .attribute_storage = static_attribute_array<decltype(attrs)...>(),
                        };
                    }
                });
            return result;
        }

        /// Counts reflected member functions. Instance methods (`SFT_REFLECT_METHOD`) and static
        /// ones (`SFT_REFLECT_STATIC_METHOD`) are counted together into one table, mirroring how
        /// `TypeInfo::find_method` searches `methods` and `static_methods` as a single namespace;
        /// `StaticMethodInfo::is_static` is what tells them apart.
        template <class T>
        [[nodiscard]] consteval usize static_method_count() noexcept {
            usize count = 0;
            TypeTraits<std::remove_cv_t<T>>::for_each_member(
                [&count]<auto Member, MemberKind Kind, class... MemberTypeArgs>(std::string_view, auto &&...) {
                    if constexpr (Kind == MemberKind::Method || Kind == MemberKind::StaticMethod) {
                        ++count;
                    }
                });
            return count;
        }

        template <class T, usize N>
        [[nodiscard]] consteval std::array<StaticMethodInfo, N> build_static_methods() noexcept {
            std::array<StaticMethodInfo, N> result{};
            usize index = 0;
            TypeTraits<std::remove_cv_t<T>>::for_each_member(
                [&result, &index]<auto Member, MemberKind Kind, class... MemberTypeArgs>(std::string_view name, auto &&...attrs) {
                    if constexpr (Kind == MemberKind::Method || Kind == MemberKind::StaticMethod) {
                        using Traits = MemberFunctionTraits<decltype(Member)>;
                        result[index++] = StaticMethodInfo{
                            .name = name,
                            .return_type = Traits::return_type_ref(),
                            .arity = Traits::arity,
                            .parameters = static_parameter_array<Traits>(),
                            .is_static = (Kind == MemberKind::StaticMethod),
                            .is_const = Traits::is_const,
                            .is_volatile = Traits::is_volatile,
                            .is_noexcept = Traits::is_noexcept,
                            .ref_qualifier = Traits::ref_qualifier,
                            .attribute_count = sizeof...(attrs),
                            .attribute_storage = static_attribute_array<decltype(attrs)...>(),
                        };
                    }
                });
            return result;
        }

        template <class T>
        [[nodiscard]] consteval usize static_constructor_count() noexcept {
            usize count = 0;
            TypeTraits<std::remove_cv_t<T>>::for_each_member(
                [&count]<auto Member, MemberKind Kind, class... MemberTypeArgs>(std::string_view, auto &&...) {
                    if constexpr (Kind == MemberKind::Constructor) {
                        ++count;
                    }
                });
            return count;
        }

        template <class T, usize N>
        [[nodiscard]] consteval std::array<StaticConstructorInfo, N> build_static_constructors() noexcept {
            std::array<StaticConstructorInfo, N> result{};
            usize index = 0;
            TypeTraits<std::remove_cv_t<T>>::for_each_member(
                [&result, &index]<auto Member, MemberKind Kind, class... MemberTypeArgs>(std::string_view, auto &&...) {
                    if constexpr (Kind == MemberKind::Constructor) {
                        result[index++] = StaticConstructorInfo{
                            .arity = sizeof...(MemberTypeArgs),
                            .parameters = static_parameter_array_from_pack<MemberTypeArgs...>(),
                        };
                    }
                });
            return result;
        }

        /// Counts declared events (`SFT_REFLECT_EVENT`). Mirrors `static_constructor_count`.
        template <class T>
        [[nodiscard]] consteval usize static_event_count() noexcept {
            usize count = 0;
            TypeTraits<std::remove_cv_t<T>>::for_each_member(
                [&count]<auto Member, MemberKind Kind, class... MemberTypeArgs>(std::string_view, auto &&...) {
                    if constexpr (Kind == MemberKind::Event) {
                        ++count;
                    }
                });
            return count;
        }

        template <class T, usize N>
        [[nodiscard]] consteval std::array<StaticEventInfo, N> build_static_events() noexcept {
            std::array<StaticEventInfo, N> result{};
            usize index = 0;
            TypeTraits<std::remove_cv_t<T>>::for_each_member(
                [&result, &index]<auto Member, MemberKind Kind, class... MemberTypeArgs>(std::string_view name, auto &&...) {
                    if constexpr (Kind == MemberKind::Event) {
                        result[index++] = StaticEventInfo{
                            .name = name,
                            .arity = sizeof...(MemberTypeArgs),
                            .parameters = static_parameter_array_from_pack<MemberTypeArgs...>(),
                        };
                    }
                });
            return result;
        }

        /// Counts `T`'s reflected bases: the primary base (`TypeTraits<T>::BaseType`, from
        /// `SFT_REFLECT_TYPE_WITH_BASE`/`_WITH_BASES`), if any, plus every secondary base
        /// (`TypeTraits<T>::SecondaryBaseTypes`, from `SFT_REFLECT_TYPE_WITH_BASES`'s trailing
        /// arguments), if any. `0` for a type reflected via plain `SFT_REFLECT_TYPE`.
        template <class T>
        [[nodiscard]] consteval usize static_base_count() noexcept {
            using TypeT = std::remove_cv_t<T>;
            usize count = 0;
            if constexpr (requires { typename TypeTraits<TypeT>::BaseType; }) {
                ++count;
            }
            if constexpr (requires { typename TypeTraits<TypeT>::SecondaryBaseTypes; }) {
                count += std::tuple_size_v<typename TypeTraits<TypeT>::SecondaryBaseTypes>;
            }
            return count;
        }

        /// Builds `T`'s reflected base-set identities, primary base first (when present),
        /// followed by every secondary base in declaration order — the compile-time counterpart
        /// of `TypeInfo::base_type`/`secondary_bases`, which is all a mod or `TypeRegistry` could
        /// previously ask this of at runtime only.
        template <class T, usize N>
        [[nodiscard]] consteval std::array<TypeId, N> build_static_base_types() noexcept {
            using TypeT = std::remove_cv_t<T>;
            std::array<TypeId, N> result{};
            usize index = 0;
            if constexpr (requires { typename TypeTraits<TypeT>::BaseType; }) {
                result[index++] = SFT::Reflection::type_id<typename TypeTraits<TypeT>::BaseType>();
            }
            if constexpr (requires { typename TypeTraits<TypeT>::SecondaryBaseTypes; }) {
                [&result, &index]<class... Bases>(std::type_identity<std::tuple<Bases...>>) {
                    ((result[index++] = SFT::Reflection::type_id<Bases>()), ...);
                }(std::type_identity<typename TypeTraits<TypeT>::SecondaryBaseTypes>{});
            }
            return result;
        }

        /// Deliberately instantiation-dependent `false` (never just `false`), so
        /// `static_assert(FieldNotFound<T, Name>, ...)` only fires when this specific
        /// specialization is actually reached — a bare `static_assert(false, ...)` in a template
        /// body fires unconditionally under some compilers even for un-instantiated branches.
        template <class T, FixedString Name>
        struct FieldNotFound : std::false_type {};

        /// Same idiom as `FieldNotFound`, for `StaticTypeInfo<T>::method_overload<Name,
        /// ParamTypes...>()`: deliberately instantiation-dependent, so it only fires a
        /// `static_assert` for the genuinely-missing case (see `FieldNotFound`'s doc comment for
        /// why a bare `static_assert(false, ...)` would be wrong here). Keyed on `ParamTypes...`
        /// too, not just `T`/`Name` — `method_overload<"take_damage", int>()` and
        /// `method_overload<"take_damage", int, DamageType>()` are different failures (a missing
        /// 1-arg vs. a missing 2-arg overload) and should each instantiate their own
        /// specialization rather than share one.
        template <class T, FixedString Name, class... ParamTypes>
        struct MethodOverloadNotFound : std::false_type {};

    } // namespace Detail

    /// The compile-time-only, `TypeRegistry`-free description of reflected type `T` — the
    /// `StaticTypeInfo` the design doc calls for. Stateless (an empty type); every member is
    /// `static consteval`/`constexpr`, so `reflect<T>()` and every method on it are usable directly
    /// inside a `static_assert`.
    ///
    /// `T` must already be declared via `SFT_REFLECT_TYPE`/`SFT_REFLECT_TYPE_VERSIONED`/
    /// `SFT_REFLECT_TYPE_WITH_BASE` (`Macros.hpp`) — this reads the exact same `TypeTraits<T>::
    /// for_each_member` walk the runtime `Detail::make_type_info<T>()` does, just without ever
    /// producing a heap-owning `TypeInfo`.
    template <class T>
    struct StaticTypeInfo {
        using ReflectedType = T;

        /// Returns `T`'s stable canonical name.
        [[nodiscard]] static consteval std::string_view name() noexcept {
            return Detail::type_name<T>();
        }

        /// Returns `T`'s compile-time canonical identity. Equal to what
        /// `TypeRegistry::type<T>().key` would return at runtime — this and the runtime path
        /// derive from the same `TypeTraits<T>::name`, they just never share a `TypeId`
        /// computation with `typeid(T).name()` (see `StaticTypeId.hpp`).
        [[nodiscard]] static consteval TypeId type_id() noexcept {
            return SFT::Reflection::type_id<T>();
        }

        /// Returns how many reflected bases `T` has: the primary base (if `T` was declared via
        /// `SFT_REFLECT_TYPE_WITH_BASE`/`_WITH_BASES`) plus every secondary base (if declared via
        /// `SFT_REFLECT_TYPE_WITH_BASES`). `0` for plain `SFT_REFLECT_TYPE`.
        [[nodiscard]] static consteval usize base_count() noexcept {
            return Detail::static_base_count<T>();
        }

        /// Returns every reflected base's compile-time identity, primary base first (when
        /// present) followed by secondary bases in declaration order — the compile-time
        /// counterpart of `TypeInfo::base_type`/`TypeInfo::secondary_bases`.
        [[nodiscard]] static consteval std::array<TypeId, base_count()> base_types() noexcept {
            return Detail::build_static_base_types<T, base_count()>();
        }

        /// Reports whether `base` appears in `T`'s reflected base set (primary or secondary).
        /// Declared-only — like `TypeInfo::base_type`/`secondary_bases`, this does not walk
        /// transitively into a base's own bases; see `TypeRegistry::is_assignable_from` for the
        /// runtime, transitive equivalent.
        [[nodiscard]] static consteval bool has_base(TypeId base) noexcept {
            for (TypeId candidate : base_types()) {
                if (candidate == base) {
                    return true;
                }
            }
            return false;
        }

        /// Returns how many data members `T` reflects (`SFT_REFLECT_FIELD` declarations only —
        /// methods/events/constructors are not counted).
        [[nodiscard]] static consteval usize field_count() noexcept {
            return Detail::static_field_count<T>();
        }

        /// Returns every reflected field's compile-time description, in declaration order.
        [[nodiscard]] static consteval std::array<StaticFieldInfo, field_count()> fields() noexcept {
            return Detail::build_static_fields<T, field_count()>();
        }

        /// Reports whether `T` declares a field named `field_name`.
        [[nodiscard]] static consteval bool has_field(std::string_view field_name) noexcept {
            for (const StaticFieldInfo &field : fields()) {
                if (field.name == field_name) {
                    return true;
                }
            }
            return false;
        }

        /// Returns how many member functions `T` reflects — instance and static together, the
        /// same single namespace `TypeInfo::find_method` searches.
        [[nodiscard]] static consteval usize method_count() noexcept {
            return Detail::static_method_count<T>();
        }

        /// Returns every reflected method's compile-time description, in declaration order.
        ///
        /// Unlike `field_count()`, this requires every method's parameter and return types to be
        /// nameable (see `StructurallyIdentifiable`) — a method taking, say, a `std::function<...>`
        /// makes this fail to compile, exactly as `fields()` does for an un-nameable field type.
        [[nodiscard]] static consteval std::array<StaticMethodInfo, method_count()> methods() noexcept {
            return Detail::build_static_methods<T, method_count()>();
        }

        /// Reports whether `T` declares a method named `method_name`. An overload set counts once
        /// per overload, so this answers "is there at least one", matching
        /// `TypeInfo::find_method(name)`'s by-name behavior.
        [[nodiscard]] static consteval bool has_method(std::string_view method_name) noexcept {
            for (const StaticMethodInfo &method : methods()) {
                if (method.name == method_name) {
                    return true;
                }
            }
            return false;
        }

        /// Returns how many declared methods share `method_name` — the size of that specific
        /// overload set, as opposed to `method_count()`'s total across every name. `1` for a
        /// non-overloaded method, `0` when `T` declares no method with this name at all.
        [[nodiscard]] static consteval usize method_count(std::string_view method_name) noexcept {
            usize count = 0;
            for (const StaticMethodInfo &candidate : methods()) {
                if (candidate.name == method_name) {
                    ++count;
                }
            }
            return count;
        }

        /// Reports whether `T` declares a method named `method_name` whose parameter list exactly
        /// matches `param_types`, in order — the compile-time counterpart of
        /// `TypeInfo::find_method(name, param_types)` (`TypeInfo.hpp`). Like that function, this is
        /// an *exact* match (same arity, same parameter identities in the same order): no
        /// assignability, no covariance, no implicit-conversion reasoning, mirroring the runtime's
        /// semantics precisely so a signature that disambiguates one layer disambiguates the other.
        ///
        /// Matching compares each parameter's *unqualified* identity
        /// (`StaticMethodInfo::parameter_types()[i].base`) against `param_types[i]` — not the full
        /// qualifier-preserving `TypeRef` — for the same reason `method_overload<Name,
        /// ParamTypes...>()` below does (see its doc comment): a caller disambiguating an overload
        /// set names the parameter's plain C++ type, not its cv/ref qualification.
        [[nodiscard]] static consteval bool has_method(std::string_view method_name, std::span<const TypeId> param_types) noexcept {
            for (const StaticMethodInfo &candidate : methods()) {
                if (candidate.name != method_name || candidate.arity != param_types.size()) {
                    continue;
                }
                bool all_match = true;
                for (usize i = 0; i < param_types.size(); ++i) {
                    if (candidate.parameters[i].base != param_types[i]) {
                        all_match = false;
                        break;
                    }
                }
                if (all_match) {
                    return true;
                }
            }
            return false;
        }

        /// Returns the compile-time description of the method named `Name`. When `T` declares an
        /// overload set under that name, this returns the first declared one — iterate `methods()`
        /// and compare `parameter_types()` to disambiguate, mirroring how the runtime
        /// `TypeInfo::find_method(name, param_types)` overload exists for the same reason.
        template <Detail::FixedString Name>
        [[nodiscard]] static consteval StaticMethodInfo method() noexcept {
            if constexpr (!has_method(Name.view())) {
                static_assert(Detail::FieldNotFound<T, Name>::value,
                              "StaticTypeInfo<T>::method<Name>(): T declares no method with this name.");
            }
            constexpr std::array<StaticMethodInfo, method_count()> all = methods();
            for (const StaticMethodInfo &candidate : all) {
                if (candidate.name == Name.view()) {
                    return candidate;
                }
            }
            return StaticMethodInfo{};
        }

        /// Returns the compile-time description of the single overload of `Name` whose parameter
        /// list is exactly `ParamTypes...`, in order — the compile-time answer to Java's
        /// `Class.getMethod(name, ParameterTypes...)`, and the disambiguating counterpart to
        /// `method<Name>()` above (which always returns the *first* declared overload and cannot
        /// pick a specific one).
        ///
        /// @par Naming: why not an overload of `method<Name>()` itself
        /// The natural API would be to give this the same name, `method`, distinguished only by a
        /// trailing `class... ParamTypes` pack that is empty for the no-disambiguation case — i.e.
        /// keep the existing `template <FixedString Name> method()` and add a second `template
        /// <FixedString Name, class... ParamTypes> method()` beside it. That was tried first, as
        /// the more natural single-name API, and rejected because it does not actually work:
        /// `method<"aim">()` (zero explicit `ParamTypes`) is ambiguous between the two templates.
        /// A trailing pack with nothing to deduce it from (there are no function parameters here
        /// to deduce against — every argument is a template argument) simply deduces to an empty
        /// pack rather than failing substitution, so both templates become viable for the exact
        /// same call, with identical (empty) function parameter lists after substitution — and
        /// partial ordering between two function templates with identical parameter-type lists
        /// does not favor "fewer template parameters" the way it favors "fewer/more specific
        /// function parameters". Confirmed empirically (both Clang and the standard's partial
        /// ordering rules agree): the call is rejected as ambiguous, not resolved to the
        /// non-pack overload. Renaming this one to `method_overload` sidesteps the ambiguity
        /// entirely and keeps `method<Name>()`'s existing "first declared, zero disambiguation"
        /// behavior completely unchanged for every existing call site.
        ///
        /// @par Matching against unqualified parameter identity
        /// Each `ParamTypes...` entry is compared against `parameter_types()[i].base` — the
        /// qualifier-*stripped* identity — not the full qualifier-preserving `TypeRef` a
        /// `StaticMethodInfo` otherwise records. This mirrors how the parameter table itself is
        /// built: `Detail::MemberFunctionTraits::param_type_refs()` derives `.base` from
        /// `structural_type_ref<Args>()`, i.e. from the parameter's bare declared type, regardless
        /// of whether the C++ signature actually takes it by value, `const&`, or `&&` — so a
        /// caller disambiguating `void take_damage(int)` from `void take_damage(int, DamageType)`
        /// writes `method_overload<"take_damage", int, DamageType>()` naming plain types, exactly
        /// as `SFT_REFLECT_METHOD_OVERLOAD`'s own `POINTER_TYPE` argument and
        /// `SFT_REFLECT_INVOKE_OVERLOAD` (`Invoke.hpp`) both already expect at the macro layer —
        /// nobody has to additionally know or spell out that `take_damage`'s first parameter
        /// happens to be taken by `const &`.
        ///
        /// `structural_type_id<ParamTypes>()`, not the narrower `type_id<ParamTypes>()`, is what
        /// each supplied type is resolved through: `parameter_types()[i].base` was itself built
        /// via `structural_type_ref`/`structural_type_id` (see above), and `structural_type_id<T>()
        /// == type_id<T>()` for every `T` the latter already accepts (`StructuralTypeId.hpp`'s
        /// doc comment), so using the wider one uniformly is both strictly more capable (covers a
        /// `std::vector<int>`-shaped disambiguating parameter, which a plain `type_id` would
        /// reject outright) and never produces a different answer for the common scalar/named-type
        /// case `type_id` alone would have covered.
        ///
        /// A compile error — not a runtime failure — when no declared overload of `Name` matches
        /// `ParamTypes...` exactly (wrong name, wrong arity, or a parameter type mismatch against
        /// every same-named candidate), via the same `if constexpr` + instantiation-dependent-false
        /// idiom `method<Name>()`/`field<Name>()` use (see `field<Name>()`'s doc comment for why a
        /// bare trailing `static_assert` would be wrong here).
        template <Detail::FixedString Name, class... ParamTypes>
        [[nodiscard]] static consteval StaticMethodInfo method_overload() noexcept {
            if constexpr (!has_method_overload<Name, ParamTypes...>()) {
                static_assert(Detail::MethodOverloadNotFound<T, Name, ParamTypes...>::value,
                              "StaticTypeInfo<T>::method_overload<Name, ParamTypes...>(): T declares no "
                              "method named Name whose parameter list exactly matches ParamTypes...");
            }
            constexpr std::array<TypeId, sizeof...(ParamTypes)> wanted{structural_type_id<ParamTypes>()...};
            constexpr std::array<StaticMethodInfo, method_count()> all = methods();
            for (const StaticMethodInfo &candidate : all) {
                if (candidate.name != Name.view() || candidate.arity != wanted.size()) {
                    continue;
                }
                bool all_match = true;
                for (usize i = 0; i < wanted.size(); ++i) {
                    if (candidate.parameters[i].base != wanted[i]) {
                        all_match = false;
                        break;
                    }
                }
                if (all_match) {
                    return candidate;
                }
            }
            return StaticMethodInfo{}; // unreachable: has_method_overload<...>() guarantees a match above.
        }

        /// Reports whether `method_overload<Name, ParamTypes...>()` would find a match, without
        /// hard-failing when it would not — the `template`-based, disambiguating equivalent of
        /// `has_method(method_name, param_types)` above, for when the parameter types are known at
        /// the call site as types rather than as a runtime `std::span<const TypeId>`.
        template <Detail::FixedString Name, class... ParamTypes>
        [[nodiscard]] static consteval bool has_method_overload() noexcept {
            constexpr std::array<TypeId, sizeof...(ParamTypes)> wanted{structural_type_id<ParamTypes>()...};
            for (const StaticMethodInfo &candidate : methods()) {
                if (candidate.name != Name.view() || candidate.arity != wanted.size()) {
                    continue;
                }
                bool all_match = true;
                for (usize i = 0; i < wanted.size(); ++i) {
                    if (candidate.parameters[i].base != wanted[i]) {
                        all_match = false;
                        break;
                    }
                }
                if (all_match) {
                    return true;
                }
            }
            return false;
        }

        /// Returns how many parameterized constructors (`SFT_REFLECT_CONSTRUCTOR`) `T` declares.
        [[nodiscard]] static consteval usize constructor_count() noexcept {
            return Detail::static_constructor_count<T>();
        }

        /// Returns every reflected constructor's compile-time signature, in declaration order.
        [[nodiscard]] static consteval std::array<StaticConstructorInfo, constructor_count()> constructors() noexcept {
            return Detail::build_static_constructors<T, constructor_count()>();
        }

        /// Returns how many events (`SFT_REFLECT_EVENT`) `T` declares.
        [[nodiscard]] static consteval usize event_count() noexcept {
            return Detail::static_event_count<T>();
        }

        /// Returns every reflected event's compile-time description, in declaration order.
        [[nodiscard]] static consteval std::array<StaticEventInfo, event_count()> events() noexcept {
            return Detail::build_static_events<T, event_count()>();
        }

        /// Reports whether `T` declares an event named `event_name`.
        [[nodiscard]] static consteval bool has_event(std::string_view event_name) noexcept {
            for (const StaticEventInfo &event : events()) {
                if (event.name == event_name) {
                    return true;
                }
            }
            return false;
        }

        /// Returns the compile-time description of the field named `Name` (a string-literal
        /// non-type template parameter, e.g. `field<"health">()`). A compile error — not a
        /// runtime failure — when `T` declares no such field; check `has_field(Name.view())`
        /// first if that needs to be a query rather than a hard requirement.
        template <Detail::FixedString Name>
        [[nodiscard]] static consteval StaticFieldInfo field() noexcept {
            // `if constexpr` (not a bare `static_assert` below the loop) is load-bearing here: a
            // `static_assert` is checked unconditionally at instantiation regardless of whether a
            // runtime-shaped `return` earlier in the function body would have bypassed it, so an
            // unconditional `static_assert(FieldNotFound<...>::value, ...)` would hard-fail for
            // *every* `field<Name>()` call, found or not. Gating it behind a compile-time-known
            // `found` makes it fire only for the genuinely-missing case.
            if constexpr (!has_field(Name.view())) {
                static_assert(Detail::FieldNotFound<T, Name>::value,
                              "StaticTypeInfo<T>::field<Name>(): T declares no field with this name.");
            }
            constexpr std::array<StaticFieldInfo, field_count()> all = fields();
            for (const StaticFieldInfo &candidate : all) {
                if (candidate.name == Name.view()) {
                    return candidate;
                }
            }
            return StaticFieldInfo{}; // unreachable: `found` guarantees a match above.
        }
    };

    /// Returns `T`'s compile-time-only reflection view. `T` must be `SFT_REFLECT_TYPE`-annotated.
    ///
    /// @return Returns a stateless `StaticTypeInfo<T>`, usable directly in a `static_assert`, e.g.
    /// `static_assert(reflect<Player>().has_field("health"));`.
    template <class T>
    [[nodiscard]] consteval StaticTypeInfo<T> reflect() noexcept {
        return StaticTypeInfo<T>{};
    }

    // ── Tier 0: direct compile-time member access/invocation/construction ─────────────────────
    //
    // Each of these takes the member pointer/constructor argument types directly as a template
    // argument — there is no name-based lookup, no `TypeRegistry`, nothing to resolve at runtime.
    // They compile to exactly the ordinary C++ expression a hand-written call site would use, and
    // are fully inlinable/optimizable by the compiler and LTO. This is the "unmodded game" path:
    // reflection that is only ever used this way leaves no trace in the generated code beyond the
    // access/call itself.

    /// Reads field `Member` off `object` — exactly `object.*Member`, nothing else.
    ///
    /// @return Returns a reference to the field.
    template <class T, auto Member>
    [[nodiscard]] constexpr decltype(auto) get(const T &object) noexcept {
        return (object.*Member);
    }

    /// Reads field `Member` off `object` — exactly `object.*Member`, nothing else.
    ///
    /// @return Returns a reference to the field.
    template <class T, auto Member>
    [[nodiscard]] constexpr decltype(auto) get(T &object) noexcept {
        return (object.*Member);
    }

    /// Writes `value` into field `Member` on `object` — exactly `object.*Member = value`.
    template <class T, auto Member, class Value>
    constexpr void set(T &object, Value &&value) noexcept(noexcept(object.*Member = std::forward<Value>(value))) {
        object.*Member = std::forward<Value>(value);
    }

    /// Calls method `Member` on `object` with `args` — exactly `(object.*Member)(args...)`.
    ///
    /// @return Returns whatever `Member` returns.
    template <class T, auto Member, class... Args>
    constexpr decltype(auto) invoke(T &object, Args &&...args) noexcept(noexcept((object.*Member)(std::forward<Args>(args)...))) {
        return (object.*Member)(std::forward<Args>(args)...);
    }

    /// Constructs a `T` from `args` — exactly `T(args...)`. The compile-time analog of
    /// `default_construct_instance`/`ConstructorInfo::invoke` (`TypeInfo.hpp`/
    /// `ConstructorInfo.hpp`) for the case where `T` and the argument types are both known at the
    /// call site, requiring no placement-new/type-erased trampoline at all.
    ///
    /// @return Returns the newly constructed value.
    template <class T, class... Args>
    [[nodiscard]] constexpr T construct(Args &&...args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        return T(std::forward<Args>(args)...);
    }


} // namespace SFT::Reflection
