#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <cmath>
#include <numbers>
#pragma endregion

#include <Renderer/UI/Style.hpp>


namespace SFT::UI::Easing {

    /// Performs the linear operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 linear(f32 t) noexcept;

    /// Performs the quad in operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 quad_in(f32 t) noexcept;
    /// Performs the quad out operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 quad_out(f32 t) noexcept;
    /// Performs the quad in out operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 quad_in_out(f32 t) noexcept;

    /// Performs the cubic in operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 cubic_in(f32 t) noexcept;
    /// Performs the cubic out operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 cubic_out(f32 t) noexcept;
    /// Performs the cubic in out operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 cubic_in_out(f32 t) noexcept;

    /// Performs the quart in operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 quart_in(f32 t) noexcept;
    /// Performs the quart out operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 quart_out(f32 t) noexcept;
    /// Performs the quart in out operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 quart_in_out(f32 t) noexcept;

    /// Performs the quint in operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 quint_in(f32 t) noexcept;
    /// Performs the quint out operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 quint_out(f32 t) noexcept;
    /// Performs the quint in out operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 quint_in_out(f32 t) noexcept;

    /// Performs the sine in operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 sine_in(f32 t) noexcept;
    /// Performs the sine out operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 sine_out(f32 t) noexcept;
    /// Performs the sine in out operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 sine_in_out(f32 t) noexcept;

    /// Performs the expo in operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 expo_in(f32 t) noexcept;
    /// Performs the expo out operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 expo_out(f32 t) noexcept;
    /// Performs the expo in out operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 expo_in_out(f32 t) noexcept;

    /// Performs the circ in operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 circ_in(f32 t) noexcept;
    /// Performs the circ out operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 circ_out(f32 t) noexcept;
    /// Performs the circ in out operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 circ_in_out(f32 t) noexcept;


    /// Performs the back in operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 back_in(f32 t) noexcept;
    /// Performs the back out operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 back_out(f32 t) noexcept;
    /// Performs the back in out operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 back_in_out(f32 t) noexcept;


    /// Performs the elastic in operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 elastic_in(f32 t) noexcept;
    /// Performs the elastic out operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 elastic_out(f32 t) noexcept;
    /// Performs the elastic in out operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 elastic_in_out(f32 t) noexcept;

    namespace Detail {
        /// Performs the bounce out operation using the supplied arguments.
        ///
        /// @param t `t` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 bounce_out(f32 t) noexcept;
    } // namespace Detail

    /// Performs the bounce out operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 bounce_out(f32 t) noexcept;
    /// Performs the bounce in operation using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 bounce_in(f32 t) noexcept;
    /// Performs the bounce in out operation for `Easing` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 bounce_in_out(f32 t) noexcept;

} // namespace SFT::UI::Easing
