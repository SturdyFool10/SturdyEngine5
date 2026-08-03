#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <cmath>
#pragma endregion

#include "Context.hpp"
#include "Easing.hpp"
#include "Style.hpp"

// A small opinionated widget built entirely on top of UI::Context's own public API (element()/
// hovered()/pointer_down()/clicked()) — not a Clay primitive, so it doesn't live as a Context method
// (see Style.hpp's own doc comment on why this wrapper stays a 1:1 Clay mirror). Anything here could
// equally be written by an app itself; it exists to avoid every button call site re-deriving the
// same hover/press color-blend boilerplate.
namespace SFT::UI {

    namespace Detail {
        // The shared elapsed/duration progress every transition in this file (color blends,
        // ToggleState's progress scalar) is built from: how far through a `duration_seconds`-long
        // transition `elapsed_seconds` is, as a 0..1 fraction (clamped, so it holds at the target
        // once the transition finishes rather than overshooting past it), reshaped by `easing` if
        // set (see EasingFn's own doc comment in Style.hpp). Deliberately *not* a per-frame
        // exponential-decay fraction: feeding a curve like Easing::cubic_in_out — designed to shape
        // a full 0..1 sweep — a tiny per-frame increment instead crushes it far below its nominal
        // rate (cubic's t^3 near zero), making every transition many times slower than
        // `duration_seconds` actually says. Tracking real elapsed time against the target sidesteps
        // that entirely and makes `transition_seconds` an honest duration for any easing curve.
        [[nodiscard]] inline f32 eased_progress(f32 elapsed_seconds, f32 duration_seconds, EasingFn easing) noexcept {
            const f32 duration = std::max(duration_seconds, 1.0e-4f);
            const f32 raw = std::clamp(elapsed_seconds / duration, 0.0f, 1.0f);
            return easing != nullptr ? easing(raw) : raw;
        }

        template <Foundation::Color::ColorSpace Space>
        [[nodiscard]] inline Color blend_in_space(const Color &current, const Color &target, f64 t) noexcept {
            const auto current_space = Foundation::Color::convert_to<Space>(current);
            const auto target_space = Foundation::Color::convert_to<Space>(target);
            return Foundation::Color::convert_to<Color>(Foundation::Color::lerp(current_space, target_space, t));
        }
    } // namespace Detail

    // Interpolates `current` toward `target` by fraction `t` (0..1, extrapolates past that range
    // untouched — see EasingFn's doc comment) through whichever Foundation::Color space `space`
    // selects, converting back to UI::Color (Srgb) for the result. See ColorBlendSpace's own doc
    // comment (Style.hpp) for why Oklab is the default and when you'd reach for another space.
    [[nodiscard]] inline Color blend_color(const Color &current, const Color &target, f64 t,
                                           ColorBlendSpace space) noexcept {
        switch (space) {
            case ColorBlendSpace::Oklch: return Detail::blend_in_space<Foundation::Color::Oklch>(current, target, t);
            case ColorBlendSpace::Linear: return Detail::blend_in_space<Foundation::Color::Linear>(current, target, t);
            case ColorBlendSpace::Srgb: return Detail::blend_in_space<Foundation::Color::Srgb>(current, target, t);
            case ColorBlendSpace::Hsl: return Detail::blend_in_space<Foundation::Color::Hsl>(current, target, t);
            case ColorBlendSpace::Oklab:
            default: return Detail::blend_in_space<Foundation::Color::Oklab>(current, target, t);
        }
    }

    // A single animated color: eases from wherever it currently is to a new target over
    // `transition_seconds`, through `color_space`, reshaped by `easing` — the engine ButtonState/
    // ToggleState/TextEditState's own color blending is built from. Factored out so any other
    // stateful/animated color (a masonry tile's hover glow, a tab's selected-underline color, ...)
    // can reuse it without needing a Button.
    //
    // Retargeting (update() called with a different `target` than last time, e.g. the pointer
    // enters or leaves mid-transition) restarts the transition from current()'s own live value, not
    // from scratch or from the old target — the same behavior CSS transitions and every other UI
    // toolkit's tweening has, and it's what keeps rapid hover flicker looking continuous instead of
    // snapping or reversing through a discontinuity.
    class ColorTransition {
      public:
        void update(const Color &target, f32 delta_seconds, f32 transition_seconds,
                    ColorBlendSpace space = ColorBlendSpace::Oklab, EasingFn easing = nullptr) noexcept {
            if (!initialized_) {
                current_ = target;
                start_ = target;
                target_ = target;
                elapsed_ = transition_seconds;
                initialized_ = true;
                return;
            }
            if (!(target == target_)) {
                start_ = current_;
                target_ = target;
                elapsed_ = 0.0f;
            }
            elapsed_ += delta_seconds;
            const f32 progress = Detail::eased_progress(elapsed_, transition_seconds, easing);
            current_ = blend_color(start_, target_, static_cast<f64>(progress), space);
        }

        [[nodiscard]] Color current() const noexcept { return initialized_ ? current_ : Color{0.0, 0.0, 0.0, 0.0}; }

      private:
        Color current_{};
        Color start_{};
        Color target_{};
        f32 elapsed_ = 0.0f;
        bool initialized_ = false;
    };

    // Visual state colors a button transitions between as the pointer hovers/presses it, plus the
    // static border/corner-radius it's drawn with (unaffected by state). Colors are UI::Color
    // (Foundation::Color::Srgb) — author however's convenient; the hover/press blend itself happens
    // in `color_space` (see ColorBlendSpace's own doc comment in Style.hpp for why Oklab is the
    // default), reshaped by `easing` if set (see EasingFn's own doc comment).
    struct ButtonStyle {
        Color idle{0.25, 0.27, 0.32, 1.0};
        Color hovered{0.32, 0.35, 0.42, 1.0};
        Color pressed{0.18, 0.19, 0.24, 1.0};
        Color disabled{0.2, 0.2, 0.2, 0.5};
        CornerRadius corner_radius{};
        BorderStyle border{};
        // Seconds for a state change (idle->hovered, hovered->pressed, ...) to finish blending —
        // an honest duration regardless of `easing` (see ColorTransition's own doc comment).
        f32 transition_seconds = 0.25f;
        ColorBlendSpace color_space = ColorBlendSpace::Oklab;
        // Defaults to Easing::cubic_in_out (Easing.hpp) — a gentle accelerate-then-decelerate feel
        // for hover/press transitions. Pass nullptr for the raw exponential-decay curve unreshaped,
        // or any other Easing::* function (or your own EasingFn).
        EasingFn easing = Easing::cubic_in_out;
    };

    // Persistent per-button animation state — one instance per logical button, kept alive across
    // frames by the caller. Immediate-mode UI has no per-widget storage of its own (Context rebuilds
    // its whole tree every frame), so this is the caller's job, just like any other cross-frame
    // application state that lives outside Context.
    class ButtonState {
      public:
        // Advances the blend toward whichever of style's four colors `hovered`/`pressed`/`enabled`
        // select, by `delta_seconds`. button() below calls this for you; call it directly only if
        // building a custom interactive element that wants the same hover/press color blend without
        // going through button()'s own element()/layout conventions.
        void update(bool hovered, bool pressed, bool enabled, const ButtonStyle &style, f32 delta_seconds) noexcept {
            const Color &target = !enabled ? style.disabled : pressed ? style.pressed : hovered ? style.hovered : style.idle;
            color_.update(target, delta_seconds, style.transition_seconds, style.color_space, style.easing);
        }

        [[nodiscard]] Color current_color() const noexcept { return color_.current(); }

      private:
        ColorTransition color_{};
    };

    // button()'s result: hover/press/click state for this frame (queried the same one-frame-stale
    // way hovered()/clicked() themselves are), plus the open ElementScope for the button's own
    // element — fill it with whatever the button's label is (Context::text(), an image(), several
    // elements laid out with their own child_alignment, anything). `decl`'s own sizing/padding/
    // child_alignment/direction control both the button's size and where that content sits inside
    // it, exactly like any other container — button() doesn't invent a separate placement API.
    struct ButtonResult {
        bool hovered = false;
        bool pressed = false;
        bool clicked = false;
        ElementScope scope;
    };

    // `decl.id` must be set (see ElementDecl::id's own doc comment) — it's how this frame's hover/
    // press/click state is queried, the same id a caller would otherwise pass to
    // Context::hovered()/clicked() by hand. `decl.background_color`/`corner_radius`/`border` are
    // overwritten from `style`/the current blended state; set every other ElementDecl field (sizing,
    // padding, child_gap, child_alignment, direction) as normal to control the button's size and
    // label placement.
    [[nodiscard]] inline ButtonResult button(Context &ctx, const ElementDecl &decl, const ButtonStyle &style,
                                             ButtonState &state, f32 delta_seconds, bool enabled = true) {
        const bool is_hovered = enabled && ctx.hovered(decl.id);
        const bool is_pressed = enabled && ctx.pointer_down(decl.id);
        const bool is_clicked = enabled && ctx.clicked(decl.id);
        state.update(is_hovered, is_pressed, enabled, style, delta_seconds);

        ElementDecl styled = decl;
        styled.background_color = state.current_color();
        styled.corner_radius = style.corner_radius;
        styled.border = style.border;
        // Web-like default: an interactive control shows the pointer/hand cursor unless the caller
        // already asked for something specific (CursorIcon::Auto is the only value this overrides —
        // see ElementDecl::cursor's own doc comment, Style.hpp); a disabled one shows "not allowed"
        // instead, since clicking it is a no-op.
        if (styled.cursor == CursorIcon::Auto) {
            styled.cursor = enabled ? CursorIcon::Pointer : CursorIcon::NotAllowed;
        }

        return ButtonResult{
            .hovered = is_hovered,
            .pressed = is_pressed,
            .clicked = is_clicked,
            .scope = ctx.element(styled),
        };
    }

} // namespace SFT::UI
