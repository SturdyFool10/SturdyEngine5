#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#pragma endregion

#include "Button.hpp"
#include "Context.hpp"
#include "Style.hpp"

/// Small boolean/single-choice controls — Checkbox, RadioButton (a "bullet": round, single-choice-
/// among-a-group semantics, otherwise identical to Checkbox), Switch — all built on Context's public
/// API plus Button.hpp's Oklab color-blend helper, the same "not a Clay primitive" reasoning as
/// Button/Masonry. None of these enforce group exclusivity (a radio group's "only one selected" rule)
/// themselves — like Button, they report what happened this frame and let the caller own the actual
/// state (e.g. one shared `usize selected` the caller sets from whichever radio_button() call
/// reports clicked==true), the same way a set of Buttons doesn't know about each other either.
namespace SFT::UI {

    /// Shared visual-state colors + transition timing for every control in this file — same shape as
    /// ButtonStyle, blended the same way (ColorTransition, Button.hpp) for the same reason
    /// (perceptually-even transitions between states by default).
    struct ToggleStyle {
        Color idle{0.2, 0.21, 0.26, 1.0};
        Color hovered{0.28, 0.3, 0.36, 1.0};
        Color checked{0.35, 0.55, 0.85, 1.0};
        Color disabled{0.2, 0.2, 0.2, 0.5};
        Color mark_color{1.0, 1.0, 1.0, 1.0};
        f32 transition_seconds = 0.25f;
        ColorBlendSpace color_space = ColorBlendSpace::Oklab;
        /// Reshapes both the color blend above and the progress() scalar below (radio_button()'s dot
        /// grow, switch_toggle()'s thumb slide). Defaults to Easing::cubic_in_out (Easing.hpp); pass
        /// nullptr for linear progress unreshaped. See EasingFn's own doc comment (Style.hpp).
        EasingFn easing = Easing::cubic_in_out;
    };

    /// Persistent per-control animation state, analogous to ButtonState but also tracks a
    /// continuous, eased 0..1 `progress` (not just a blended color) — radio_button() uses it to
    /// grow/shrink its inner dot, switch_toggle() uses it to slide its thumb. Both ride the same
    /// elapsed-time clock (they always retarget together, driven by the same `checked` flip), so a
    /// single `elapsed_`/retarget-detection pair drives both — see ColorTransition's own doc comment
    /// (Button.hpp) for why retargeting restarts from the current live value rather than snapping.
    class ToggleState {
      public:
        void update(bool checked, bool hovered, bool enabled, const ToggleStyle &style, f32 delta_seconds) noexcept;

        [[nodiscard]] Color current_color() const noexcept;

        /// 0 = fully off/unchecked, 1 = fully on/checked, eased between — see class doc comment for
        /// what each control uses it for.
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
        [[nodiscard]] ToggleResult query_toggle_input(Context &ctx, const UString &id, bool enabled) noexcept;
    } // namespace Detail

    /// A square box; filled + a checkmark glyph when `checked`. `decl.id` must be set (same
    /// convention as button()). `font_id` must already be registered (Context::register_font()) with
    /// a font that has U+2713 (✓), such as Noto Sans or DejaVu Sans.
    [[nodiscard]] ToggleResult checkbox(Context &ctx, const ElementDecl &decl, const ToggleStyle &style,
                                               ToggleState &state, f32 delta_seconds, bool checked, FontId font_id,
                                               bool enabled = true);

    /// A circle; a smaller filled inner dot (grown in via ToggleState::progress()) when `selected`.
    /// Visually a "radio button" — named radio_button() rather than bullet_box() since that's what
    /// every other UI toolkit calls this control, despite the round "bullet" look.
    [[nodiscard]] ToggleResult radio_button(Context &ctx, const ElementDecl &decl, const ToggleStyle &style,
                                                   ToggleState &state, f32 delta_seconds, bool selected,
                                                   bool enabled = true);

    /// A pill-shaped track with a round thumb that slides smoothly between the two ends —
    /// ToggleState::progress() drives the thumb's pixel position directly (a fixed-width leading
    /// spacer sized `progress * travel_distance`, ordinary Clay layout — no floating/absolute
    /// positioning needed for a slide that only ever moves within its own track). `decl.sizing` sets
    /// the track's own size; the thumb is sized to fit within it (track height minus padding).
    [[nodiscard]] ToggleResult switch_toggle(Context &ctx, const ElementDecl &decl, const ToggleStyle &style,
                                                    ToggleState &state, f32 delta_seconds, bool on,
                                                    bool enabled = true);

} // namespace SFT::UI
