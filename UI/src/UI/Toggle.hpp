#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#pragma endregion

#include "Button.hpp"
#include "Context.hpp"
#include "Style.hpp"

// Small boolean/single-choice controls — Checkbox, RadioButton (a "bullet": round, single-choice-
// among-a-group semantics, otherwise identical to Checkbox), Switch — all built on Context's public
// API plus Button.hpp's Oklab color-blend helper, the same "not a Clay primitive" reasoning as
// Button/Masonry. None of these enforce group exclusivity (a radio group's "only one selected" rule)
// themselves — like Button, they report what happened this frame and let the caller own the actual
// state (e.g. one shared `usize selected` the caller sets from whichever radio_button() call
// reports clicked==true), the same way a set of Buttons doesn't know about each other either.
namespace SFT::UI {

    // Shared visual-state colors + transition timing for every control in this file — same shape as
    // ButtonStyle, blended the same way (ColorTransition, Button.hpp) for the same reason
    // (perceptually-even transitions between states by default).
    struct ToggleStyle {
        Color idle{0.2, 0.21, 0.26, 1.0};     // off/unchecked, not hovered
        Color hovered{0.28, 0.3, 0.36, 1.0};  // off/unchecked, hovered
        Color checked{0.35, 0.55, 0.85, 1.0}; // on/checked/selected
        Color disabled{0.2, 0.2, 0.2, 0.5};
        Color mark_color{1.0, 1.0, 1.0, 1.0}; // checkmark glyph / radio dot / switch thumb
        f32 transition_seconds = 0.25f;
        ColorBlendSpace color_space = ColorBlendSpace::Oklab;
        // Reshapes both the color blend above and the progress() scalar below (radio_button()'s dot
        // grow, switch_toggle()'s thumb slide). Defaults to Easing::cubic_in_out (Easing.hpp); pass
        // nullptr for linear progress unreshaped. See EasingFn's own doc comment (Style.hpp).
        EasingFn easing = Easing::cubic_in_out;
    };

    // Persistent per-control animation state, analogous to ButtonState but also tracks a
    // continuous, eased 0..1 `progress` (not just a blended color) — radio_button() uses it to
    // grow/shrink its inner dot, switch_toggle() uses it to slide its thumb. Both ride the same
    // elapsed-time clock (they always retarget together, driven by the same `checked` flip), so a
    // single `elapsed_`/retarget-detection pair drives both — see ColorTransition's own doc comment
    // (Button.hpp) for why retargeting restarts from the current live value rather than snapping.
    class ToggleState {
      public:
        void update(bool checked, bool hovered, bool enabled, const ToggleStyle &style, f32 delta_seconds) noexcept {
            const Color &target_color = !enabled ? style.disabled : checked ? style.checked : hovered ? style.hovered : style.idle;
            const f32 target_progress = checked ? 1.0f : 0.0f;
            if (!initialized_) {
                color_ = target_color;
                progress_ = target_progress;
                start_color_ = target_color;
                start_progress_ = target_progress;
                target_color_ = target_color;
                target_progress_ = target_progress;
                elapsed_ = style.transition_seconds;
                initialized_ = true;
                return;
            }
            if (!(target_color == target_color_) || target_progress != target_progress_) {
                start_color_ = color_;
                start_progress_ = progress_;
                target_color_ = target_color;
                target_progress_ = target_progress;
                elapsed_ = 0.0f;
            }
            elapsed_ += delta_seconds;
            const f32 progress = Detail::eased_progress(elapsed_, style.transition_seconds, style.easing);
            color_ = blend_color(start_color_, target_color_, static_cast<f64>(progress), style.color_space);
            progress_ = std::lerp(start_progress_, target_progress_, progress);
        }

        [[nodiscard]] Color current_color() const noexcept {
            return initialized_ ? color_ : Color{0.0, 0.0, 0.0, 0.0};
        }

        // 0 = fully off/unchecked, 1 = fully on/checked, eased between — see class doc comment for
        // what each control uses it for.
        [[nodiscard]] f32 progress() const noexcept { return progress_; }

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
        [[nodiscard]] inline ToggleResult query_toggle_input(Context &ctx, const UString &id, bool enabled) noexcept {
            return ToggleResult{
                .hovered = enabled && ctx.hovered(id),
                .pressed = enabled && ctx.pointer_down(id),
                .clicked = enabled && ctx.clicked(id),
            };
        }
    } // namespace Detail

    // A square box; filled + a checkmark glyph when `checked`. `decl.id` must be set (same
    // convention as button()). `font_id` must already be registered (Context::register_font()) with
    // a font that has U+2713 (✓), such as Noto Sans or DejaVu Sans.
    [[nodiscard]] inline ToggleResult checkbox(Context &ctx, const ElementDecl &decl, const ToggleStyle &style,
                                               ToggleState &state, f32 delta_seconds, bool checked, FontId font_id,
                                               bool enabled = true) {
        const ToggleResult result = Detail::query_toggle_input(ctx, decl.id, enabled);
        state.update(checked, result.hovered, enabled, style, delta_seconds);

        ElementDecl styled = decl;
        styled.background_color = state.current_color();
        styled.child_alignment = {AlignX::Center, AlignY::Center};
        auto scope = ctx.element(styled);
        (void)scope;
        if (checked) {
            const f32 size = std::min(decl.sizing.width.value, decl.sizing.height.value);
            ctx.text(u8"✓"_ustr,
                    TextStyle{.color = style.mark_color, .font_id = font_id, .font_size = static_cast<u16>(size * 0.75f)});
        }
        return result;
    }

    // A circle; a smaller filled inner dot (grown in via ToggleState::progress()) when `selected`.
    // Visually a "radio button" — named radio_button() rather than bullet_box() since that's what
    // every other UI toolkit calls this control, despite the round "bullet" look.
    [[nodiscard]] inline ToggleResult radio_button(Context &ctx, const ElementDecl &decl, const ToggleStyle &style,
                                                   ToggleState &state, f32 delta_seconds, bool selected,
                                                   bool enabled = true) {
        const ToggleResult result = Detail::query_toggle_input(ctx, decl.id, enabled);
        state.update(selected, result.hovered, enabled, style, delta_seconds);

        const f32 size = std::min(decl.sizing.width.value, decl.sizing.height.value);
        ElementDecl styled = decl;
        styled.background_color = Color{0.0, 0.0, 0.0, 0.0}; // a radio never fills its outer ring
        styled.corner_radius = CornerRadius::all(size * 0.5f);
        styled.border = BorderStyle{.color = state.current_color(), .width = BorderWidth::all(2)};
        styled.child_alignment = {AlignX::Center, AlignY::Center};
        auto scope = ctx.element(styled);
        (void)scope;
        const f32 dot_size = size * 0.5f * state.progress();
        if (dot_size > 0.5f) {
            auto dot = ctx.element(ElementDecl{
                .sizing = {SizingAxis::fixed(dot_size), SizingAxis::fixed(dot_size)},
                .background_color = style.mark_color,
                .corner_radius = CornerRadius::all(dot_size * 0.5f),
            });
            (void)dot;
        }
        return result;
    }

    // A pill-shaped track with a round thumb that slides smoothly between the two ends —
    // ToggleState::progress() drives the thumb's pixel position directly (a fixed-width leading
    // spacer sized `progress * travel_distance`, ordinary Clay layout — no floating/absolute
    // positioning needed for a slide that only ever moves within its own track). `decl.sizing` sets
    // the track's own size; the thumb is sized to fit within it (track height minus padding).
    [[nodiscard]] inline ToggleResult switch_toggle(Context &ctx, const ElementDecl &decl, const ToggleStyle &style,
                                                    ToggleState &state, f32 delta_seconds, bool on,
                                                    bool enabled = true) {
        const ToggleResult result = Detail::query_toggle_input(ctx, decl.id, enabled);
        state.update(on, result.hovered, enabled, style, delta_seconds);

        constexpr f32 kInset = 3.0f;
        const f32 track_width = decl.sizing.width.value;
        const f32 track_height = decl.sizing.height.value;
        const f32 thumb_size = std::max(track_height - kInset * 2.0f, 1.0f);
        const f32 travel = std::max(track_width - kInset * 2.0f - thumb_size, 0.0f);
        const f32 leading_space = travel * state.progress();

        ElementDecl styled = decl;
        styled.background_color = state.current_color();
        styled.corner_radius = CornerRadius::all(track_height * 0.5f);
        styled.padding = Padding::all(static_cast<u16>(kInset));
        styled.child_alignment = {AlignX::Left, AlignY::Center};
        auto track = ctx.element(styled);
        (void)track;
        {
            auto spacer = ctx.element(ElementDecl{
                .sizing = {SizingAxis::fixed(leading_space), SizingAxis::fixed(1.0f)},
            });
            (void)spacer;
        }
        {
            auto thumb = ctx.element(ElementDecl{
                .sizing = {SizingAxis::fixed(thumb_size), SizingAxis::fixed(thumb_size)},
                .background_color = style.mark_color,
                .corner_radius = CornerRadius::all(thumb_size * 0.5f),
            });
            (void)thumb;
        }
        return result;
    }

} // namespace SFT::UI
