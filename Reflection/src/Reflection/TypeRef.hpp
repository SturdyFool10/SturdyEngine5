#pragma once

#include <Reflection/TypeId.hpp>

#include <Foundation/Foundation.hpp>

#include <type_traits>

namespace SFT::Reflection {


    /// Qualifiers stripped off a type by `remove_cvref`-style erasure elsewhere in this package
    /// (`Detail::erased_type_id`'s `param_types`/`field_type`/`return_type`), preserved here
    /// instead so a full signature — `void foo(const Player&, Item&&) const noexcept` — can be
    /// reconstructed rather than degraded to `void foo(Player, Item)`.
    enum class TypeQualifiers : u32 {
        None = 0,
        Const = 1u << 0u,
        Volatile = 1u << 1u,
        LValueRef = 1u << 2u,
        RValueRef = 1u << 3u,
        Pointer = 1u << 4u,
    };

    /// Combines the operands with bitwise OR.
    ///
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr TypeQualifiers operator|(TypeQualifiers lhs, TypeQualifiers rhs) noexcept {
        return static_cast<TypeQualifiers>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
    }

    /// Reports whether `flag` is set on `value`.
    ///
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr bool has_flag(TypeQualifiers value, TypeQualifiers flag) noexcept {
        return (static_cast<u32>(value) & static_cast<u32>(flag)) != 0;
    }

    /// A type identity (`TypeId`) plus the cv/ref/pointer qualifiers that were on the original
    /// declaration — `field_type`/`return_type`/`param_types` used a bare `TypeId` before this
    /// existed, which meant `const Player&` and `Player` were indistinguishable once erased. This
    /// is purely additive metadata: `base` alone remains a valid, comparable identity for "what
    /// type is this" (two `TypeRef`s to the same base type but different qualifiers still name the
    /// same reflected type), while `qualifiers` recovers what was lost.
    struct TypeRef {
        TypeId base{};
        TypeQualifiers qualifiers = TypeQualifiers::None;

        /// Compares the operands for equality (identity and qualifiers must both match).
        ///
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(TypeRef, TypeRef) noexcept = default;
    };

    namespace Detail {

        /// Derives a `TypeRef`'s qualifiers from `T` exactly as declared (before any
        /// `remove_cvref`/`remove_pointer` is applied) — `T` may be `const Player&`, `Item&&`,
        /// `const Weapon*`, or a plain value type.
        template <class T>
        [[nodiscard]] consteval TypeQualifiers qualifiers_of() noexcept {
            using NoRef = std::remove_reference_t<T>;
            TypeQualifiers result = TypeQualifiers::None;
            if constexpr (std::is_lvalue_reference_v<T>) {
                result = result | TypeQualifiers::LValueRef;
            }
            if constexpr (std::is_rvalue_reference_v<T>) {
                result = result | TypeQualifiers::RValueRef;
            }
            if constexpr (std::is_pointer_v<NoRef>) {
                result = result | TypeQualifiers::Pointer;
            }
            using Pointee = std::remove_pointer_t<NoRef>;
            if constexpr (std::is_const_v<Pointee>) {
                result = result | TypeQualifiers::Const;
            }
            if constexpr (std::is_volatile_v<Pointee>) {
                result = result | TypeQualifiers::Volatile;
            }
            return result;
        }

    } // namespace Detail


} // namespace SFT::Reflection
