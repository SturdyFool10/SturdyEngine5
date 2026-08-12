#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <utility>
#include <vector>
#pragma endregion

#include "Context.hpp"
#include "ScrollArea.hpp"
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
        // See TextInputResult::caret_bounds's own doc comment — same one-frame-stale IME-positioning
        // hook, present only while focused.
        std::optional<ElementBounds> caret_bounds;
    };

    // `decl.id` must be set (see text_input()'s own doc comment — same click-to-focus/
    // click_outside-to-defocus convention). `decl.clip` is overwritten to
    // `{.horizontal = true, .vertical = true}` regardless of what's passed in — a text area is a
    // scroll container by definition, and is drawn through scroll_area() (ScrollArea.hpp) for a
    // real egui-style thumb/track — see `scrollbar_style`/`scroll_state` below. `scroll_state` is
    // persistent, caller-owned state, same convention as `state` itself.
    //
    // One v1 simplification, documented in more depth on its own piece:
    // - EditKey::Up/Down move by *paragraph* (hard '\n'-delimited line), not by wrapped visual
    //   line — a very long paragraph that word-wraps across several visual lines only gets one
    //   Up/Down stop for the whole paragraph, not one per wrapped line. True visual-line
    //   granularity needs per-wrapped-line bounds Clay's own word-wrap result doesn't expose after
    //   the fact, which is out of scope for the same reasons click-to-position previously was.
    //
    // Lines don't word-wrap by default (TextEditStyle::wrap_lines = false) — a line wider than the
    // box just scrolls horizontally, the same convention a code editor uses (and this is where a
    // syntax-highlighted line's Highlighter spans nearly always land: see Detail::render_line's own
    // doc comment for why a line split into multiple runs can't wrap coherently across them even
    // with wrap_lines = true).
    [[nodiscard]] inline TextAreaResult text_area(Context &ctx, const ElementDecl &decl, const TextEditStyle &style,
                                                   TextEditState &state, const TextEditInput &input, f32 delta_seconds,
                                                   const ScrollbarStyle &scrollbar_style, ScrollAreaState &scroll_state,
                                                   const UString &placeholder = {}, bool enabled = true) {
        // Click/drag hit-testing is matched against *last* frame's committed element_bounds() (see
        // Context's own one-frame-stale convention), which were laid out from text() as it stood
        // before this frame's apply_input() below runs — so this split must happen here, before
        // that call, not be reused after it mutates text() (a second split happens after
        // apply_input(), reused by Up/Down navigation and the render loop, which both need the
        // post-edit paragraph breakdown instead).
        const vector<std::pair<usize, usize>> click_paragraphs = Detail::split_paragraphs(state.text());

        const bool is_hovered = enabled && ctx.hovered(decl.id);
        if (enabled && ctx.clicked(decl.id)) {
            state.set_focused(true);
            usize caret_scalar = state.text().size();
            // Click-to-position: find whichever paragraph row's last-committed bounds sit closest
            // (by vertical center) to the click, then hit-test the click's x within that paragraph
            // — same two-step "which row, then where in it" a multi-line editor needs regardless of
            // whether row layout comes from hard line breaks (here) or word-wrap.
            const std::optional<Detail::ParagraphHit> hit =
                Detail::hit_test_paragraphs(ctx, style, state.text(), click_paragraphs, decl.id, ctx.pointer_position());
            if (hit) {
                caret_scalar = hit->scalar;
            }
            // See text_input()'s identical branch for why shift+click never participates in the
            // multi-click streak, and why the streak (self-timed, not OS-synced) picks word- vs.
            // line-select.
            if (style.features.pointer_selection && input.shift_held) {
                state.register_click(ctx.pointer_position(), /*allow_multi_click=*/false);
                state.set_caret_to(caret_scalar, /*extend=*/true);
            } else if (style.features.pointer_selection) {
                const u8 click_streak = state.register_click(ctx.pointer_position(), /*allow_multi_click=*/true);
                if (click_streak >= 3 && hit) {
                    state.select_range(hit->paragraph_start, hit->paragraph_start + hit->paragraph_length);
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

        // Drag-to-select: see text_input()'s identical block for the capture/release pattern (mirrors
        // ScrollArea.hpp's thumb drag) and the same v1 simplifications (character-granularity only,
        // no auto-scroll past the visible edge).
        if (enabled && style.features.pointer_selection && ctx.has_pointer_capture(decl.id)) {
            if (ctx.pointer_cancelled_this_frame() || (!ctx.pointer_is_down() && !ctx.pointer_pressed_this_frame())) {
                ctx.release_pointer(decl.id);
            } else if (ctx.pointer_is_down()) {
                if (const std::optional<Detail::ParagraphHit> drag_hit = Detail::hit_test_paragraphs(
                        ctx, style, state.text(), click_paragraphs, decl.id, ctx.pointer_position())) {
                    state.set_caret_to(drag_hit->scalar, /*extend=*/true);
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

        // Reused by the render loop below. Starts as click_paragraphs (still accurate when
        // !enabled, since text() can't have changed this frame without apply_input() running) and
        // is refreshed after apply_input() actually mutates text() — see click_paragraphs' own doc
        // comment for why those two moments can't share a single split.
        vector<std::pair<usize, usize>> paragraphs = click_paragraphs;

        TextEditState::ApplyResult apply_result{};
        if (enabled) {
            apply_result = state.apply_input(filtered_input, /*multiline=*/true, style.features, style.bindings);
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

        // See text_input()'s identical call for why this must run before the box/rows below open.
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
