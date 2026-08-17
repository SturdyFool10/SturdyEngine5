#include <UI/src/UI/TextArea.hpp>


namespace SFT::UI {

    TextAreaResult text_area(Context &ctx, const ElementDecl &decl, const TextEditStyle &style,
                                                   TextEditState &state, const TextEditInput &input, f32 delta_seconds,
                                                   const ScrollbarStyle &scrollbar_style, ScrollAreaState &scroll_state,
                                                   const UString &placeholder, bool enabled) {






        const vector<std::pair<usize, usize>> click_paragraphs = Detail::split_paragraphs(state.text());

        const bool is_hovered = enabled && ctx.hovered(decl.id);
        if (enabled && ctx.clicked(decl.id)) {
            state.set_focused(true);
            usize caret_scalar = state.text().size();




            const std::optional<Detail::ParagraphHit> hit =
                Detail::hit_test_paragraphs(ctx, style, state.text(), click_paragraphs, decl.id, ctx.pointer_position());
            if (hit) {
                caret_scalar = hit->scalar;
            }



            if (style.features.pointer_selection && input.shift_held) {
                state.register_click(ctx.pointer_position(),                       false);
                state.set_caret_to(caret_scalar,            true);
            } else if (style.features.pointer_selection) {
                const u8 click_streak = state.register_click(ctx.pointer_position(),                       true);
                if (click_streak >= 3 && hit) {
                    state.select_range(hit->paragraph_start, hit->paragraph_start + hit->paragraph_length);
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
                if (const std::optional<Detail::ParagraphHit> drag_hit = Detail::hit_test_paragraphs(
                        ctx, style, state.text(), click_paragraphs, decl.id, ctx.pointer_position())) {
                    state.set_caret_to(drag_hit->scalar,            true);
                }
            }
        }

        state.update_visual(is_hovered, enabled, style, delta_seconds);

        bool up_pressed = false;
        bool down_pressed = false;
        TextEditInput filtered_input = input;
        filtered_input.keys.clear();
        for (EditKey key : input.keys) {
            if (key == EditKey::Up) {
                up_pressed = true;
            } else if (key == EditKey::Down) {
                down_pressed = true;
            } else {
                filtered_input.keys.push_back(key);
            }
        }





        vector<std::pair<usize, usize>> paragraphs = click_paragraphs;

        TextEditState::ApplyResult apply_result{};
        if (enabled) {
            apply_result = state.apply_input(filtered_input,               true, style.features, style.bindings);
            paragraphs = Detail::split_paragraphs(state.text());
            if (state.focused() && style.features.navigation && (up_pressed || down_pressed)) {
                usize para_index = 0;
                usize local_col = 0;
                for (usize i = 0; i < paragraphs.size(); ++i) {
                    const auto &[pstart, plen] = paragraphs[i];
                    if (state.caret() >= pstart && state.caret() <= pstart + plen) {
                        para_index = i;
                        local_col = state.caret() - pstart;
                    }
                }
                const isize target_index =
                    static_cast<isize>(para_index) + (down_pressed ? 1 : 0) - (up_pressed ? 1 : 0);
                if (target_index < 0) {
                    state.set_caret_to(0, style.features.selection && input.shift_held);
                } else if (target_index >= static_cast<isize>(paragraphs.size())) {
                    state.set_caret_to(state.text().size(), style.features.selection && input.shift_held);
                } else {
                    const auto &[tstart, tlen] = paragraphs[static_cast<usize>(target_index)];
                    state.set_caret_to(tstart + std::min(local_col, tlen), style.features.selection && input.shift_held);
                }
            }
        }


        if (state.focused()) {
            ctx.scroll_into_view(decl.id, Detail::caret_element_id(decl.id));
        }

        ElementDecl styled = decl;
        styled.background_color = state.current_color();
        styled.corner_radius = style.corner_radius;
        styled.border = state.focused() ? style.border_focused : style.border_idle;
        styled.direction = LayoutDirection::TopToBottom;
        styled.clip = ClipConfig{.horizontal = style.features.horizontal_scroll, .vertical = style.features.vertical_scroll};
        if (styled.cursor == CursorIcon::Auto) {
            styled.cursor = enabled ? CursorIcon::Text : CursorIcon::NotAllowed;
        }

        const bool buffer_empty = state.text().empty();
        (void)scroll_area(
            ctx, decl.id, styled,
            [&] {
                ScrollbarStyle resolved = scrollbar_style;
                if (!style.features.scrollbars) {
                    resolved.visibility = ScrollbarVisibility::AlwaysHidden;
                }
                return resolved;
            }(),
            scroll_state, delta_seconds,
            [&](Context &inner) {
                for (const auto &[pstart, plen] : paragraphs) {
                    const UString paragraph_text = state.text().substr(pstart, plen);
                    Detail::render_line(inner, paragraph_text, pstart, style, state, decl.id, buffer_empty ? placeholder : UString{});
                }
            },
            enabled);

        return TextAreaResult{
            .changed = apply_result.changed,
            .focused = state.focused(),
            .caret_bounds = state.focused() ? ctx.element_bounds(Detail::caret_element_id(decl.id)) : std::nullopt,
        };
    }

} // namespace SFT::UI

