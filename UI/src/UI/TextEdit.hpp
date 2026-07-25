#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>
#pragma endregion

#include <Text/Text.hpp>

#include "Button.hpp"
#include "Context.hpp"
#include "Style.hpp"

using std::string_view;
using std::vector;

// Shared editing engine behind TextInput.hpp's text_input() (single-line) and TextArea.hpp's
// text_area() (multi-line) — the buffer/caret/selection/clipboard/highlight-cache machinery both
// widgets need is identical; only how they lay it out on screen (one row vs. wrapped/scrollable
// paragraphs, Enter-submits vs. Enter-inserts-newline) differs. Same "engine shared, presentation
// split" shape as Toggle.hpp sharing one ToggleState/ToggleStyle across checkbox/radio_button/
// switch_toggle.
namespace SFT::UI {

    // High-level, already-decoded editing intents this frame — the caller (Runtime/Engine
    // integration) translates raw platform key/modifier state into this before calling
    // text_input()/text_area(), the same "caller sources it, UI stays backend-agnostic" split
    // PointerState already uses for pointer input. In particular Ctrl+A/C/X/V (or Cmd+ on macOS)
    // are expected to already be resolved into SelectAll/Copy/Cut/Paste here, not delivered as
    // Copy == 'C' + a modifier the widget would have to interpret itself.
    enum class EditKey : u8 {
        Left,
        Right,
        Up,
        Down,
        Home,
        End,
        Backspace,
        Delete,
        Enter,
        Tab,
        Escape,
        SelectAll,
        Copy,
        Cut,
        Paste,
    };

    struct TextEditInput {
        // This frame's UTF-8 text-input bytes (from Platform::Windowing::WindowTextInputEvent —
        // pass its NUL-terminated utf8[32] straight through, e.g. `std::string_view{event.text.
        // utf8}`), empty if nothing was typed. A plain string_view (not `ustr`, which is
        // deliberately non-copyable/non-movable and so can't live in an otherwise-copyable struct
        // like this one) — borrowed, must outlive the text_input()/text_area() call it's passed to.
        string_view typed_text;
        // Edit-relevant key presses this frame — OS key-repeat naturally produces more than one
        // Left/Backspace/etc. in a row across frames, which is why this is used as-is rather than
        // requiring exactly one key per frame.
        vector<EditKey> keys;
        bool shift_held = false;
        // Ctrl (or Cmd on macOS) — widens Left/Right/Backspace/Delete to word granularity.
        bool word_modifier_held = false;
        // Clipboard bridge for Copy/Cut/Paste — text_input()/text_area() never touch a platform
        // clipboard API directly (same platform-agnostic reasoning as everything else here). Leave
        // both null to make Copy/Cut/Paste silent no-ops. See Platform::Windowing::Window::
        // clipboard_text()/set_clipboard_text() for the SDL3/GLFW-backed implementation this
        // typically wraps.
        std::function<UString()> get_clipboard_text;
        std::function<void(const UString &)> set_clipboard_text;
    };

    // One color override over a scalar-index range of the buffer — e.g. a keyword, a string
    // literal, a Markdown heading marker. Deliberately just a color (not a full TextStyle): font
    // weight/size variation would need multiple font registrations per run, which is a much bigger
    // ask than the common "recolor this token" case syntax highlighting and Markdown actually need.
    struct RichTextSpan {
        usize scalar_start = 0;
        usize scalar_length = 0;
        Color color{1.0, 1.0, 1.0, 1.0};
    };

    // Optional syntax/Markdown highlighting hook: given the buffer's current text, return the
    // color runs to paint over it. nullptr (default) renders the whole buffer in
    // TextEditStyle::text_color, one run, no per-character cost at all.
    //
    // Efficiency contract: TextEditState caches the last text this was run against and only calls
    // it again when the buffer actually changed (see TextEditState::highlighted_spans()) — a
    // highlighter is free to be as expensive as a real Markdown parse or a tokenizer without
    // costing anything on frames where the user isn't actively typing (the overwhelming majority
    // of frames for an idle-but-focused input).
    using Highlighter = std::function<vector<RichTextSpan>(const UString &)>;

    struct TextEditStyle {
        Color idle{0.14, 0.15, 0.19, 1.0};
        Color hovered{0.17, 0.18, 0.23, 1.0};
        Color focused{0.12, 0.13, 0.17, 1.0};
        Color disabled{0.14, 0.15, 0.19, 0.5};
        BorderStyle border_idle{.color = Color{0.3, 0.32, 0.38, 1.0}, .width = BorderWidth::all(1)};
        BorderStyle border_focused{.color = Color{0.35, 0.55, 0.85, 1.0}, .width = BorderWidth::all(1)};
        Color text_color{0.92, 0.93, 0.95, 1.0};
        Color placeholder_color{0.5, 0.52, 0.56, 1.0};
        Color selection_color{0.28, 0.45, 0.75, 0.45};
        Color caret_color{1.0, 1.0, 1.0, 1.0};
        CornerRadius corner_radius = CornerRadius::all(6.0f);
        FontId font_id = 0;
        u16 font_size = 16;
        // Full on/off period is twice this — half visible, half not. <= 0 keeps the caret solidly
        // visible (no blink).
        f32 caret_blink_seconds = 0.53f;
        f32 transition_seconds = 0.25f;
        ColorBlendSpace color_space = ColorBlendSpace::Oklab;
        EasingFn easing = Easing::cubic_in_out;
        // See Highlighter's own doc comment.
        Highlighter highlighter = nullptr;
        // false (default): lines never word-wrap — a line wider than the box scrolls
        // horizontally instead (Detail::render_line's row is Fit-width inside the caller's own
        // ClipConfig::horizontal box), the same convention a code editor or a plain single-line
        // input uses. true opts into word-wrap, but only takes full effect on a line with no
        // highlighter match, active selection, or caret on it — see Detail::render_line's own doc
        // comment for why a line that has to split into multiple runs can't wrap coherently across
        // them regardless of this flag (Clay only wraps within a single text element).
        bool wrap_lines = false;
    };

    namespace Detail {

        [[nodiscard]] inline UString strip_newlines(const UString &text) {
            UString result;
            for (char32_t scalar : text.codepoints()) {
                if (scalar != U'\n' && scalar != U'\r') {
                    result.append(scalar);
                }
            }
            return result;
        }

    } // namespace Detail

    // Persistent per-input state: the text buffer itself, caret/selection, focus, and the
    // animation/highlight caches text_input()/text_area() need — one instance per logical input,
    // kept alive across frames by the caller, same convention as every other *State in this
    // package (ButtonState, ToggleState, DropdownState). The buffer/caret/selection mutators below
    // are public so a caller building fully custom text-editing UI (not going through
    // text_input()/text_area() at all) can still reuse this engine directly — same "usable
    // standalone" reasoning as Button.hpp's ColorTransition.
    class TextEditState {
      public:
        [[nodiscard]] const UString &text() const noexcept { return text_; }

        // Replaces the whole buffer and resets caret/selection to the end — for programmatic
        // changes (loading a saved value, resetting a form). Editing through
        // text_input()/text_area() itself never calls this.
        void set_text(UString text) noexcept {
            text_ = std::move(text);
            caret_ = text_.size();
            selection_anchor_ = caret_;
        }

        [[nodiscard]] usize caret() const noexcept { return caret_; }
        [[nodiscard]] bool has_selection() const noexcept { return selection_anchor_ != caret_; }
        [[nodiscard]] usize selection_min() const noexcept { return std::min(caret_, selection_anchor_); }
        [[nodiscard]] usize selection_max() const noexcept { return std::max(caret_, selection_anchor_); }
        [[nodiscard]] UString selected_text() const {
            return has_selection() ? text_.substr(selection_min(), selection_max() - selection_min()) : UString{};
        }

        [[nodiscard]] bool focused() const noexcept { return focused_; }
        void set_focused(bool focused) noexcept { focused_ = focused; }

        void insert(const UString &value) {
            if (value.empty()) {
                return;
            }
            if (has_selection()) {
                delete_selection();
            }
            text_.insert(caret_, value);
            caret_ += value.size();
            selection_anchor_ = caret_;
        }

        void delete_selection() {
            if (!has_selection()) {
                return;
            }
            const usize start = selection_min();
            text_.erase(start, selection_max() - start);
            caret_ = start;
            selection_anchor_ = start;
        }

        void backspace(bool word) {
            if (has_selection()) {
                delete_selection();
                return;
            }
            if (caret_ == 0) {
                return;
            }
            const usize start = word ? word_boundary_before(caret_) : caret_ - 1;
            text_.erase(start, caret_ - start);
            caret_ = start;
            selection_anchor_ = start;
        }

        void delete_forward(bool word) {
            if (has_selection()) {
                delete_selection();
                return;
            }
            if (caret_ >= text_.size()) {
                return;
            }
            const usize stop = word ? word_boundary_after(caret_) : caret_ + 1;
            text_.erase(caret_, stop - caret_);
        }

        // `direction` < 0 is Left, > 0 is Right. Collapses an existing selection to its
        // corresponding edge instead of moving past it when `extend` is false — the same
        // convention every text editor uses (Left with a selection lands you at its start, not one
        // further left).
        void move_caret(isize direction, bool extend, bool word) noexcept {
            if (has_selection() && !extend) {
                caret_ = direction < 0 ? selection_min() : selection_max();
                selection_anchor_ = caret_;
                return;
            }
            if (direction < 0) {
                caret_ = word ? word_boundary_before(caret_) : (caret_ > 0 ? caret_ - 1 : 0);
            } else {
                caret_ = word ? word_boundary_after(caret_) : std::min(caret_ + 1, text_.size());
            }
            if (!extend) {
                selection_anchor_ = caret_;
            }
        }

        void move_to_start(bool extend) noexcept {
            caret_ = 0;
            if (!extend) {
                selection_anchor_ = caret_;
            }
        }

        void move_to_end(bool extend) noexcept {
            caret_ = text_.size();
            if (!extend) {
                selection_anchor_ = caret_;
            }
        }

        void select_all() noexcept {
            selection_anchor_ = 0;
            caret_ = text_.size();
        }

        // Clamped to the buffer's own bounds — for placing the caret from something other than a
        // keypress (e.g. a future precise click-to-position implementation; text_input()/
        // text_area() currently only call this with 0 or text().size(), see their own doc comments
        // on the click-to-focus simplification).
        void set_caret_to(usize scalar_index, bool extend) noexcept {
            caret_ = std::min(scalar_index, text_.size());
            if (!extend) {
                selection_anchor_ = caret_;
            }
        }

        struct ApplyResult {
            bool changed = false;
            // Enter pressed in single-line mode (text_input()) — text_area() never sets this,
            // Enter inserts a newline there instead. See apply_input()'s own doc comment.
            bool submitted = false;
        };

        // Applies one frame's typed text + edit keys in order (typed text first, matching how a
        // real keystroke both types a character and can't also be a control key in the same
        // event). `multiline` controls two things: whether Enter inserts '\n' (true) or reports
        // ApplyResult::submitted (false), and whether typed/pasted text has embedded newlines
        // stripped (single-line inputs strip them; multi-line inputs keep them). No-ops entirely
        // (returns a default-constructed ApplyResult) when not focused(). EditKey::Up/Down are
        // deliberately ignored here — vertical caret movement needs to know about line layout,
        // which only text_area() has, so text_area() intercepts and handles those two itself
        // before calling this.
        ApplyResult apply_input(const TextEditInput &input, bool multiline) {
            ApplyResult result{};
            if (!focused_) {
                return result;
            }

            if (!input.typed_text.empty()) {
                UString typed{input.typed_text};
                if (!multiline) {
                    typed = Detail::strip_newlines(typed);
                }
                if (!typed.empty()) {
                    insert(typed);
                    result.changed = true;
                }
            }

            for (EditKey key : input.keys) {
                switch (key) {
                case EditKey::Backspace:
                    backspace(input.word_modifier_held);
                    result.changed = true;
                    break;
                case EditKey::Delete:
                    delete_forward(input.word_modifier_held);
                    result.changed = true;
                    break;
                case EditKey::Left:
                    move_caret(-1, input.shift_held, input.word_modifier_held);
                    break;
                case EditKey::Right:
                    move_caret(1, input.shift_held, input.word_modifier_held);
                    break;
                case EditKey::Home:
                    move_to_start(input.shift_held);
                    break;
                case EditKey::End:
                    move_to_end(input.shift_held);
                    break;
                case EditKey::Up:
                case EditKey::Down:
                case EditKey::Tab:
                    // Up/Down: see this method's own doc comment. Tab: no focus-chain concept
                    // exists in this package (see Context.hpp) for Tab to move between, so it's
                    // left for the caller to interpret (insert a literal tab, move focus
                    // elsewhere, do nothing) rather than this engine guessing.
                    break;
                case EditKey::Enter:
                    if (multiline) {
                        insert(UString{"\n"});
                        result.changed = true;
                    } else {
                        result.submitted = true;
                    }
                    break;
                case EditKey::Escape:
                    set_focused(false);
                    break;
                case EditKey::SelectAll:
                    select_all();
                    break;
                case EditKey::Copy:
                    if (has_selection() && input.set_clipboard_text) {
                        input.set_clipboard_text(selected_text());
                    }
                    break;
                case EditKey::Cut:
                    if (has_selection() && input.set_clipboard_text) {
                        input.set_clipboard_text(selected_text());
                        delete_selection();
                        result.changed = true;
                    }
                    break;
                case EditKey::Paste:
                    if (input.get_clipboard_text) {
                        UString pasted = input.get_clipboard_text();
                        if (!multiline) {
                            pasted = Detail::strip_newlines(pasted);
                        }
                        if (!pasted.empty()) {
                            insert(pasted);
                            result.changed = true;
                        }
                    }
                    break;
                }
            }

            if (result.changed) {
                blink_elapsed_ = 0.0f; // typing/editing always shows a solid caret, not mid-blink-off
            }
            return result;
        }

        // Advances the idle/hovered/focused/disabled color blend and the caret blink clock — call
        // once per frame regardless of whether apply_input() changed anything.
        void update_visual(bool hovered, bool enabled, const TextEditStyle &style, f32 delta_seconds) noexcept {
            const Color &target = !enabled ? style.disabled : focused_ ? style.focused : hovered ? style.hovered : style.idle;
            color_.update(target, delta_seconds, style.transition_seconds, style.color_space, style.easing);
            blink_elapsed_ += delta_seconds;
        }

        [[nodiscard]] Color current_color() const noexcept { return color_.current(); }

        [[nodiscard]] bool caret_blink_on(f32 blink_seconds) const noexcept {
            if (blink_seconds <= 0.0f) {
                return true;
            }
            const f32 period = blink_seconds * 2.0f;
            const f32 phase = std::fmod(blink_elapsed_, period);
            return phase < blink_seconds;
        }

        // Cached highlighter output — see Highlighter's own doc comment for the efficiency
        // contract this implements (only re-runs `highlighter` when text() changed since the last
        // call). Empty (and cheap: one string comparison, no cache touch) when `highlighter` is
        // null.
        [[nodiscard]] const vector<RichTextSpan> &highlighted_spans(const Highlighter &highlighter) const {
            if (!highlighter) {
                highlight_cache_spans_.clear();
                highlight_cache_key_.clear();
                return highlight_cache_spans_;
            }
            if (highlight_cache_key_ != text_) {
                highlight_cache_key_ = text_;
                highlight_cache_spans_ = highlighter(text_);
            }
            return highlight_cache_spans_;
        }

      private:
        [[nodiscard]] usize word_boundary_before(usize scalar_index) const {
            if (scalar_index == 0 || text_.empty()) {
                return 0;
            }
            const usize byte_pos = text_.byte_index_of(scalar_index);
            const vector<usize> boundaries = Text::word_boundaries(text_.as_ustr());
            usize best = 0;
            for (usize boundary : boundaries) {
                if (boundary < byte_pos) {
                    best = boundary;
                } else {
                    break;
                }
            }
            return text_.scalar_index_of_byte(best);
        }

        [[nodiscard]] usize word_boundary_after(usize scalar_index) const {
            if (text_.empty()) {
                return 0;
            }
            const usize byte_pos = text_.byte_index_of(scalar_index);
            const vector<usize> boundaries = Text::word_boundaries(text_.as_ustr());
            for (usize boundary : boundaries) {
                if (boundary > byte_pos) {
                    return text_.scalar_index_of_byte(boundary);
                }
            }
            return text_.size();
        }

        UString text_;
        usize caret_ = 0;
        usize selection_anchor_ = 0;
        bool focused_ = false;

        ColorTransition color_{};
        f32 blink_elapsed_ = 0.0f;

        mutable UString highlight_cache_key_;
        mutable vector<RichTextSpan> highlight_cache_spans_;
    };

    namespace Detail {

        // Splits `text` into paragraphs at each '\n' — (scalar_start, scalar_length) pairs,
        // exclusive of the separator itself, always at least one entry (an empty buffer yields one
        // empty paragraph). TextArea.hpp uses this both to lay out one row per paragraph and, since
        // it's the same "line" granularity TextEditState::apply_input() leaves EditKey::Up/Down
        // for text_area() to handle itself, to find which paragraph the caret is in for vertical
        // caret movement — see text_area()'s own doc comment for why paragraphs (hard line breaks)
        // rather than wrapped visual lines is the granularity here.
        [[nodiscard]] inline vector<std::pair<usize, usize>> split_paragraphs(const UString &text) {
            vector<std::pair<usize, usize>> result;
            usize start = 0;
            const usize len = text.size();
            for (usize i = 0; i < len; ++i) {
                if (text[i] == U'\n') {
                    result.emplace_back(start, i - start);
                    start = i + 1;
                }
            }
            result.emplace_back(start, len - start);
            return result;
        }

        // Stable per-widget id for the caret bar element render_line() creates on whichever line
        // currently contains it — text_input()/text_area() use the same id to ask
        // Context::scroll_into_view() to keep it on screen. `widget_id` is the text_input()/
        // text_area() call's own ElementDecl::id.
        [[nodiscard]] inline UString caret_element_id(const UString &widget_id) {
            return UString{widget_id.cpp_string() + "#caret"};
        }

        [[nodiscard]] inline Color color_for_run(const vector<RichTextSpan> &spans, usize global_scalar_index,
                                                 const Color &fallback) noexcept {
            for (const RichTextSpan &span : spans) {
                if (global_scalar_index >= span.scalar_start && global_scalar_index < span.scalar_start + span.scalar_length) {
                    return span.color;
                }
            }
            return fallback;
        }

        // Renders one visual line/paragraph of `state.text()` as its own row element (opens and
        // fills it — callers just call this once per line, they don't pre-open a row of their
        // own). `line_text` is that line's own substring, `line_scalar_offset` is where it starts
        // within the whole buffer (so state.caret()/selection/highlighted_spans(), all in whole-
        // buffer scalar indices, can be mapped onto it).
        //
        // style.wrap_lines is false by default: every run renders at its natural unwrapped width
        // (TextWrapMode::None) inside a Fit-width row that's free to exceed the box — the caller's
        // own outer ClipConfig::horizontal scrolls it instead, the same convention a code editor or
        // a plain single-line input uses. Set style.wrap_lines = true to word-wrap instead, which
        // takes full effect only on a line with no highlighter match, active selection, or caret on
        // it (rendered as a single ordinary Clay text element, wrapped normally within the row's
        // own bounded `grow()` width) — any other line still has to split into multiple sibling
        // elements (one per highlight/selection/caret run, so selection highlighting is just that
        // run's own background_color and the caret is a plain 2px-wide element spliced into the
        // flow — placed pixel-correctly by Clay's ordinary layout with no glyph-shaping or text-
        // measurement code needed on our side at all), and Clay only ever wraps text within a
        // single text element: letting several sibling Fit elements plus the caret's own fixed
        // width all compete in Clay's width-fitting/shrink pass corrupts each run's *independent*
        // wrap far worse than not wrapping at all (a shrunk run's wrap boundary has nothing to do
        // with where the row itself would have wrapped), so those lines fall back to the same
        // unwrapped/scrolled rendering regardless of wrap_lines. See text_area()'s own doc comment
        // for the user-visible summary of this tradeoff.
        inline void render_line(Context &ctx, const UString &line_text, usize line_scalar_offset,
                                const TextEditStyle &style, const TextEditState &state, const UString &widget_id,
                                const UString &placeholder = UString{}) {
            const usize line_len = line_text.size();
            const vector<RichTextSpan> &spans = state.highlighted_spans(style.highlighter);

            std::set<usize> cuts{0, line_len};
            for (const RichTextSpan &span : spans) {
                const isize local_start = static_cast<isize>(span.scalar_start) - static_cast<isize>(line_scalar_offset);
                const isize local_end = local_start + static_cast<isize>(span.scalar_length);
                if (local_end <= 0 || local_start >= static_cast<isize>(line_len)) {
                    continue;
                }
                cuts.insert(static_cast<usize>(std::clamp<isize>(local_start, 0, static_cast<isize>(line_len))));
                cuts.insert(static_cast<usize>(std::clamp<isize>(local_end, 0, static_cast<isize>(line_len))));
            }

            const bool has_sel = state.has_selection();
            usize sel_min = 0;
            usize sel_max = 0;
            if (has_sel) {
                const isize s0 = static_cast<isize>(state.selection_min()) - static_cast<isize>(line_scalar_offset);
                const isize s1 = static_cast<isize>(state.selection_max()) - static_cast<isize>(line_scalar_offset);
                sel_min = static_cast<usize>(std::clamp<isize>(s0, 0, static_cast<isize>(line_len)));
                sel_max = static_cast<usize>(std::clamp<isize>(s1, 0, static_cast<isize>(line_len)));
                if (sel_max > sel_min) {
                    cuts.insert(sel_min);
                    cuts.insert(sel_max);
                }
            }

            const isize caret_local = static_cast<isize>(state.caret()) - static_cast<isize>(line_scalar_offset);
            const bool caret_on_this_line =
                state.focused() && caret_local >= 0 && caret_local <= static_cast<isize>(line_len);
            if (caret_on_this_line) {
                cuts.insert(static_cast<usize>(caret_local));
            }
            const f32 caret_height = static_cast<f32>(style.font_size) * 1.2f;
            // The caret element itself is always created (reserving its 2px) whenever it's on this
            // line — only its *color* toggles between caret_color and transparent for the blink,
            // never its presence. Toggling presence instead would add/remove 2px from the row's
            // total Fit width every blink cycle, visibly nudging every run/placeholder after it by
            // that same 2px each time — a real, previously-shipped jitter bug, not a hypothetical.
            const Color caret_render_color =
                state.caret_blink_on(style.caret_blink_seconds) ? style.caret_color : Color{0.0, 0.0, 0.0, 0.0};

            const vector<usize> sorted_cuts(cuts.begin(), cuts.end());
            // Exactly {0, line_len} — no highlight/selection/caret cut fell inside this line, the
            // common case (see this function's own doc comment for why the multi-run case can't
            // wrap the same way).
            const bool single_run = sorted_cuts.size() <= 2;
            const bool can_wrap = style.wrap_lines && single_run;
            const TextWrapMode run_wrap_mode = can_wrap ? TextWrapMode::Words : TextWrapMode::None;

            auto row = ctx.element(ElementDecl{
                .sizing = can_wrap ? Sizing{SizingAxis::grow(), SizingAxis::fit()} : Sizing{SizingAxis::fit(), SizingAxis::fit()},
            });
            (void)row;

            for (usize i = 0; i + 1 < sorted_cuts.size(); ++i) {
                const usize run_start = sorted_cuts[i];
                const usize run_end = sorted_cuts[i + 1];
                if (caret_on_this_line && run_start == static_cast<usize>(caret_local)) {
                    auto caret_bar = ctx.element(ElementDecl{
                        .sizing = {SizingAxis::fixed(2.0f), SizingAxis::fixed(caret_height)},
                        .background_color = caret_render_color,
                        .id = caret_element_id(widget_id),
                    });
                    (void)caret_bar;
                }
                if (run_end == run_start) {
                    continue;
                }
                const Color run_color = color_for_run(spans, run_start + line_scalar_offset, style.text_color);
                const bool run_selected = has_sel && run_start >= sel_min && run_end <= sel_max;
                auto run_scope = ctx.element(ElementDecl{
                    .background_color = run_selected ? style.selection_color : Color{0.0, 0.0, 0.0, 0.0},
                });
                (void)run_scope;
                const UString run_text = line_text.substr(run_start, run_end - run_start);
                ctx.text(run_text.as_ustr(), TextStyle{.color = run_color, .font_id = style.font_id,
                                                       .font_size = style.font_size, .wrap_mode = run_wrap_mode});
            }
            if (caret_on_this_line && static_cast<usize>(caret_local) == line_len) {
                auto caret_bar = ctx.element(ElementDecl{
                    .sizing = {SizingAxis::fixed(2.0f), SizingAxis::fixed(caret_height)},
                    .background_color = caret_render_color,
                    .id = caret_element_id(widget_id),
                });
                (void)caret_bar;
            }
            if (line_len == 0 && !placeholder.empty()) {
                ctx.text(placeholder.as_ustr(), TextStyle{.color = style.placeholder_color, .font_id = style.font_id,
                                                          .font_size = style.font_size});
            }
        }

    } // namespace Detail

} // namespace SFT::UI
