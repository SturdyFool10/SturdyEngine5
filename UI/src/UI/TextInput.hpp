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
            state.set_caret_to(caret_scalar, false);
        } else if (ctx.clicked_outside(decl.id)) {
            state.set_focused(false);
        }

        state.update_visual(is_hovered, enabled, style, delta_seconds);
        const TextEditState::ApplyResult apply_result =
            enabled ? state.apply_input(input, /*multiline=*/false) : TextEditState::ApplyResult{};

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
        styled.clip = ClipConfig{.horizontal = true};
        auto box = ctx.element(styled);
        (void)box;

        Detail::render_line(ctx, state.text(), 0, style, state, decl.id, placeholder);

        return TextInputResult{.changed = apply_result.changed, .submitted = apply_result.submitted, .focused = state.focused()};
    }

} // namespace SFT::UI
