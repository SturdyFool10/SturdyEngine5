#pragma once

#include <Foundation/src/Foundation.hpp>

#include "Context.hpp"
#include "Style.hpp"
#include "TextEdit.hpp"

// Single-line text input, built on TextEdit.hpp's shared buffer/caret/selection engine — same
// "not a Clay primitive" reasoning as Button/Toggle/Dropdown.
namespace SFT::UI {

    struct TextInputResult {
        bool changed = false;
        // True the one frame EditKey::Enter was pressed while focused (see TextEditState::
        // apply_input()'s own doc comment — single-line inputs treat Enter as "submit", not as a
        // literal newline).
        bool submitted = false;
        bool focused = false;
        // The caret's own on-screen rectangle (one frame stale, same as every other
        // Context::element_bounds() read — see this field's own use in the function body), present
        // only while focused. Feed it to Platform::Windowing::Window::set_text_input_area() (via
        // whatever request-queue bridge the embedding app uses — see Engine::WindowRequests::
        // set_text_input_area() for this engine's own) so an IME's composition candidate window
        // anchors to the caret instead of a default/wrong location.
        std::optional<ElementBounds> caret_bounds;
    };

    // `decl.id` must be set (same convention as button()) — it's the click-to-focus hit-test id.
    // Set `decl.sizing`/`.padding`/`.child_alignment` (typically `{Left, Center}` for a
    // single-line box) yourself, same as every other widget here — text_input() only overwrites
    // `background_color`/`corner_radius`/`border`, driven by the current hover/focus/disabled
    // state.
    //
    // Click-to-focus *and* click-to-position: clicking the input focuses it and places the caret at
    // the clicked character, found via Context::hit_test_text_byte_offset() against the single
    // line's own on-screen bounds (Detail::hit_test_line_scalar(), TextEdit.hpp) — falls back to
    // the end of the text only if that line wasn't rendered last frame yet (e.g. the very first
    // frame this input ever appears, before Context has any committed bounds for it to hit-test
    // against). Clicking anywhere else in the UI (via Context::clicked_outside()) drops focus.
    [[nodiscard]] inline TextInputResult text_input(Context &ctx, const ElementDecl &decl, const TextEditStyle &style,
                                                     TextEditState &state, const TextEditInput &input,
                                                     f32 delta_seconds, const UString &placeholder = {},
                                                     bool enabled = true) {
        const bool is_hovered = enabled && ctx.hovered(decl.id);
        if (enabled && ctx.clicked(decl.id)) {
            state.set_focused(true);
            usize caret_scalar = state.text().size();
            if (const std::optional<ElementBounds> line_bounds = ctx.element_bounds(Detail::line_element_id(decl.id, 0))) {
                const f32 local_x = ctx.pointer_position().x - line_bounds->position.x;
                caret_scalar = Detail::hit_test_line_scalar(ctx, style, state.text(), local_x);
            }
            // Shift+click always plainly extends the selection — never itself a leg of a multi-
            // click gesture (see TextEditState::register_click()'s own doc comment). Otherwise a
            // click's streak (double/triple, timed independently of any OS multi-click setting —
            // same self-timed choice as caret_blink_on()) selects the word / the whole line
            // (single-line, so "the line" is the whole buffer) instead of just placing the caret.
            if (style.features.pointer_selection && input.shift_held) {
                state.register_click(ctx.pointer_position(), /*allow_multi_click=*/false);
                state.set_caret_to(caret_scalar, /*extend=*/true);
            } else if (style.features.pointer_selection) {
                const u8 click_streak = state.register_click(ctx.pointer_position(), /*allow_multi_click=*/true);
                if (click_streak >= 3) {
                    state.select_all();
                } else if (click_streak == 2) {
                    state.select_word_at(caret_scalar);
                } else {
                    state.set_caret_to(caret_scalar, /*extend=*/false);
                }
            } else {
                state.set_caret_to(caret_scalar, /*extend=*/false);
            }
            if (style.features.pointer_selection) {
                (void)ctx.try_capture_pointer(decl.id);
            }
        } else if (ctx.clicked_outside(decl.id)) {
            state.set_focused(false);
        }

        // Drag-to-select: extends the selection by character while the initial click's pointer
        // capture is held, independent of whether the pointer leaves the box's own bounds — same
        // capture-then-track-every-frame pattern ScrollArea.hpp's thumb drag uses (including its
        // release condition). v1 simplification: always character-granularity, even after a
        // double/triple-click, and no auto-scroll while dragging past the visible edge.
        if (enabled && style.features.pointer_selection && ctx.has_pointer_capture(decl.id)) {
            if (ctx.pointer_cancelled_this_frame() || (!ctx.pointer_is_down() && !ctx.pointer_pressed_this_frame())) {
                ctx.release_pointer(decl.id);
            } else if (ctx.pointer_is_down()) {
                if (const std::optional<ElementBounds> line_bounds = ctx.element_bounds(Detail::line_element_id(decl.id, 0))) {
                    const f32 local_x = ctx.pointer_position().x - line_bounds->position.x;
                    const usize drag_scalar = Detail::hit_test_line_scalar(ctx, style, state.text(), local_x);
                    state.set_caret_to(drag_scalar, /*extend=*/true);
                }
            }
        }

        state.update_visual(is_hovered, enabled, style, delta_seconds);
        const TextEditState::ApplyResult apply_result =
            enabled ? state.apply_input(input, /*multiline=*/false, style.features, style.bindings) : TextEditState::ApplyResult{};

        // Must run before this frame's box/row are declared — it nudges last frame's committed
        // scroll offset, which Context::element() reads (via Clay_GetScrollOffset()) the moment
        // `box` below opens, so the correction is visible this same frame instead of one frame
        // late. See Context::scroll_into_view()'s own doc comment.
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
