#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#pragma endregion

#include <Text/Text.hpp>

#include "Button.hpp"
#include "Context.hpp"
#include "Style.hpp"

using std::function;
using std::pair;
using std::set;
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

    // Which independently useful pieces of editor behavior a widget exposes. Keeping this policy
    // on TextEditStyle lets a search field, terminal, code editor, or password prompt share the
    // same editing engine without each growing a fork with subtly different keyboard semantics.
    struct TextEditFeatures {
        bool typing = true;
        bool navigation = true;
        bool deletion = true;
        bool selection = true;
        bool clipboard = true;
        bool submission = true;
        bool escape_to_unfocus = true;
        bool pointer_selection = true;
        bool horizontal_scroll = true;
        bool vertical_scroll = true;
        bool scrollbars = true;
    };

    // Maps an input-layer key intent to an editing command. The input layer may therefore expose
    // its own stable, backend-neutral bindings and a consumer can rebind them per widget without
    // teaching TextEditState about platform key codes. Entries not listed retain their identity
    // mapping; a listed entry with enabled=false disables that trigger.
    struct TextEditKeyBinding {
        EditKey trigger = EditKey::Left;
        EditKey command = EditKey::Left;
        bool enabled = true;
    };

    struct TextEditBindings {
        vector<TextEditKeyBinding> keys;

        [[nodiscard]] bool enabled(EditKey trigger) const noexcept {
            for (const TextEditKeyBinding &binding : keys) {
                if (binding.trigger == trigger) {
                    return binding.enabled;
                }
            }
            return true;
        }

        [[nodiscard]] EditKey resolve(EditKey trigger) const noexcept {
            for (const TextEditKeyBinding &binding : keys) {
                if (binding.trigger == trigger) {
                    return binding.command;
                }
            }
            return trigger;
        }
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
        // This frame's in-progress IME composition (preedit) text — e.g. the romaji not yet
        // converted to kana, or a pinyin sequence before its candidate is picked. Source it from
        // Engine::InputState::composition_text() (Platform::Windowing::WindowTextEditingEvent's own
        // doc comment explains why this needs to stay a separate event/field from typed_text: it is
        // never committed text). Rendered inline at the caret, underlined, and never inserted into
        // the buffer — only `typed_text` (which arrives once the IME actually commits) becomes part
        // of text(). Borrowed like typed_text; must outlive the text_input()/text_area() call.
        string_view composition_text;
        // Whether an IME composition is open this frame — normally `!composition_text.empty()`
        // (see Engine::InputState::composing()'s own doc comment for the one rule this follows),
        // kept as its own field so a caller sourcing input some other way than InputState isn't
        // forced to encode "composing but momentarily empty" as a non-empty sentinel string. While
        // true, Enter/Escape are treated as belonging to the IME (confirming/cancelling the
        // composition) rather than the widget's own submit/unfocus behavior — see apply_input()'s
        // EditKey::Enter/EditKey::Escape cases.
        bool composing = false;
        // Clipboard bridge for Copy/Cut/Paste — text_input()/text_area() never touch a platform
        // clipboard API directly (same platform-agnostic reasoning as everything else here). Leave
        // both null to make Copy/Cut/Paste silent no-ops. See Platform::Windowing::Window::
        // clipboard_text()/set_clipboard_text() for the SDL3/GLFW-backed implementation this
        // typically wraps.
        function<UString()> get_clipboard_text;
        function<void(const UString &)> set_clipboard_text;
    };

    // One inline presentation override over a scalar-index range of the buffer — suitable for
    // syntax/LSP semantic tokens, Markdown, or a LaTex parser. `font_id` deliberately selects an
    // application-registered face: bold, italic, math, and code faces vary by application and the
    // UI renderer does not invent synthetic glyph variants. The scalar range is UTF-8 safe because
    // UString's editing and substring APIs use the same scalar indices.
    struct RichTextSpan {
        usize scalar_start = 0;
        usize scalar_length = 0;
        Color color{1.0, 1.0, 1.0, 1.0};
        FontId font_id = 0;
        // false keeps TextEditStyle::font_id; true uses this span's registered face (including
        // face id zero, if that is the application's chosen bold/italic/math face).
        bool use_font_id = false;
        // Superscript/subscript and math runs can select a smaller registered size. A true
        // baseline offset is deliberately not exposed until TextStyle gains a renderer-backed
        // baseline-offset primitive; pretending with a layout element would corrupt caret and hit
        // testing geometry.
        f32 font_size_scale = 1.0f;
        bool underline = false;
        bool strikethrough = false;
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
    using Highlighter = function<vector<RichTextSpan>(const UString &)>;

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
        // Password mode: every character *renders* as `mask_glyph` while the buffer itself keeps
        // the real text (TextEditState::text() is unaffected — that's what the app submits).
        // Masking happens per scalar, so caret/selection positions stay exact. The placeholder is
        // never masked. `mask_glyph` must be a single UTF-8 code point the style's font actually
        // has — the bundled Maple Mono NF has U+2022 (•).
        bool mask_characters = false;
        const char *mask_glyph = "•";
        // See Highlighter's own doc comment. RichTextSpan can select registered bold, italic,
        // code, or math faces as well as color and baseline/size for super/subscripts.
        Highlighter highlighter = nullptr;
        TextEditFeatures features{};
        TextEditBindings bindings{};
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

        // This frame's in-progress IME composition text, captured by apply_input() from
        // TextEditInput::composition_text — render_line() paints it inline (underlined) at the
        // caret. Stored as an owned string rather than forwarding TextEditInput's borrowed
        // string_view: TextEditState outlives any single frame's apply_input() call, and render_line()
        // (called later the same frame, but still a second call) has no way to know whether a
        // stored view's backing buffer is still alive — same reasoning as text_/highlight_cache_key_
        // below being owned rather than borrowed.
        [[nodiscard]] string_view composition_text() const noexcept { return composition_text_; }

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

        // Selects the word containing `scalar_index`, via the same word-boundary table
        // Ctrl+Left/Right word-jump already uses (word_boundary_before/after) — double-click's
        // selection target.
        void select_word_at(usize scalar_index) noexcept {
            if (text_.empty()) {
                caret_ = 0;
                selection_anchor_ = 0;
                return;
            }
            const usize probe = std::min(scalar_index, text_.size() - 1);
            selection_anchor_ = word_boundary_before(probe + 1);
            caret_ = word_boundary_after(probe);
        }

        // Plain clamped range setter — triple-click's selection target (the widget already knows
        // the clicked paragraph's own [start, start+length) bounds, no word-boundary lookup needed).
        void select_range(usize start, usize end) noexcept {
            selection_anchor_ = std::min(start, text_.size());
            caret_ = std::min(end, text_.size());
        }

        // Classifies one click event as continuing (or not) a multi-click gesture — timed
        // independently of the OS's own double-click setting, the same "self-timed, not
        // platform-synced" choice caret_blink_on() already makes for the blink rate. Returns the
        // resulting streak (1 = single, 2 = double, capped at 3 = triple-or-more); callers
        // typically treat 2 as "select word" and >=3 as "select line/paragraph".
        //
        // `allow_multi_click=false` (a shift+click) always resets to a fresh streak of 1 — a
        // shift+click is never itself part of a multi-click gesture, and must not let a later
        // plain click spuriously continue a streak that started before it.
        u8 register_click(glm::vec2 position, bool allow_multi_click) noexcept {
            constexpr f32 max_interval_seconds = 0.4f;
            constexpr f32 max_distance_px = 4.0f;
            if (allow_multi_click) {
                const glm::vec2 delta = position - last_click_position_;
                const bool continues_streak = click_streak_ > 0 && time_since_last_click_ <= max_interval_seconds &&
                                              (delta.x * delta.x + delta.y * delta.y) <=
                                                  max_distance_px * max_distance_px;
                click_streak_ = continues_streak ? static_cast<u8>(std::min<int>(click_streak_ + 1, 3)) : u8{1};
            } else {
                click_streak_ = 1;
            }
            last_click_position_ = position;
            time_since_last_click_ = 0.0f;
            return click_streak_;
        }

        // Clamped to the buffer's own bounds — for placing the caret from something other than a
        // keypress, e.g. text_input()/text_area()'s own click-to-position handling (Detail::
        // hit_test_line_scalar(), further down this file).
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
        ApplyResult apply_input(const TextEditInput &input, bool multiline, const TextEditFeatures &features = {},
                                const TextEditBindings &bindings = {}) {
            ApplyResult result{};
            if (!focused_) {
                composition_text_.clear(); // never show a stale preedit on a field that lost focus
                return result;
            }
            composition_text_.assign(input.composition_text);

            if (features.typing && !input.typed_text.empty()) {
                UString typed{input.typed_text};
                if (!multiline) {
                    typed = Detail::strip_newlines(typed);
                }
                if (!typed.empty()) {
                    insert(typed);
                    result.changed = true;
                }
            }

            for (EditKey trigger : input.keys) {
                if (!bindings.enabled(trigger)) {
                    continue;
                }
                const EditKey key = bindings.resolve(trigger);
                switch (key) {
                case EditKey::Backspace:
                    if (features.deletion) {
                        backspace(input.word_modifier_held);
                        result.changed = true;
                    }
                    break;
                case EditKey::Delete:
                    if (features.deletion) {
                        delete_forward(input.word_modifier_held);
                        result.changed = true;
                    }
                    break;
                case EditKey::Left:
                    if (features.navigation) {
                        move_caret(-1, features.selection && input.shift_held, input.word_modifier_held);
                    }
                    break;
                case EditKey::Right:
                    if (features.navigation) {
                        move_caret(1, features.selection && input.shift_held, input.word_modifier_held);
                    }
                    break;
                case EditKey::Home:
                    if (features.navigation) {
                        move_to_start(features.selection && input.shift_held);
                    }
                    break;
                case EditKey::End:
                    if (features.navigation) {
                        move_to_end(features.selection && input.shift_held);
                    }
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
                    // While an IME composition is open, Enter belongs to it (confirming the
                    // conversion) — most platforms never even forward this keypress to the app in
                    // that case, but treating it as a no-op here too is a cheap defense against
                    // whichever backend/compositor combination does, instead of the field silently
                    // submitting/newlining out from under an in-progress composition. See
                    // TextEditInput::composing's own doc comment.
                    if (input.composing) {
                        break;
                    }
                    if (features.submission) {
                        if (multiline) {
                            insert(UString{"\n"});
                            result.changed = true;
                        } else {
                            result.submitted = true;
                        }
                    }
                    break;
                case EditKey::Escape:
                    // Same reasoning as Enter above: while composing, Escape cancels the IME's
                    // composition, not this field's focus.
                    if (input.composing) {
                        break;
                    }
                    if (features.escape_to_unfocus) {
                        set_focused(false);
                    }
                    break;
                case EditKey::SelectAll:
                    if (features.selection) {
                        select_all();
                    }
                    break;
                case EditKey::Copy:
                    if (features.clipboard && has_selection() && input.set_clipboard_text) {
                        input.set_clipboard_text(selected_text());
                    }
                    break;
                case EditKey::Cut:
                    if (features.clipboard && features.deletion && has_selection() && input.set_clipboard_text) {
                        input.set_clipboard_text(selected_text());
                        delete_selection();
                        result.changed = true;
                    }
                    break;
                case EditKey::Paste:
                    if (features.clipboard && features.typing && input.get_clipboard_text) {
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
            time_since_last_click_ += delta_seconds;
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
        std::string composition_text_;

        ColorTransition color_{};
        f32 blink_elapsed_ = 0.0f;

        u8 click_streak_ = 0;
        f32 time_since_last_click_ = 1.0e6f;
        glm::vec2 last_click_position_{0.0f};

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
        [[nodiscard]] inline vector<pair<usize, usize>> split_paragraphs(const UString &text) {
            vector<pair<usize, usize>> result;
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

        // Stable per-line id for render_line()'s own row element — text_input() (one line, always
        // line_scalar_offset 0) and text_area() (one per paragraph, keyed by that paragraph's own
        // scalar start) use this so a click can later look up *this exact line's* on-screen bounds
        // via Context::element_bounds() and translate the click into a scalar index — see
        // hit_test_line_scalar()'s own doc comment.
        [[nodiscard]] inline UString line_element_id(const UString &widget_id, usize line_scalar_offset) {
            return UString{widget_id.cpp_string() + "#line" + std::to_string(line_scalar_offset)};
        }

        [[nodiscard]] inline const RichTextSpan *span_for_run(const vector<RichTextSpan> &spans,
                                                               usize global_scalar_index) noexcept {
            for (const RichTextSpan &span : spans) {
                if (global_scalar_index >= span.scalar_start && global_scalar_index < span.scalar_start + span.scalar_length) {
                    return &span;
                }
            }
            return nullptr;
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

            set<usize> cuts{0, line_len};
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
            // Blink toggles the caret's *color* between caret_color and transparent, never its
            // presence — one less structural difference between blink phases.
            const Color caret_render_color =
                state.caret_blink_on(style.caret_blink_seconds) ? style.caret_color : Color{0.0, 0.0, 0.0, 0.0};

            // The caret must have zero layout footprint: it's the editor's own indicator, not a
            // physical glyph the text should wrap around or be nudged aside by. A zero-*width*
            // anchor holds the insertion point's place in the flow (splitting the surrounding runs
            // positions it pixel-correctly through ordinary layout, no glyph measurement needed),
            // and the visible 2px bar is a floating element centered on that anchor — floating
            // elements never affect layout, so text and placeholder render byte-for-byte
            // identically whether a caret is present or not. (An earlier version spliced the 2px
            // bar directly into the flow, which shifted everything after the caret by 2px and made
            // the placeholder dodge it — the exact "caret as a physical object" bug this replaces.)
            // The anchor still carries caret_height so an empty line's row keeps its text height,
            // and it owns caret_element_id so scroll_into_view() keeps targeting in-flow geometry.
            const auto emit_caret = [&]() {
                auto anchor = ctx.element(ElementDecl{
                    .sizing = {SizingAxis::fixed(0.0f), SizingAxis::fixed(caret_height)},
                    .id = caret_element_id(widget_id),
                });
                auto caret_bar = ctx.element(ElementDecl{
                    .sizing = {SizingAxis::fixed(2.0f), SizingAxis::fixed(caret_height)},
                    .background_color = caret_render_color,
                    .floating = FloatingConfig{
                        .attach_to = FloatingAttachTo::Parent,
                        .element_attach_point = FloatingAttachPoint::CenterCenter,
                        .parent_attach_point = FloatingAttachPoint::CenterCenter,
                        .capture_pointer = false,
                        // Clips with the input box's own scroll container instead of painting over
                        // whatever sits past its edge — see FloatingClipTo's own doc comment.
                        .clip_to = FloatingClipTo::AttachedParent,
                    },
                });
                (void)caret_bar;
                (void)anchor;
            };

            // The in-progress IME composition (preedit) string, if any — rendered as an ordinary
            // (layout-occupying, unlike the caret above) text run right at the caret position,
            // underlined so it reads as "not committed yet," with the caret itself landing after it
            // once emit_caret() runs next. Deliberately not run through the highlighter (it isn't
            // part of the buffer) or password-masked (an IME never composes into a password field in
            // any way that would leak the underlying text — the OS's own candidate window shows the
            // plain composition regardless of what this widget does).
            const string_view composition = state.composition_text();
            const bool show_composition = caret_on_this_line && !composition.empty();
            const auto emit_composition = [&]() {
                if (!show_composition) {
                    return;
                }
                // A dedicated wrapper scope, same as the ordinary run_scope further below — the
                // underline's grow() sizing must resolve against the composition text's own width,
                // not the whole row, and a floating element's "Parent" is whichever element is
                // currently open when it's declared.
                auto scope = ctx.element(ElementDecl{});
                (void)scope;
                auto decoration = ctx.element(ElementDecl{
                    .sizing = {SizingAxis::grow(), SizingAxis::fixed(1.0f)},
                    .background_color = style.text_color,
                    .floating = FloatingConfig{
                        .attach_to = FloatingAttachTo::Parent,
                        .element_attach_point = FloatingAttachPoint::LeftBottom,
                        .parent_attach_point = FloatingAttachPoint::LeftBottom,
                        .capture_pointer = false,
                        .clip_to = FloatingClipTo::AttachedParent,
                    },
                });
                (void)decoration;
                const UString composition_text{composition};
                ctx.text(composition_text.as_ustr(), TextStyle{.color = style.text_color, .font_id = style.font_id,
                                                                .font_size = style.font_size, .wrap_mode = TextWrapMode::None});
            };

            const vector<usize> sorted_cuts(cuts.begin(), cuts.end());
            // Exactly {0, line_len} — no highlight/selection/caret cut fell inside this line, the
            // common case (see this function's own doc comment for why the multi-run case can't
            // wrap the same way).
            const bool single_run = sorted_cuts.size() <= 2;
            const bool can_wrap = style.wrap_lines && single_run;
            const TextWrapMode run_wrap_mode = can_wrap ? TextWrapMode::Words : TextWrapMode::None;

            auto row = ctx.element(ElementDecl{
                .sizing = can_wrap ? Sizing{SizingAxis::grow(), SizingAxis::fit()} : Sizing{SizingAxis::fit(), SizingAxis::fit()},
                .id = line_element_id(widget_id, line_scalar_offset),
            });
            (void)row;

            // Typographic strut, CSS-line-box style: every row — including a completely empty
            // line — keeps at least the caret's height. Without it an empty paragraph rendered as
            // a zero-height row, invisible until the caret moved onto it and its anchor suddenly
            // gave the line height — a "line that didn't exist" popping in and shifting everything
            // below. The vertical cousin of the zero-width caret anchor: the caret must never
            // change the geometry of the text around it, in either axis.
            {
                auto strut = ctx.element(ElementDecl{
                    .sizing = {SizingAxis::fixed(0.0f), SizingAxis::fixed(caret_height)},
                });
                (void)strut;
            }

            for (usize i = 0; i + 1 < sorted_cuts.size(); ++i) {
                const usize run_start = sorted_cuts[i];
                const usize run_end = sorted_cuts[i + 1];
                if (caret_on_this_line && run_start == static_cast<usize>(caret_local)) {
                    emit_composition();
                    emit_caret();
                }
                if (run_end == run_start) {
                    continue;
                }
                const RichTextSpan *span = span_for_run(spans, run_start + line_scalar_offset);
                const Color run_color = span != nullptr ? span->color : style.text_color;
                const FontId run_font_id = span != nullptr && span->use_font_id ? span->font_id : style.font_id;
                const f32 font_scale = span != nullptr ? span->font_size_scale : 1.0f;
                const u16 run_font_size = static_cast<u16>(std::clamp(
                    std::round(static_cast<f32>(style.font_size) * std::max(font_scale, 0.1f)), 1.0f,
                    static_cast<f32>(std::numeric_limits<u16>::max())));
                const bool run_selected = has_sel && run_start >= sel_min && run_end <= sel_max;
                auto run_scope = ctx.element(ElementDecl{
                    .background_color = run_selected ? style.selection_color : Color{0.0, 0.0, 0.0, 0.0},
                });
                (void)run_scope;
                // Decorations float over the fitted run scope, so unlike ordinary children they
                // never change the glyph flow or the editor's hit-test/caret geometry. Their width
                // resolves from this run scope's text child after Clay lays it out.
                const auto emit_decoration = [&](bool strikethrough) {
                    auto decoration = ctx.element(ElementDecl{
                        .sizing = {SizingAxis::grow(), SizingAxis::fixed(1.0f)},
                        .background_color = run_color,
                        .floating = FloatingConfig{
                            .attach_to = FloatingAttachTo::Parent,
                            .element_attach_point = strikethrough ? FloatingAttachPoint::LeftCenter
                                                                  : FloatingAttachPoint::LeftBottom,
                            .parent_attach_point = strikethrough ? FloatingAttachPoint::LeftCenter
                                                                 : FloatingAttachPoint::LeftBottom,
                            .capture_pointer = false,
                            .clip_to = FloatingClipTo::AttachedParent,
                        },
                    });
                    (void)decoration;
                };
                if (span != nullptr && span->underline) {
                    emit_decoration(/*strikethrough=*/false);
                }
                if (span != nullptr && span->strikethrough) {
                    emit_decoration(/*strikethrough=*/true);
                }
                // Password mode substitutes one mask glyph per scalar — run boundaries, selection,
                // and the caret all stay index-exact because the substitution is 1:1 per character.
                UString run_text;
                if (style.mask_characters) {
                    const ustr mask_glyph{style.mask_glyph};
                    run_text.reserve(mask_glyph.byte_size() * (run_end - run_start));
                    for (usize j = run_start; j < run_end; ++j) {
                        run_text.append(mask_glyph);
                    }
                } else {
                    run_text = line_text.substr(run_start, run_end - run_start);
                }
                ctx.text(run_text.as_ustr(),
                         TextStyle{.color = run_color, .font_id = run_font_id, .font_size = run_font_size,
                                   .wrap_mode = run_wrap_mode});
            }
            if (caret_on_this_line && static_cast<usize>(caret_local) == line_len) {
                emit_composition();
                emit_caret();
            }
            if (line_len == 0 && !placeholder.empty()) {
                ctx.text(placeholder.as_ustr(), TextStyle{.color = style.placeholder_color, .font_id = style.font_id,
                                                          .font_size = style.font_size});
            }
        }

        // Maps a click at `local_x` pixels — already relative to `line_text`'s own rendered left
        // edge, i.e. `pointer.x - ctx.element_bounds(line_element_id(...))->position.x` — to a
        // scalar index within `line_text`. Callers don't need to separately account for horizontal
        // scroll: a scroll container's children are laid out with the current scroll offset already
        // baked into their position (Clay applies childOffset while positioning, not as a separate
        // paint-time transform), so the row's own last-frame bounds already reflect it.
        //
        // Mirrors render_line()'s own password-mask substitution (TextEditStyle::mask_characters):
        // hit-tests against the same per-scalar mask_glyph repetition actually painted, then divides
        // the resulting byte offset (into the masked string, not line_text) by the mask glyph's own
        // fixed UTF-8 byte width to recover a scalar index — masking is always exactly one mask_glyph
        // per scalar, so that division is exact.
        [[nodiscard]] inline usize hit_test_line_scalar(Context &ctx, const TextEditStyle &style,
                                                        const UString &line_text, f32 local_x) {
            const TextStyle text_style{.font_id = style.font_id, .font_size = style.font_size};
            if (!style.mask_characters) {
                const usize byte_offset = ctx.hit_test_text_byte_offset(text_style, line_text.cpp_string_view(), local_x);
                return line_text.scalar_index_of_byte(byte_offset);
            }
            const usize scalar_count = line_text.size();
            const ustr mask_glyph{style.mask_glyph};
            const usize mask_glyph_bytes = mask_glyph.byte_size();
            if (mask_glyph_bytes == 0) {
                return 0;
            }
            UString masked;
            masked.reserve(mask_glyph_bytes * scalar_count);
            for (usize i = 0; i < scalar_count; ++i) {
                masked.append(mask_glyph);
            }
            const usize byte_offset = ctx.hit_test_text_byte_offset(text_style, masked.cpp_string_view(), local_x);
            return std::min(byte_offset / mask_glyph_bytes, scalar_count);
        }

        // One resolved hit against a multi-paragraph buffer: `scalar` is the buffer-wide caret
        // position the click/drag point maps to, and `paragraph_start`/`paragraph_length` are the
        // bounds of whichever paragraph it landed in — triple-click line-select uses the latter two
        // directly instead of re-deriving them.
        struct ParagraphHit {
            usize scalar = 0;
            usize paragraph_start = 0;
            usize paragraph_length = 0;
        };

        // Multi-paragraph hit test: finds which paragraph row (from `paragraphs`, each a
        // (scalar_start, scalar_length) pair from split_paragraphs()) sits closest — by vertical
        // center — to `pointer`, then hit-tests the point's x within that paragraph's own last-
        // committed bounds. Shared by text_area()'s click-to-position and drag-to-extend-selection,
        // which both need exactly this "which row, then where in it" resolution against the same
        // TextEditStyle. std::nullopt when none of `paragraphs`' rows have committed bounds yet
        // (e.g. the very first frame text_area() renders).
        [[nodiscard]] inline std::optional<ParagraphHit>
        hit_test_paragraphs(Context &ctx, const TextEditStyle &style, const UString &text,
                            const vector<pair<usize, usize>> &paragraphs, const UString &widget_id, glm::vec2 pointer) {
            std::optional<usize> best_index;
            f32 best_distance = 0.0f;
            for (usize i = 0; i < paragraphs.size(); ++i) {
                const std::optional<ElementBounds> line_bounds = ctx.element_bounds(line_element_id(widget_id, paragraphs[i].first));
                if (!line_bounds) {
                    continue;
                }
                const f32 center_y = line_bounds->position.y + line_bounds->size.y * 0.5f;
                const f32 distance = std::abs(pointer.y - center_y);
                if (!best_index || distance < best_distance) {
                    best_distance = distance;
                    best_index = i;
                }
            }
            if (!best_index) {
                return std::nullopt;
            }
            const auto &[pstart, plen] = paragraphs[*best_index];
            const std::optional<ElementBounds> line_bounds = ctx.element_bounds(line_element_id(widget_id, pstart));
            if (!line_bounds) {
                return std::nullopt;
            }
            const f32 local_x = pointer.x - line_bounds->position.x;
            const UString paragraph_text = text.substr(pstart, plen);
            return ParagraphHit{
                .scalar = pstart + hit_test_line_scalar(ctx, style, paragraph_text, local_x),
                .paragraph_start = pstart,
                .paragraph_length = plen,
            };
        }

    } // namespace Detail

} // namespace SFT::UI
