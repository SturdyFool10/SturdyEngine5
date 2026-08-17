#include <UI/src/UI/Button.hpp>


namespace SFT::UI::Detail {

    f32 eased_progress(f32 elapsed_seconds, f32 duration_seconds, EasingFn easing) noexcept {
        const f32 duration = std::max(duration_seconds, 1.0e-4f);
        const f32 raw = std::clamp(elapsed_seconds / duration, 0.0f, 1.0f);
        return easing != nullptr ? easing(raw) : raw;
    }

} // namespace SFT::UI::Detail

namespace SFT::UI {

    Color blend_color(const Color &current, const Color &target, f64 t,
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

    void ColorTransition::update(const Color &target, f32 delta_seconds, f32 transition_seconds,
                ColorBlendSpace space, EasingFn easing) noexcept {
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

    Color ColorTransition::current() const noexcept { return initialized_ ? current_ : Color{0.0, 0.0, 0.0, 0.0}; }

    void ButtonState::update(bool hovered, bool pressed, bool enabled, const ButtonStyle &style, f32 delta_seconds) noexcept {
        const Color &target = !enabled ? style.disabled : pressed ? style.pressed : hovered ? style.hovered : style.idle;
        color_.update(target, delta_seconds, style.transition_seconds, style.color_space, style.easing);
    }

    Color ButtonState::current_color() const noexcept { return color_.current(); }

    ButtonResult button(Context &ctx, const ElementDecl &decl, const ButtonStyle &style,
                                             ButtonState &state, f32 delta_seconds, bool enabled) {
        const bool is_hovered = enabled && ctx.hovered(decl.id);
        const bool is_pressed = enabled && ctx.pointer_down(decl.id);
        const bool is_clicked = enabled && ctx.clicked(decl.id);
        state.update(is_hovered, is_pressed, enabled, style, delta_seconds);

        ElementDecl styled = decl;
        styled.background_color = state.current_color();
        styled.corner_radius = style.corner_radius;
        styled.border = style.border;




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

