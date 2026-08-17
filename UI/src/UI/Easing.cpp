#include <UI/src/UI/Easing.hpp>


namespace SFT::UI::Easing {

    /// Performs the linear operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 linear(f32 t) noexcept { return t; }

    /// Performs the quad in operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 quad_in(f32 t) noexcept { return t * t; }

    /// Performs the quad out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 quad_out(f32 t) noexcept { return 1.0f - (1.0f - t) * (1.0f - t); }

    /// Performs the quad in out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 quad_in_out(f32 t) noexcept {
        return t < 0.5f ? 2.0f * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) / 2.0f;
    }

    /// Performs the cubic in operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 cubic_in(f32 t) noexcept { return t * t * t; }

    /// Performs the cubic out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 cubic_out(f32 t) noexcept { return 1.0f - std::pow(1.0f - t, 3.0f); }

    /// Performs the cubic in out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 cubic_in_out(f32 t) noexcept {
        return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
    }

    /// Performs the quart in operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 quart_in(f32 t) noexcept { return t * t * t * t; }

    /// Performs the quart out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 quart_out(f32 t) noexcept { return 1.0f - std::pow(1.0f - t, 4.0f); }

    /// Performs the quart in out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 quart_in_out(f32 t) noexcept {
        return t < 0.5f ? 8.0f * t * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 4.0f) / 2.0f;
    }

    /// Performs the quint in operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 quint_in(f32 t) noexcept { return t * t * t * t * t; }

    /// Performs the quint out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 quint_out(f32 t) noexcept { return 1.0f - std::pow(1.0f - t, 5.0f); }

    /// Performs the quint in out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 quint_in_out(f32 t) noexcept {
        return t < 0.5f ? 16.0f * t * t * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 5.0f) / 2.0f;
    }

    /// Performs the sine in operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 sine_in(f32 t) noexcept { return 1.0f - std::cos(t * std::numbers::pi_v<f32> / 2.0f); }

    /// Performs the sine out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 sine_out(f32 t) noexcept { return std::sin(t * std::numbers::pi_v<f32> / 2.0f); }

    /// Performs the sine in out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 sine_in_out(f32 t) noexcept { return -(std::cos(std::numbers::pi_v<f32> * t) - 1.0f) / 2.0f; }

    /// Performs the expo in operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 expo_in(f32 t) noexcept { return t <= 0.0f ? 0.0f : std::pow(2.0f, 10.0f * t - 10.0f); }

    /// Performs the expo out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 expo_out(f32 t) noexcept { return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t); }

    /// Performs the expo in out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 expo_in_out(f32 t) noexcept {
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        return t < 0.5f ? std::pow(2.0f, 20.0f * t - 10.0f) / 2.0f : (2.0f - std::pow(2.0f, -20.0f * t + 10.0f)) / 2.0f;
    }

    /// Performs the circ in operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 circ_in(f32 t) noexcept { return 1.0f - std::sqrt(1.0f - t * t); }

    /// Performs the circ out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 circ_out(f32 t) noexcept { return std::sqrt(1.0f - (t - 1.0f) * (t - 1.0f)); }

    /// Performs the circ in out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 circ_in_out(f32 t) noexcept {
        return t < 0.5f ? (1.0f - std::sqrt(1.0f - std::pow(2.0f * t, 2.0f))) / 2.0f
                        : (std::sqrt(1.0f - std::pow(-2.0f * t + 2.0f, 2.0f)) + 1.0f) / 2.0f;
    }

    /// Performs the back in operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 back_in(f32 t) noexcept {
        constexpr f32 c1 = 1.70158f;
        constexpr f32 c3 = c1 + 1.0f;
        return c3 * t * t * t - c1 * t * t;
    }

    /// Performs the back out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 back_out(f32 t) noexcept {
        constexpr f32 c1 = 1.70158f;
        constexpr f32 c3 = c1 + 1.0f;
        return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
    }

    /// Performs the back in out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 back_in_out(f32 t) noexcept {
        constexpr f32 c1 = 1.70158f;
        constexpr f32 c2 = c1 * 1.525f;
        return t < 0.5f ? (std::pow(2.0f * t, 2.0f) * ((c2 + 1.0f) * 2.0f * t - c2)) / 2.0f
                        : (std::pow(2.0f * t - 2.0f, 2.0f) * ((c2 + 1.0f) * (t * 2.0f - 2.0f) + c2) + 2.0f) / 2.0f;
    }

    /// Performs the elastic in operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 elastic_in(f32 t) noexcept {
        constexpr f32 c4 = 2.0f * std::numbers::pi_v<f32> / 3.0f;
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        return -std::pow(2.0f, 10.0f * t - 10.0f) * std::sin((t * 10.0f - 10.75f) * c4);
    }

    /// Performs the elastic out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 elastic_out(f32 t) noexcept {
        constexpr f32 c4 = 2.0f * std::numbers::pi_v<f32> / 3.0f;
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
    }

    /// Performs the elastic in out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 elastic_in_out(f32 t) noexcept {
        constexpr f32 c5 = 2.0f * std::numbers::pi_v<f32> / 4.5f;
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        return t < 0.5f ? -(std::pow(2.0f, 20.0f * t - 10.0f) * std::sin((20.0f * t - 11.125f) * c5)) / 2.0f
                        : (std::pow(2.0f, -20.0f * t + 10.0f) * std::sin((20.0f * t - 11.125f) * c5)) / 2.0f + 1.0f;
    }

    /// Performs the bounce out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 bounce_out(f32 t) noexcept { return Detail::bounce_out(t); }

    /// Performs the bounce in operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 bounce_in(f32 t) noexcept { return 1.0f - Detail::bounce_out(1.0f - t); }

    /// Performs the bounce in out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 bounce_in_out(f32 t) noexcept {
        return t < 0.5f ? (1.0f - Detail::bounce_out(1.0f - 2.0f * t)) / 2.0f
                        : (1.0f + Detail::bounce_out(2.0f * t - 1.0f)) / 2.0f;
    }

} // namespace SFT::UI::Easing

namespace SFT::UI::Easing::Detail {

    /// Performs the bounce out operation for `Detail` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 bounce_out(f32 t) noexcept {
        constexpr f32 n1 = 7.5625f;
        constexpr f32 d1 = 2.75f;
        if (t < 1.0f / d1) {
            return n1 * t * t;
        }
        if (t < 2.0f / d1) {
            t -= 1.5f / d1;
            return n1 * t * t + 0.75f;
        }
        if (t < 2.5f / d1) {
            t -= 2.25f / d1;
            return n1 * t * t + 0.9375f;
        }
        t -= 2.625f / d1;
        return n1 * t * t + 0.984375f;
    }

} // namespace SFT::UI::Easing::Detail

