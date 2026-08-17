#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <type_traits>
#pragma endregion

namespace SFT::RHI {


    template <class E>
    struct enable_flag_ops : std::false_type {};

    template <class E>
    concept FlagEnum = std::is_enum_v<E> && enable_flag_ops<E>::value;

    /// Combines the operands with bitwise OR.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    template <FlagEnum E>
    [[nodiscard]] constexpr E operator|(E a, E b) noexcept {
        using U = std::underlying_type_t<E>;
        return static_cast<E>(static_cast<U>(a) | static_cast<U>(b));
    }

    /// Combines the operands with bitwise AND.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    template <FlagEnum E>
    [[nodiscard]] constexpr E operator&(E a, E b) noexcept {
        using U = std::underlying_type_t<E>;
        return static_cast<E>(static_cast<U>(a) & static_cast<U>(b));
    }

    /// Combines the operands with bitwise XOR.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    template <FlagEnum E>
    [[nodiscard]] constexpr E operator^(E a, E b) noexcept {
        using U = std::underlying_type_t<E>;
        return static_cast<E>(static_cast<U>(a) ^ static_cast<U>(b));
    }

    /// Implements `operator~` for `RHI`.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    template <FlagEnum E>
    [[nodiscard]] constexpr E operator~(E a) noexcept {
        using U = std::underlying_type_t<E>;
        return static_cast<E>(~static_cast<U>(a));
    }

    /// Combines this object with the right-hand operand using bitwise OR.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    template <FlagEnum E>
    constexpr E &operator|=(E &a, E b) noexcept {
        return a = a | b;
    }

    /// Combines this object with the right-hand operand using bitwise AND.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    template <FlagEnum E>
    constexpr E &operator&=(E &a, E b) noexcept {
        return a = a & b;
    }

    /// Combines this object with the right-hand operand using bitwise XOR.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    template <FlagEnum E>
    constexpr E &operator^=(E &a, E b) noexcept {
        return a = a ^ b;
    }


    /// Reports whether any is available.
    ///
    /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    template <FlagEnum E>
    [[nodiscard]] constexpr bool has_any(E value, E mask) noexcept {
        using U = std::underlying_type_t<E>;
        return (static_cast<U>(value) & static_cast<U>(mask)) != U{0};
    }

    /// Reports whether all is available.
    ///
    /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    template <FlagEnum E>
    [[nodiscard]] constexpr bool has_all(E value, E mask) noexcept {
        using U = std::underlying_type_t<E>;
        return (static_cast<U>(value) & static_cast<U>(mask)) == static_cast<U>(mask);
    }

    /// Reports whether empty holds for this `RHI`.
    ///
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    template <FlagEnum E>
    [[nodiscard]] constexpr bool is_empty(E value) noexcept {
        return static_cast<std::underlying_type_t<E>>(value) == std::underlying_type_t<E>{0};
    }

} // namespace SFT::RHI
