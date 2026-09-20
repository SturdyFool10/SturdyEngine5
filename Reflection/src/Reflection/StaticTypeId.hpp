#pragma once

#include <Reflection/EnumInfo.hpp>
#include <Reflection/TypeId.hpp>
#include <Reflection/TypeInfo.hpp>
#include <Reflection/TypeRef.hpp>

#include <Foundation/Foundation.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

namespace SFT::Reflection {

    namespace Detail {

        /// Compile-time canonical name for a fundamental (non-reflected, non-enum) type, keyed by
        /// exact type — the piece that lets `type_id<T>()` below be `consteval` for `int`/`float`/
        /// `UString`/etc. without ever falling back to `typeid(T).name()` (compiler/ABI-dependent,
        /// unstable across builds — see the audit that motivated this file). Deliberately a
        /// closed, explicit set: an unlisted, unreflected type is a `type_id<T>()` compile error
        /// rather than a silent, unstable identity (`Detail::erased_type_id` in `Macros.hpp`
        /// remains the lenient, RTTI-fallback runtime path for the macro-based frontend's existing
        /// field/param types until those are migrated onto this table).
        template <class T>
        struct FundamentalTypeName {
            static constexpr std::string_view value{};
        };

#define STURDY_REFLECTION_FUNDAMENTAL_NAME(TYPE, NAME)     \
    template <>                                            \
    struct FundamentalTypeName<TYPE> {                      \
        static constexpr std::string_view value{NAME};      \
    };

        STURDY_REFLECTION_FUNDAMENTAL_NAME(bool, "bool")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(char, "char")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(i8, "i8")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(i16, "i16")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(i32, "i32")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(i64, "i64")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(u8, "u8")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(u16, "u16")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(u32, "u32")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(u64, "u64")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(f32, "f32")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(f64, "f64")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(std::string_view, "std::string_view")
        // Deliberately spelled as the engine's own vocabulary types rather than their underlying
        // spellings: these names are the *canonical* identity mods/saves/networking compare
        // against, so they must not drift if, say, `UString`'s internal representation changes.
        STURDY_REFLECTION_FUNDAMENTAL_NAME(std::string, "std::string")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(Foundation::UString, "UString")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(void, "void")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(std::byte, "std::byte")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(std::nullptr_t, "std::nullptr_t")
        // `long long`/`unsigned long long` are distinct types from `i64`/`u64` (which are
        // `long`/`unsigned long` on every LP64 target this engine builds for), so they need their
        // own entries rather than aliasing onto the fixed-width ones.
        STURDY_REFLECTION_FUNDAMENTAL_NAME(long long, "long long")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(unsigned long long, "unsigned long long")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(wchar_t, "wchar_t")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(char8_t, "char8_t")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(char16_t, "char16_t")
        STURDY_REFLECTION_FUNDAMENTAL_NAME(char32_t, "char32_t")

#undef STURDY_REFLECTION_FUNDAMENTAL_NAME

        template <class T>
        concept HasFundamentalTypeName = !FundamentalTypeName<T>::value.empty();

        /// True when `T` (after `remove_cv`) has a compile-time canonical identity available:
        /// reflected (`SFT_REFLECT_TYPE`), a reflected enum (`SFT_REFLECT_ENUM`), or a known
        /// fundamental type (`FundamentalTypeName`). A `concept`, not a `static_assert`
        /// consequence, specifically so callers can *ask* "does `type_id<T>()` work for this `T`"
        /// via `requires`/`if constexpr` — e.g. `TypeRegistry::try_register<T>()` uses this to
        /// decide, per type, whether it's safe to cross-check a runtime `TypeInfo` against the
        /// static layer, without hard-failing for the (still common) types with a field whose
        /// type doesn't yet have a static identity (containers, `UString`, ...).
        template <class T>
        concept HasStableTypeId = !TypeTraits<std::remove_cv_t<T>>::name.empty() ||
                                   !EnumTraits<std::remove_cv_t<T>>::name.empty() ||
                                   HasFundamentalTypeName<std::remove_cv_t<T>>;

        /// The canonical name string `type_id<T>()` hashes, exposed separately so a *composed*
        /// identity (`StructuralTypeId.hpp`'s `std::vector<game.Player>`-style names for container
        /// shapes) can be assembled from its parts without re-implementing — or drifting out of
        /// sync with — the reflected/enum/fundamental resolution order below.
        template <class T>
            requires HasStableTypeId<T>
        [[nodiscard]] consteval std::string_view stable_type_name() noexcept {
            using TypeT = std::remove_cv_t<T>;
            if constexpr (!TypeTraits<TypeT>::name.empty()) {
                return TypeTraits<TypeT>::name;
            } else if constexpr (!EnumTraits<TypeT>::name.empty()) {
                return EnumTraits<TypeT>::name;
            } else {
                return FundamentalTypeName<TypeT>::value;
            }
        }

    } // namespace Detail

    /// The compile-time, canonical-identity equivalent of `Detail::erased_type_id<T>()`
    /// (`Macros.hpp`): resolves to the same `TypeId` a reflected type's own `TypeInfo::key`/
    /// `EnumInfo::key` carries when `T` is reflected, the same for a reflected enum, and a stable
    /// name for a known fundamental type — but, unlike `erased_type_id`, never falls back to
    /// `typeid(T).name()`. An unreflected, non-fundamental `T` fails to find this overload at all
    /// (see `Detail::HasStableTypeId`) rather than resolving to a compiler/ABI-dependent identity,
    /// satisfying the "no `typeid().name()` in canonical reflection identity" requirement for the
    /// compile-time-first API surface (`StaticReflection.hpp`, `reflect<T>()`, `get`/`set`/
    /// `invoke`). The `requires` clause (rather than an internal `static_assert`) is what lets
    /// `Detail::HasStableTypeId<T>`/`requires { type_id<T>(); }` detect support instead of always
    /// hard-failing the moment anyone names this function for an unsupported `T`.
    ///
    /// @return Returns the newly constructed id.
    template <class T>
    [[nodiscard]] consteval TypeId type_id() noexcept
        requires Detail::HasStableTypeId<T>
    {
        return TypeId::from_name(Detail::stable_type_name<T>());
    }

    /// The `TypeRef`-producing equivalent of `type_id<T>()`, preserving `T`'s exact cv/ref/pointer
    /// qualifiers (see `TypeRef`'s doc comment) rather than requiring the caller to
    /// `remove_cvref_t` first. See `type_id`'s doc comment for why this is `requires`-constrained
    /// rather than `static_assert`-guarded.
    ///
    /// @return Returns the newly constructed reference.
    template <class T>
    [[nodiscard]] consteval TypeRef type_ref() noexcept
        requires Detail::HasStableTypeId<std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<T>>>>
    {
        using Bare = std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<T>>>;
        return TypeRef{.base = type_id<Bare>(), .qualifiers = Detail::qualifiers_of<T>()};
    }


} // namespace SFT::Reflection
