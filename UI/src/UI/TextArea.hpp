#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <utility>
#include <vector>
#pragma endregion

#include "Context.hpp"
#include "Style.hpp"
#include "TextEdit.hpp"

using std::vector;

// Multi-line text input, built on the same TextEdit.hpp engine text_input() (TextInput.hpp) uses —
// Enter inserts a newline instead of submitting, the box scrolls both ways (or wraps, see
// TextEditStyle::wrap_lines), and EditKey::Up/Down move the caret vertically between paragraphs.
namespace SFT::UI {

    struct TextAreaResult {
        bool changed = false;
        bool focused = false;
    };

    // `decl.id` must be set (see text_input()'s own doc comment — same click-to-focus/
    // click_outside-to-defocus convention). `decl.clip` is overwritten to
    // `{.horizontal = true, .vertical = true}` regardless of what's passed in — a text area is a
    // scroll container by definition.
    //
    // Two v1 simplifications, both documented in more depth on their own pieces:
    // - Click-to-focus, not click-to-position (TextEditState/text_input()'s own doc comment).
    // - EditKey::Up/Down move by *paragraph* (hard '\n'-delimited line), not by wrapped visual
    //   line — a very long paragraph that word-wraps across several visual lines only gets one
    //   Up/Down stop for the whole paragraph, not one per wrapped line. True visual-line
    //   granularity needs the same glyph-shaping pass click-to-position would (Clay's own word-wrap
    //   result isn't queryable after the fact), which is out of scope for the same reasons.
    //
    // Lines don't word-wrap by default (TextEditStyle::wrap_lines = false) — a line wider than the
    // box just scrolls horizontally, the same convention a code editor uses (and this is where a
    // syntax-highlighted line's Highlighter spans nearly always land: see Detail::render_line's own
    // doc comment for why a line split into multiple runs can't wrap coherently across them even
    // with wrap_lines = true).
    [[nodiscard]] inline TextAreaResult text_area(Context &ctx, const ElementDecl &decl, const TextEditStyle &style,
                                                   TextEditState &state, const TextEditInput &input, f32 delta_seconds,
                                                   const UString &placeholder = {}, bool enabled = true) {
        const bool is_hovered = enabled && ctx.hovered(decl.id);
        if (enabled && ctx.clicked(decl.id)) {
            state.set_focused(true);
            state.set_caret_to(state.text().size(), false);
        } else if (ctx.clicked_outside(decl.id)) {
            state.set_focused(false);
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

        TextEditState::ApplyResult apply_result{};
        if (enabled) {
            apply_result = state.apply_input(filtered_input, /*multiline=*/true);
            if (state.focused() && (up_pressed || down_pressed)) {
                const vector<std::pair<usize, usize>> paragraphs = Detail::split_paragraphs(state.text());
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
                    state.set_caret_to(0, input.shift_held);
                } else if (target_index >= static_cast<isize>(paragraphs.size())) {
                    state.set_caret_to(state.text().size(), input.shift_held);
                } else {
                    const auto &[tstart, tlen] = paragraphs[static_cast<usize>(target_index)];
                    state.set_caret_to(tstart + std::min(local_col, tlen), input.shift_held);
                }
            }
        }

        // See text_input()'s identical call for why this must run before the box/rows below open.
        if (state.focused()) {
            ctx.scroll_into_view(decl.id, Detail::caret_element_id(decl.id));
        }

        ElementDecl styled = decl;
        styled.background_color = state.current_color();
        styled.corner_radius = style.corner_radius;
        styled.border = state.focused() ? style.border_focused : style.border_idle;
        styled.direction = LayoutDirection::TopToBottom;
        styled.clip = ClipConfig{.horizontal = true, .vertical = true};
        auto box = ctx.element(styled);
        (void)box;

        const bool buffer_empty = state.text().empty();
        const vector<std::pair<usize, usize>> paragraphs = Detail::split_paragraphs(state.text());
        for (const auto &[pstart, plen] : paragraphs) {
            const UString paragraph_text = state.text().substr(pstart, plen);
            Detail::render_line(ctx, paragraph_text, pstart, style, state, decl.id, buffer_empty ? placeholder : UString{});
        }

        return TextAreaResult{.changed = apply_result.changed, .focused = state.focused()};
    }

} // namespace SFT::UI
