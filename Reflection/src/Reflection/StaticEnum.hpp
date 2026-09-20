#pragma once

#include <Reflection/EnumInfo.hpp>
#include <Reflection/FixedString.hpp>
#include <Reflection/Macros.hpp>
#include <Reflection/StaticTypeId.hpp>
#include <Reflection/TypeId.hpp>

#include <Foundation/Foundation.hpp>

#include <array>
#include <string_view>
#include <type_traits>

/// The compile-time-only half of enum reflection — the `StaticEnumInfo` the design doc calls for,
/// and the enum counterpart to `StaticReflection.hpp`'s `StaticTypeInfo<T>`.
///
/// Everything here is `consteval`: `name_of`/`value_of`/`contains` resolve during compilation and
/// leave no runtime trace, so the extremely common "turn this enum into a string for a log line,
/// a save file, or a console command" operation costs nothing at all when the enumerator is known
/// at the call site. The design doc's requirement that "compile-time enum operations must not
/// require TypeRegistry" is structural here rather than aspirational: this file does not include
/// `TypeRegistry.hpp` and could not reach the registry if it wanted to.
///
/// The runtime `EnumInfo`/`TypeRegistry::find_enum` path (`EnumInfo.hpp`) remains for the dynamic
/// cases it is actually needed for — a mod resolving an enumerator by a name it only learned at
/// runtime, or an enum registered dynamically with no C++ type behind it at all.
namespace SFT::Reflection {


    /// One enumerator's compile-time description. `value` is widened to `i64` so every enum shares
    /// one homogeneous descriptor type regardless of underlying type; the original typed value is
    /// recoverable via `StaticEnumInfo<E>::value_of` or a plain `static_cast<E>(value)`.
    struct StaticEnumeratorInfo {
        std::string_view name;
        i64 value = 0;

        /// Compares the operands for equality.
        ///
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(const StaticEnumeratorInfo &, const StaticEnumeratorInfo &) noexcept = default;
    };

    namespace Detail {

        /// Counts `EnumTraits<E>::for_each_enumerator`'s declarations — the first of the two passes
        /// `StaticEnumInfo<E>::enumerators()` needs, since `std::array` wants its size up front.
        template <class E>
        [[nodiscard]] consteval usize static_enumerator_count() noexcept {
            usize count = 0;
            EnumTraits<std::remove_cv_t<E>>::for_each_enumerator(
                [&count]<auto Value>(std::string_view) { (void)Value; ++count; });
            return count;
        }

        template <class E, usize N>
        [[nodiscard]] consteval std::array<StaticEnumeratorInfo, N> build_static_enumerators() noexcept {
            std::array<StaticEnumeratorInfo, N> result{};
            usize index = 0;
            EnumTraits<std::remove_cv_t<E>>::for_each_enumerator(
                [&result, &index]<auto Value>(std::string_view name) {
                    result[index++] = StaticEnumeratorInfo{.name = name, .value = static_cast<i64>(Value)};
                });
            return result;
        }

        /// Deliberately instantiation-dependent `false`, so the "no such enumerator" diagnostic
        /// fires only for the specialization that actually reached it — same idiom as
        /// `StaticReflection.hpp`'s `FieldNotFound`.
        template <class E, FixedString Name>
        struct EnumeratorNotFound : std::false_type {};

    } // namespace Detail

    /// The compile-time-only, `TypeRegistry`-free description of reflected enum `E`. Stateless;
    /// every member is `static consteval`, so all of it is usable inside a `static_assert`.
    ///
    /// `E` must be declared via `SFT_REFLECT_ENUM`/`SFT_REFLECT_ENUM_VALUE` (`Macros.hpp`) — this
    /// reads the same `EnumTraits<E>::for_each_enumerator` walk the runtime
    /// `Detail::make_enum_info<E>()` does, without ever producing a heap-owning `EnumInfo`.
    template <class E>
    struct StaticEnumInfo {
        static_assert(std::is_enum_v<std::remove_cv_t<E>>, "StaticEnumInfo<E> requires an enum type.");

        using ReflectedEnum = std::remove_cv_t<E>;
        using Underlying = std::underlying_type_t<ReflectedEnum>;

        /// Returns `E`'s stable canonical name.
        [[nodiscard]] static consteval std::string_view name() noexcept {
            return Detail::enum_name<ReflectedEnum>();
        }

        /// Returns `E`'s compile-time canonical identity — the same `TypeId` the runtime
        /// `EnumInfo::key` carries, both being derived from `EnumTraits<E>::name`.
        [[nodiscard]] static consteval TypeId type_id() noexcept {
            return SFT::Reflection::type_id<ReflectedEnum>();
        }

        /// Returns the identity of `E`'s underlying integer type (`i32` for a plain `enum class`).
        [[nodiscard]] static consteval TypeId underlying_type() noexcept {
            return SFT::Reflection::type_id<Underlying>();
        }

        /// Reports whether `E` is a scoped enum (`enum class`) rather than an unscoped one.
        /// Detected by whether `E` implicitly converts to its own underlying type — the one
        /// observable behavioral difference between the two in the language.
        [[nodiscard]] static consteval bool is_scoped() noexcept {
            return !std::is_convertible_v<ReflectedEnum, Underlying>;
        }

        /// Returns how many enumerators `E` declares via `SFT_REFLECT_ENUM_VALUE`.
        [[nodiscard]] static consteval usize enumerator_count() noexcept {
            return Detail::static_enumerator_count<ReflectedEnum>();
        }

        /// Returns every declared enumerator, in declaration order.
        [[nodiscard]] static consteval std::array<StaticEnumeratorInfo, enumerator_count()> enumerators() noexcept {
            return Detail::build_static_enumerators<ReflectedEnum, enumerator_count()>();
        }

        /// Reports whether `value` corresponds to a declared enumerator. The check a
        /// deserializer/network reader needs before trusting an integer as an enum — C++ lets any
        /// underlying-type value be cast to an enum, so "is this one of the real ones" is a
        /// question that has to be asked explicitly.
        [[nodiscard]] static consteval bool contains(ReflectedEnum value) noexcept {
            for (const StaticEnumeratorInfo &entry : enumerators()) {
                if (entry.value == static_cast<i64>(value)) {
                    return true;
                }
            }
            return false;
        }

        /// Reports whether `enumerator_name` names a declared enumerator.
        [[nodiscard]] static consteval bool contains(std::string_view enumerator_name) noexcept {
            for (const StaticEnumeratorInfo &entry : enumerators()) {
                if (entry.name == enumerator_name) {
                    return true;
                }
            }
            return false;
        }

        /// Returns `value`'s declared name, or an empty view when it is not a declared enumerator.
        ///
        /// When two enumerators share a value (an alias, e.g. `Count = Blue`), this returns the
        /// first declared one — the alias is still reachable through `enumerators()`, which lists
        /// every declaration including aliases.
        [[nodiscard]] static consteval std::string_view name_of(ReflectedEnum value) noexcept {
            for (const StaticEnumeratorInfo &entry : enumerators()) {
                if (entry.value == static_cast<i64>(value)) {
                    return entry.name;
                }
            }
            return std::string_view{};
        }

        /// Returns the enumerator named `Name`. A compile error — not a sentinel — when `E`
        /// declares no such enumerator, which is the whole advantage of resolving a name at
        /// compile time instead of through `EnumInfo::find_enumerator` at runtime.
        template <Detail::FixedString Name>
        [[nodiscard]] static consteval ReflectedEnum value_of() noexcept {
            if constexpr (!contains(Name.view())) {
                static_assert(Detail::EnumeratorNotFound<ReflectedEnum, Name>::value,
                              "StaticEnumInfo<E>::value_of<Name>(): E declares no enumerator with this name.");
            }
            for (const StaticEnumeratorInfo &entry : enumerators()) {
                if (entry.name == Name.view()) {
                    return static_cast<ReflectedEnum>(entry.value);
                }
            }
            return ReflectedEnum{};
        }

        /// Resolves `enumerator_name` to its value, writing it to `out` and returning `true` on
        /// success. The lookup-by-a-name-you-only-have-as-data counterpart to `value_of<Name>()`;
        /// `constexpr` rather than `consteval` because the name may legitimately come from runtime
        /// input (a console command, a save file), which is exactly when a sentinel-returning form
        /// is the right shape instead of a hard compile error.
        [[nodiscard]] static constexpr bool try_value_of(std::string_view enumerator_name, ReflectedEnum &out) noexcept {
            for (const StaticEnumeratorInfo &entry : enumerators()) {
                if (entry.name == enumerator_name) {
                    out = static_cast<ReflectedEnum>(entry.value);
                    return true;
                }
            }
            return false;
        }
    };

    /// Returns `E`'s compile-time-only reflection view. `E` must be `SFT_REFLECT_ENUM`-annotated.
    ///
    /// @return Returns a stateless `StaticEnumInfo<E>`, usable directly in a `static_assert`, e.g.
    /// `static_assert(reflect_enum<Color>().name_of(Color::Red) == "Red");`.
    template <class E>
    [[nodiscard]] consteval StaticEnumInfo<E> reflect_enum() noexcept {
        return StaticEnumInfo<E>{};
    }


} // namespace SFT::Reflection
