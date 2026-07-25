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
    // Click-to-focus only for v1 — clicking anywhere on the input focuses it and places the caret
    // at the end of the existing text, rather than at the clicked character. Precise click-to-
    // position needs either per-character hit-test ids (expensive for long text) or a from-scratch
    // glyph-shaping/measurement pass outside Clay's own text layout; neither seemed worth it before
    // a caller actually needs it — arrow keys/Home/End/word-jump (Ctrl+Left/Right, via `input.
    // word_modifier_held`) already reach any position once focused. Clicking anywhere else in the
    // UI (via Context::clicked_outside()) drops focus.
    [[nodiscard]] inline TextInputResult text_input(Context &ctx, const ElementDecl &decl, const TextEditStyle &style,
                                                     TextEditState &state, const TextEditInput &input,
                                                     f32 delta_seconds, const UString &placeholder = {},
                                                     bool enabled = true) {
        const bool is_hovered = enabled && ctx.hovered(decl.id);
        if (enabled && ctx.clicked(decl.id)) {
            state.set_focused(true);
            state.set_caret_to(state.text().size(), false);
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
