#include <Renderer/UI/Toggle.hpp>


namespace SFT::UI {

    /// Updates the `UI` state from the supplied values.
    ///
    /// @param checked `checked` value used by the operation.
    /// @param hovered `hovered` value used by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    /// @param style `style` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void ToggleState::update(bool checked, bool hovered, bool enabled, const ToggleStyle &style, f32 delta_seconds) noexcept {
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

    /// Returns the current or globally available current color value.
    ///
    /// @return Returns the current current color value.
    /// @note This function does not throw exceptions.
    Color ToggleState::current_color() const noexcept {
        return initialized_ ? color_ : Color{0.0, 0.0, 0.0, 0.0};
    }

    /// Returns the current or globally available progress value.
    ///
    /// @return Returns the current progress value.
    /// @note This function does not throw exceptions.
    f32 ToggleState::progress() const noexcept { return progress_; }

    /// Performs the checkbox operation for `UI` using the supplied arguments.
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
    ToggleResult checkbox(Context &ctx, const ElementDecl &decl, const ToggleStyle &style,
                                               ToggleState &state, f32 delta_seconds, bool checked, FontId font_id,
                                               bool enabled) {
        const ToggleResult result = Detail::query_toggle_input(ctx, decl.id, enabled);
        state.update(checked, result.hovered, enabled, style, delta_seconds);

        ElementDecl styled = decl;
        styled.background_color = state.current_color();
        styled.child_alignment = {AlignX::Center, AlignY::Center};
        if (styled.cursor == CursorIcon::Auto) {
            styled.cursor = enabled ? CursorIcon::Pointer : CursorIcon::NotAllowed;
        }
        auto scope = ctx.element(styled);
        (void)scope;
        if (checked) {
            const f32 size = std::min(decl.sizing.width.value, decl.sizing.height.value);
            ctx.text(u8"✓"_ustr,
                    TextStyle{.color = style.mark_color, .font_id = font_id, .font_size = static_cast<u16>(size * 0.75f)});
        }
        return result;
    }

    /// Performs the radio button operation for `UI` using the supplied arguments.
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
    ToggleResult radio_button(Context &ctx, const ElementDecl &decl, const ToggleStyle &style,
                                                   ToggleState &state, f32 delta_seconds, bool selected,
                                                   bool enabled) {
        const ToggleResult result = Detail::query_toggle_input(ctx, decl.id, enabled);
        state.update(selected, result.hovered, enabled, style, delta_seconds);

        const f32 size = std::min(decl.sizing.width.value, decl.sizing.height.value);
        ElementDecl styled = decl;
        styled.background_color = Color{0.0, 0.0, 0.0, 0.0};
        styled.corner_radius = CornerRadius::all(size * 0.5f);
        styled.border = BorderStyle{.color = state.current_color(), .width = BorderWidth::all(2)};
        styled.child_alignment = {AlignX::Center, AlignY::Center};
        if (styled.cursor == CursorIcon::Auto) {
            styled.cursor = enabled ? CursorIcon::Pointer : CursorIcon::NotAllowed;
        }
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
    ToggleResult switch_toggle(Context &ctx, const ElementDecl &decl, const ToggleStyle &style,
                                                    ToggleState &state, f32 delta_seconds, bool on,
                                                    bool enabled) {
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
        if (styled.cursor == CursorIcon::Auto) {
            styled.cursor = enabled ? CursorIcon::Pointer : CursorIcon::NotAllowed;
        }
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

namespace SFT::UI::Detail {

    /// Queries toggle input from the active backend or runtime state.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param id Identifier of the target object or resource.
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    ToggleResult query_toggle_input(Context &ctx, const UString &id, bool enabled) noexcept {
        return ToggleResult{
            .hovered = enabled && ctx.hovered(id),
            .pressed = enabled && ctx.pointer_down(id),
            .clicked = enabled && ctx.clicked(id),
        };
    }

} // namespace SFT::UI::Detail

