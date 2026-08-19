#include <Renderer/UI/TextInput.hpp>


namespace SFT::UI {

    /// Performs the text input operation for `UI` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param decl `decl` value used by the operation.
    /// @param style `style` value used by the operation.
    /// @param state `state` value used by the operation.
    /// @param input `input` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    /// @param placeholder `placeholder` value used by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    TextInputResult text_input(Context &ctx, const ElementDecl &decl, const TextEditStyle &style,
                                                     TextEditState &state, const TextEditInput &input,
                                                     f32 delta_seconds, const UString &placeholder,
                                                     bool enabled) {
        const bool is_hovered = enabled && ctx.hovered(decl.id);
        if (enabled && ctx.clicked(decl.id)) {
            state.set_focused(true);
            usize caret_scalar = state.text().size();
            if (const std::optional<ElementBounds> line_bounds = ctx.element_bounds(Detail::line_element_id(decl.id, 0))) {
                const f32 local_x = ctx.pointer_position().x - line_bounds->position.x;
                caret_scalar = Detail::hit_test_line_scalar(ctx, style, state.text(), local_x);
            }


            if (style.features.pointer_selection && input.shift_held) {
                state.register_click(ctx.pointer_position(),                       false);
                state.set_caret_to(caret_scalar,            true);
            } else if (style.features.pointer_selection) {
                const u8 click_streak = state.register_click(ctx.pointer_position(),                       true);
                if (click_streak >= 3) {
                    state.select_all();
                } else if (click_streak == 2) {
                    state.select_word_at(caret_scalar);
                } else {
                    state.set_caret_to(caret_scalar,            false);
                }
            } else {
                state.set_caret_to(caret_scalar,            false);
            }
            if (style.features.pointer_selection) {
                (void)ctx.try_capture_pointer(decl.id);
            }
        } else if (ctx.clicked_outside(decl.id)) {
            state.set_focused(false);
        }


        if (enabled && style.features.pointer_selection && ctx.has_pointer_capture(decl.id)) {
            if (ctx.pointer_cancelled_this_frame() || (!ctx.pointer_is_down() && !ctx.pointer_pressed_this_frame())) {
                ctx.release_pointer(decl.id);
            } else if (ctx.pointer_is_down()) {
                if (const std::optional<ElementBounds> line_bounds = ctx.element_bounds(Detail::line_element_id(decl.id, 0))) {
                    const f32 local_x = ctx.pointer_position().x - line_bounds->position.x;
                    const usize drag_scalar = Detail::hit_test_line_scalar(ctx, style, state.text(), local_x);
                    state.set_caret_to(drag_scalar,            true);
                }
            }
        }

        state.update_visual(is_hovered, enabled, style, delta_seconds);
        const TextEditState::ApplyResult apply_result =
            enabled ? state.apply_input(input,               false, style.features, style.bindings) : TextEditState::ApplyResult{};


        if (state.focused()) {
            ctx.scroll_into_view(decl.id, Detail::caret_element_id(decl.id));
        }

        ElementDecl styled = decl;
        styled.background_color = state.current_color();
        styled.corner_radius = style.corner_radius;
        styled.border = state.focused() ? style.border_focused : style.border_idle;
        styled.clip = ClipConfig{.horizontal = style.features.horizontal_scroll};
        if (styled.cursor == CursorIcon::Auto) {
            styled.cursor = enabled ? CursorIcon::Text : CursorIcon::NotAllowed;
        }
        auto box = ctx.element(styled);
        (void)box;

        Detail::render_line(ctx, state.text(), 0, style, state, decl.id, placeholder);

        return TextInputResult{
            .changed = apply_result.changed,
            .submitted = apply_result.submitted,
            .focused = state.focused(),
            .caret_bounds = state.focused() ? ctx.element_bounds(Detail::caret_element_id(decl.id)) : std::nullopt,
        };
    }

} // namespace SFT::UI

