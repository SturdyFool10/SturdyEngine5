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

/// Multi-line text input, built on the same TextEdit.hpp engine text_input() (TextInput.hpp) uses —
/// Enter inserts a newline instead of submitting, the box scrolls both ways (or wraps, see
/// TextEditStyle::wrap_lines), and EditKey::Up/Down move the caret vertically between paragraphs.
namespace SFT::UI {

    struct TextAreaResult {
        bool changed = false;
        bool focused = false;
        /// See TextInputResult::caret_bounds's own doc comment — same one-frame-stale IME-positioning
        /// hook, present only while focused.
        std::optional<ElementBounds> caret_bounds;
    };

    /// `decl.id` must be set (see text_input()'s own doc comment — same click-to-focus/
    /// click_outside-to-defocus convention). `decl.clip` is overwritten to
    /// `{.horizontal = true, .vertical = true}` regardless of what's passed in — a text area is a
    /// scroll container by definition, and is drawn through scroll_area() (ScrollArea.hpp) for a
    /// real egui-style thumb/track — see `scrollbar_style`/`scroll_state` below. `scroll_state` is
    /// persistent, caller-owned state, same convention as `state` itself.
    ///
    /// One v1 simplification, documented in more depth on its own piece:
    /// - EditKey::Up/Down move by *paragraph* (hard '\n'-delimited line), not by wrapped visual
    ///   line — a very long paragraph that word-wraps across several visual lines only gets one
    ///   Up/Down stop for the whole paragraph, not one per wrapped line. True visual-line
    ///   granularity needs per-wrapped-line bounds Clay's own word-wrap result doesn't expose after
    ///   the fact, which is out of scope for the same reasons click-to-position previously was.
    ///
    /// Lines don't word-wrap by default (TextEditStyle::wrap_lines = false) — a line wider than the
    /// box just scrolls horizontally, the same convention a code editor uses (and this is where a
    /// syntax-highlighted line's Highlighter spans nearly always land: see Detail::render_line's own
    /// doc comment for why a line split into multiple runs can't wrap coherently across them even
    /// with wrap_lines = true).
    [[nodiscard]] TextAreaResult text_area(Context &ctx, const ElementDecl &decl, const TextEditStyle &style,
                                                   TextEditState &state, const TextEditInput &input, f32 delta_seconds,
                                                   const ScrollbarStyle &scrollbar_style, ScrollAreaState &scroll_state,
                                                   const UString &placeholder = {}, bool enabled = true);

} // namespace SFT::UI
