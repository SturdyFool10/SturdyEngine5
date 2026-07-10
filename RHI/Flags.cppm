module;

#pragma region Imports
#include <type_traits>
#pragma endregion

export module Sturdy.RHI:Flags;

import Sturdy.Foundation;

export namespace SFT::RHI {

    // Opt-in bitmask machinery for the RHI's `enum class` flag types (BufferUsage, TextureUsage,
    // ShaderStage, ColorWriteMask, ...). A scoped enum has no bitwise operators by default — rather
    // than hand-writing `|`/`&`/`~`/`has_any` for each flag enum, a flag enum's own partition
    // specializes `enable_flag_ops<E>` to `true_type` and inherits the whole operator set below via
    // ADL (the operators live in `SFT::RHI` alongside every flag enum, so they're found without a
    // using-directive at the call site).
    template <class E>
    struct enable_flag_ops : std::false_type {};

    template <class E>
    concept FlagEnum = std::is_enum_v<E> && enable_flag_ops<E>::value;

    template <FlagEnum E>
    [[nodiscard]] constexpr E operator|(E a, E b) noexcept {
        using U = std::underlying_type_t<E>;
        return static_cast<E>(static_cast<U>(a) | static_cast<U>(b));
    }

    template <FlagEnum E>
    [[nodiscard]] constexpr E operator&(E a, E b) noexcept {
        using U = std::underlying_type_t<E>;
        return static_cast<E>(static_cast<U>(a) & static_cast<U>(b));
    }

    template <FlagEnum E>
    [[nodiscard]] constexpr E operator^(E a, E b) noexcept {
        using U = std::underlying_type_t<E>;
        return static_cast<E>(static_cast<U>(a) ^ static_cast<U>(b));
    }

    template <FlagEnum E>
    [[nodiscard]] constexpr E operator~(E a) noexcept {
        using U = std::underlying_type_t<E>;
        return static_cast<E>(~static_cast<U>(a));
    }

    template <FlagEnum E>
    constexpr E &operator|=(E &a, E b) noexcept {
        return a = a | b;
    }

    template <FlagEnum E>
    constexpr E &operator&=(E &a, E b) noexcept {
        return a = a & b;
    }

    template <FlagEnum E>
    constexpr E &operator^=(E &a, E b) noexcept {
        return a = a ^ b;
    }

    // `has_any` — do `value` and `mask` share at least one bit; `has_all` — does `value` contain
    // every bit of `mask`. Both read clearer at a call site than an inline `(value & mask) != 0`.
    template <FlagEnum E>
    [[nodiscard]] constexpr bool has_any(E value, E mask) noexcept {
        using U = std::underlying_type_t<E>;
        return (static_cast<U>(value) & static_cast<U>(mask)) != U{0};
    }

    template <FlagEnum E>
    [[nodiscard]] constexpr bool has_all(E value, E mask) noexcept {
        using U = std::underlying_type_t<E>;
        return (static_cast<U>(value) & static_cast<U>(mask)) == static_cast<U>(mask);
    }

    template <FlagEnum E>
    [[nodiscard]] constexpr bool is_empty(E value) noexcept {
        return static_cast<std::underlying_type_t<E>>(value) == std::underlying_type_t<E>{0};
    }

} // namespace SFT::RHI
