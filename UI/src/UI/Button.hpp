#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <cmath>
#pragma endregion

#include "Context.hpp"
#include "Easing.hpp"
#include "Style.hpp"


namespace SFT::UI {

    namespace Detail {


        /// Performs the eased progress operation using the supplied arguments.
        ///
        /// @param elapsed_seconds `elapsed_seconds` value used by the operation.
        /// @param duration_seconds `duration_seconds` value used by the operation.
        /// @param easing `easing` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 eased_progress(f32 elapsed_seconds, f32 duration_seconds, EasingFn easing) noexcept;

        /// Returns the current or globally available blend in space value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        template <Foundation::Color::ColorSpace Space>
        [[nodiscard]] inline Color blend_in_space(const Color &current, const Color &target, f64 t) noexcept {
            const auto current_space = Foundation::Color::convert_to<Space>(current);
            const auto target_space = Foundation::Color::convert_to<Space>(target);
            return Foundation::Color::convert_to<Color>(Foundation::Color::lerp(current_space, target_space, t));
        }
    } // namespace Detail


    /// Performs the blend color operation using the supplied arguments.
    ///
    /// @param current `current` value used by the operation.
    /// @param target `target` value used by the operation.
    /// @param t `t` value used by the operation.
    /// @param space `space` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] Color blend_color(const Color &current, const Color &target, f64 t,
                                           ColorBlendSpace space) noexcept;


    class ColorTransition {
      public:
        /// Updates the `ColorTransition` state from the supplied values.
        ///
        /// @param target `target` value used by the operation.
        /// @param delta_seconds `delta_seconds` value used by the operation.
        /// @param transition_seconds `transition_seconds` value used by the operation.
        /// @param space `space` value used by the operation.
        /// @param easing `easing` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void update(const Color &target, f32 delta_seconds, f32 transition_seconds,
                    ColorBlendSpace space = ColorBlendSpace::Oklab, EasingFn easing = nullptr) noexcept;

        /// Returns the current or globally available current value.
        ///
        /// @return Returns the current current value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Color current() const noexcept;

      private:
        Color current_{};
        Color start_{};
        Color target_{};
        f32 elapsed_ = 0.0f;
        bool initialized_ = false;
    };


    struct ButtonStyle {
        Color idle{0.25, 0.27, 0.32, 1.0};
        Color hovered{0.32, 0.35, 0.42, 1.0};
        Color pressed{0.18, 0.19, 0.24, 1.0};
        Color disabled{0.2, 0.2, 0.2, 0.5};
        CornerRadius corner_radius{};
        BorderStyle border{};


        f32 transition_seconds = 0.25f;
        ColorBlendSpace color_space = ColorBlendSpace::Oklab;


        EasingFn easing = Easing::cubic_in_out;
    };


    class ButtonState {
      public:


        /// Updates the `ButtonState` state from the supplied values.
        ///
        /// @param hovered `hovered` value used by the operation.
        /// @param pressed `pressed` value used by the operation.
        /// @param enabled Whether the associated behavior is enabled.
        /// @param style `style` value used by the operation.
        /// @param delta_seconds `delta_seconds` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void update(bool hovered, bool pressed, bool enabled, const ButtonStyle &style, f32 delta_seconds) noexcept;

        /// Returns the current or globally available current color value.
        ///
        /// @return Returns the current current color value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Color current_color() const noexcept;

      private:
        ColorTransition color_{};
    };


    struct ButtonResult {
        bool hovered = false;
        bool pressed = false;
        bool clicked = false;
        ElementScope scope;
    };


    /// Performs the button operation for `UI` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param decl `decl` value used by the operation.
    /// @param style `style` value used by the operation.
    /// @param state `state` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] ButtonResult button(Context &ctx, const ElementDecl &decl, const ButtonStyle &style,
                                             ButtonState &state, f32 delta_seconds, bool enabled = true);

} // namespace SFT::UI
