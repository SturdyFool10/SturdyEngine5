#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#pragma endregion

#include "Button.hpp"
#include "Context.hpp"
#include "Style.hpp"


namespace SFT::UI {


    struct ToggleStyle {
        Color idle{0.2, 0.21, 0.26, 1.0};
        Color hovered{0.28, 0.3, 0.36, 1.0};
        Color checked{0.35, 0.55, 0.85, 1.0};
        Color disabled{0.2, 0.2, 0.2, 0.5};
        Color mark_color{1.0, 1.0, 1.0, 1.0};
        f32 transition_seconds = 0.25f;
        ColorBlendSpace color_space = ColorBlendSpace::Oklab;


        EasingFn easing = Easing::cubic_in_out;
    };


    class ToggleState {
      public:
        /// Updates the `ToggleState` state from the supplied values.
        ///
        /// @param checked `checked` value used by the operation.
        /// @param hovered `hovered` value used by the operation.
        /// @param enabled Whether the associated behavior is enabled.
        /// @param style `style` value used by the operation.
        /// @param delta_seconds `delta_seconds` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void update(bool checked, bool hovered, bool enabled, const ToggleStyle &style, f32 delta_seconds) noexcept;

        /// Returns the current or globally available current color value.
        ///
        /// @return Returns the current current color value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Color current_color() const noexcept;


        /// Returns the current or globally available progress value.
        ///
        /// @return Returns the current progress value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 progress() const noexcept;

      private:
        Color color_{};
        Color start_color_{};
        Color target_color_{};
        f32 progress_ = 0.0f;
        f32 start_progress_ = 0.0f;
        f32 target_progress_ = 0.0f;
        f32 elapsed_ = 0.0f;
        bool initialized_ = false;
    };

    struct ToggleResult {
        bool hovered = false;
        bool pressed = false;
        bool clicked = false;
    };

    namespace Detail {
        /// Queries toggle input from the active backend or runtime state.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param id Identifier of the target object or resource.
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ToggleResult query_toggle_input(Context &ctx, const UString &id, bool enabled) noexcept;
    } // namespace Detail


    /// Performs the checkbox operation using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param decl `decl` value used by the operation.
    /// @param style `style` value used by the operation.
    /// @param state `state` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    /// @param checked `checked` value used by the operation.
    /// @param font_id Identifier of the target object or resource.
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] ToggleResult checkbox(Context &ctx, const ElementDecl &decl, const ToggleStyle &style,
                                               ToggleState &state, f32 delta_seconds, bool checked, FontId font_id,
                                               bool enabled = true);


    /// Performs the radio button operation using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param decl `decl` value used by the operation.
    /// @param style `style` value used by the operation.
    /// @param state `state` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    /// @param selected `selected` value used by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] ToggleResult radio_button(Context &ctx, const ElementDecl &decl, const ToggleStyle &style,
                                                   ToggleState &state, f32 delta_seconds, bool selected,
                                                   bool enabled = true);


    /// Performs the switch toggle operation for `UI` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param decl `decl` value used by the operation.
    /// @param style `style` value used by the operation.
    /// @param state `state` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    /// @param on `on` value used by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] ToggleResult switch_toggle(Context &ctx, const ElementDecl &decl, const ToggleStyle &style,
                                                    ToggleState &state, f32 delta_seconds, bool on,
                                                    bool enabled = true);

} // namespace SFT::UI
