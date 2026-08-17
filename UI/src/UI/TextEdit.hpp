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

/// Shared editing engine behind TextInput.hpp's text_input() (single-line) and TextArea.hpp's
/// text_area() (multi-line) — the buffer/caret/selection/clipboard/highlight-cache machinery both
/// widgets need is identical; only how they lay it out on screen (one row vs. wrapped/scrollable
/// paragraphs, Enter-submits vs. Enter-inserts-newline) differs. Same "engine shared, presentation
/// split" shape as Toggle.hpp sharing one ToggleState/ToggleStyle across checkbox/radio_button/
/// switch_toggle.
namespace SFT::UI {

    /// High-level, already-decoded editing intents this frame — the caller (Runtime/Engine
    /// integration) translates raw platform key/modifier state into this before calling
    /// text_input()/text_area(), the same "caller sources it, UI stays backend-agnostic" split
    /// PointerState already uses for pointer input. In particular Ctrl+A/C/X/V (or Cmd+ on macOS)
    /// are expected to already be resolved into SelectAll/Copy/Cut/Paste here, not delivered as
    /// Copy == 'C' + a modifier the widget would have to interpret itself.
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

    /// Which independently useful pieces of editor behavior a widget exposes. Keeping this policy
    /// on TextEditStyle lets a search field, terminal, code editor, or password prompt share the
    /// same editing engine without each growing a fork with subtly different keyboard semantics.
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
        /// Whether this field participates in IME composition at all — on by default. A field that
        /// sets this false never renders TextEditInput::composition_text (Detail::emit_composition())
        /// and never treats input.composing as true for its own Enter/Escape handling (apply_input()
        /// below), even if the OS-level IME is still technically composing at the moment focus
        /// changed. Set false for fields where a composition candidate window would be actively
        /// wrong — a password field, a numeric-only field, or a hotkey-capture field that wants raw
        /// keystrokes. Engine::forward_text_input_state() (Engine/EcsUi.hpp) reads this to decide
        /// whether to tell the OS to keep IME active while this field is focused.
        bool ime_enabled = true;
    };

    /// Maps an input-layer key intent to an editing command. The input layer may therefore expose
    /// its own stable, backend-neutral bindings and a consumer can rebind them per widget without
    /// teaching TextEditState about platform key codes. Entries not listed retain their identity
    /// mapping; a listed entry with enabled=false disables that trigger.
    struct TextEditKeyBinding {
        EditKey trigger = EditKey::Left;
        EditKey command = EditKey::Left;
        bool enabled = true;
    };

    struct TextEditBindings {
        vector<TextEditKeyBinding> keys;

        [[nodiscard]] bool enabled(EditKey trigger) const noexcept;

        [[nodiscard]] EditKey resolve(EditKey trigger) const noexcept;
    };

    struct TextEditInput {
        /// This frame's UTF-8 text-input bytes (from Platform::Windowing::WindowTextInputEvent —
        /// pass its NUL-terminated utf8[32] straight through, e.g. `std::string_view{event.text.
        /// utf8}`), empty if nothing was typed. A plain string_view (not `ustr`, which is
        /// deliberately non-copyable/non-movable and so can't live in an otherwise-copyable struct
        /// like this one) — borrowed, must outlive the text_input()/text_area() call it's passed to.
        string_view typed_text;
        /// Edit-relevant key presses this frame — OS key-repeat naturally produces more than one
        /// Left/Backspace/etc. in a row across frames, which is why this is used as-is rather than
        /// requiring exactly one key per frame.
        vector<EditKey> keys;
        bool shift_held = false;
        /// Ctrl (or Cmd on macOS) — widens Left/Right/Backspace/Delete to word granularity.
        bool word_modifier_held = false;
        /// This frame's in-progress IME composition (preedit) text — e.g. the romaji not yet
        /// converted to kana, or a pinyin sequence before its candidate is picked. Source it from
        /// Engine::InputState::composition_text() (Platform::Windowing::WindowTextEditingEvent's own
        /// doc comment explains why this needs to stay a separate event/field from typed_text: it is
        /// never committed text). Rendered inline at the caret, underlined, and never inserted into
        /// the buffer — only `typed_text` (which arrives once the IME actually commits) becomes part
        /// of text(). Borrowed like typed_text; must outlive the text_input()/text_area() call.
        string_view composition_text;
        /// Whether an IME composition is open this frame — normally `!composition_text.empty()`
        /// (see Engine::InputState::composing()'s own doc comment for the one rule this follows),
        /// kept as its own field so a caller sourcing input some other way than InputState isn't
        /// forced to encode "composing but momentarily empty" as a non-empty sentinel string. While
        /// true, Enter/Escape are treated as belonging to the IME (confirming/cancelling the
        /// composition) rather than the widget's own submit/unfocus behavior — see apply_input()'s
        /// EditKey::Enter/EditKey::Escape cases.
        bool composing = false;
        /// Clipboard bridge for Copy/Cut/Paste — text_input()/text_area() never touch a platform
        /// clipboard API directly (same platform-agnostic reasoning as everything else here). Leave
        /// both null to make Copy/Cut/Paste silent no-ops. See Platform::Windowing::Window::
        /// clipboard_text()/set_clipboard_text() for the SDL3/GLFW-backed implementation this
        /// typically wraps.
        function<UString()> get_clipboard_text;
        function<void(const UString &)> set_clipboard_text;
    };

    /// One inline presentation override over a scalar-index range of the buffer — suitable for
    /// syntax/LSP semantic tokens, Markdown, or a LaTex parser. `font_id` deliberately selects an
    /// application-registered face: bold, italic, math, and code faces vary by application and the
    /// UI renderer does not invent synthetic glyph variants. The scalar range is UTF-8 safe because
    /// UString's editing and substring APIs use the same scalar indices.
    struct RichTextSpan {
        usize scalar_start = 0;
        usize scalar_length = 0;
        Color color{1.0, 1.0, 1.0, 1.0};
        FontId font_id = 0;
        /// false keeps TextEditStyle::font_id; true uses this span's registered face (including
        /// face id zero, if that is the application's chosen bold/italic/math face).
        bool use_font_id = false;
        /// Superscript/subscript and math runs can select a smaller registered size. A true
        /// baseline offset is deliberately not exposed until TextStyle gains a renderer-backed
        /// baseline-offset primitive; pretending with a layout element would corrupt caret and hit
        /// testing geometry.
        f32 font_size_scale = 1.0f;
        bool underline = false;
        bool strikethrough = false;
    };

    /// Optional syntax/Markdown highlighting hook: given the buffer's current text, return the
    /// color runs to paint over it. nullptr (default) renders the whole buffer in
    /// TextEditStyle::text_color, one run, no per-character cost at all.
    ///
    /// Efficiency contract: TextEditState caches the last text this was run against and only calls
    /// it again when the buffer actually changed (see TextEditState::highlighted_spans()) — a
    /// highlighter is free to be as expensive as a real Markdown parse or a tokenizer without
    /// costing anything on frames where the user isn't actively typing (the overwhelming majority
    /// of frames for an idle-but-focused input).
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
        /// Full on/off period is twice this — half visible, half not. <= 0 keeps the caret solidly
        /// visible (no blink).
        f32 caret_blink_seconds = 0.53f;
        f32 transition_seconds = 0.25f;
        ColorBlendSpace color_space = ColorBlendSpace::Oklab;
        EasingFn easing = Easing::cubic_in_out;
        /// Password mode: every character *renders* as `mask_glyph` while the buffer itself keeps
        /// the real text (TextEditState::text() is unaffected — that's what the app submits).
        /// Masking happens per scalar, so caret/selection positions stay exact. The placeholder is
        /// never masked. `mask_glyph` must be a single UTF-8 code point the style's font actually
        /// has — the bundled Maple Mono NF has U+2022 (•).
        bool mask_characters = false;
        const char *mask_glyph = "•";
        /// See Highlighter's own doc comment. RichTextSpan can select registered bold, italic,
        /// code, or math faces as well as color and baseline/size for super/subscripts.
        Highlighter highlighter = nullptr;
        TextEditFeatures features{};
        TextEditBindings bindings{};
        /// false (default): lines never word-wrap — a line wider than the box scrolls
        /// horizontally instead (Detail::render_line's row is Fit-width inside the caller's own
        /// ClipConfig::horizontal box), the same convention a code editor or a plain single-line
        /// input uses. true opts into word-wrap, but only takes full effect on a line with no
        /// highlighter match, active selection, or caret on it — see Detail::render_line's own doc
        /// comment for why a line that has to split into multiple runs can't wrap coherently across
        /// them regardless of this flag (Clay only wraps within a single text element).
        bool wrap_lines = false;
    };

    namespace Detail {

        [[nodiscard]] UString strip_newlines(const UString &text);

    } // namespace Detail

    /// Persistent per-input state: the text buffer itself, caret/selection, focus, and the
    /// animation/highlight caches text_input()/text_area() need — one instance per logical input,
    /// kept alive across frames by the caller, same convention as every other *State in this
    /// package (ButtonState, ToggleState, DropdownState). The buffer/caret/selection mutators below
    /// are public so a caller building fully custom text-editing UI (not going through
    /// text_input()/text_area() at all) can still reuse this engine directly — same "usable
    /// standalone" reasoning as Button.hpp's ColorTransition.
    class TextEditState {
      public:
        [[nodiscard]] const UString &text() const noexcept;

        /// Replaces the whole buffer and resets caret/selection to the end — for programmatic
        /// changes (loading a saved value, resetting a form). Editing through
        /// text_input()/text_area() itself never calls this.
        void set_text(UString text) noexcept;

        [[nodiscard]] usize caret() const noexcept;
        [[nodiscard]] bool has_selection() const noexcept;
        [[nodiscard]] usize selection_min() const noexcept;
        [[nodiscard]] usize selection_max() const noexcept;
        [[nodiscard]] UString selected_text() const;

        [[nodiscard]] bool focused() const noexcept;
        void set_focused(bool focused) noexcept;

        /// This frame's in-progress IME composition text, captured by apply_input() from
        /// TextEditInput::composition_text — render_line() paints it inline (underlined) at the
        /// caret. Stored as an owned string rather than forwarding TextEditInput's borrowed
        /// string_view: TextEditState outlives any single frame's apply_input() call, and render_line()
        /// (called later the same frame, but still a second call) has no way to know whether a
        /// stored view's backing buffer is still alive — same reasoning as text_/highlight_cache_key_
        /// below being owned rather than borrowed.
        [[nodiscard]] string_view composition_text() const noexcept;

        void insert(const UString &value);

        void delete_selection();

        void backspace(bool word);

        void delete_forward(bool word);

        /// `direction` < 0 is Left, > 0 is Right. Collapses an existing selection to its
        /// corresponding edge instead of moving past it when `extend` is false — the same
        /// convention every text editor uses (Left with a selection lands you at its start, not one
        /// further left).
        void move_caret(isize direction, bool extend, bool word) noexcept;

        void move_to_start(bool extend) noexcept;

        void move_to_end(bool extend) noexcept;

        void select_all() noexcept;

        /// Selects the word containing `scalar_index`, via the same word-boundary table
        /// Ctrl+Left/Right word-jump already uses (word_boundary_before/after) — double-click's
        /// selection target.
        void select_word_at(usize scalar_index) noexcept;

        /// Plain clamped range setter — triple-click's selection target (the widget already knows
        /// the clicked paragraph's own [start, start+length) bounds, no word-boundary lookup needed).
        void select_range(usize start, usize end) noexcept;

        /// Classifies one click event as continuing (or not) a multi-click gesture — timed
        /// independently of the OS's own double-click setting, the same "self-timed, not
        /// platform-synced" choice caret_blink_on() already makes for the blink rate. Returns the
        /// resulting streak (1 = single, 2 = double, capped at 3 = triple-or-more); callers
        /// typically treat 2 as "select word" and >=3 as "select line/paragraph".
        ///
        /// `allow_multi_click=false` (a shift+click) always resets to a fresh streak of 1 — a
        /// shift+click is never itself part of a multi-click gesture, and must not let a later
        /// plain click spuriously continue a streak that started before it.
        u8 register_click(glm::vec2 position, bool allow_multi_click) noexcept;

        /// Clamped to the buffer's own bounds — for placing the caret from something other than a
        /// keypress, e.g. text_input()/text_area()'s own click-to-position handling (Detail::
        /// hit_test_line_scalar(), further down this file).
        void set_caret_to(usize scalar_index, bool extend) noexcept;

        struct ApplyResult {
            bool changed = false;
            /// Enter pressed in single-line mode (text_input()) — text_area() never sets this,
            /// Enter inserts a newline there instead. See apply_input()'s own doc comment.
            bool submitted = false;
        };

        /// Applies one frame's typed text + edit keys in order (typed text first, matching how a
        /// real keystroke both types a character and can't also be a control key in the same
        /// event). `multiline` controls two things: whether Enter inserts '\n' (true) or reports
        /// ApplyResult::submitted (false), and whether typed/pasted text has embedded newlines
        /// stripped (single-line inputs strip them; multi-line inputs keep them). No-ops entirely
        /// (returns a default-constructed ApplyResult) when not focused(). EditKey::Up/Down are
        /// deliberately ignored here — vertical caret movement needs to know about line layout,
        /// which only text_area() has, so text_area() intercepts and handles those two itself
        /// before calling this.
        ApplyResult apply_input(const TextEditInput &input, bool multiline, const TextEditFeatures &features = {},
                                const TextEditBindings &bindings = {});

        /// Advances the idle/hovered/focused/disabled color blend and the caret blink clock — call
        /// once per frame regardless of whether apply_input() changed anything.
        void update_visual(bool hovered, bool enabled, const TextEditStyle &style, f32 delta_seconds) noexcept;

        [[nodiscard]] Color current_color() const noexcept;

        [[nodiscard]] bool caret_blink_on(f32 blink_seconds) const noexcept;

        /// Cached highlighter output — see Highlighter's own doc comment for the efficiency
        /// contract this implements (only re-runs `highlighter` when text() changed since the last
        /// call). Empty (and cheap: one string comparison, no cache touch) when `highlighter` is
        /// null.
        [[nodiscard]] const vector<RichTextSpan> &highlighted_spans(const Highlighter &highlighter) const;

      private:
        [[nodiscard]] usize word_boundary_before(usize scalar_index) const;

        [[nodiscard]] usize word_boundary_after(usize scalar_index) const;

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

        /// Splits `text` into paragraphs at each '\n' — (scalar_start, scalar_length) pairs,
        /// exclusive of the separator itself, always at least one entry (an empty buffer yields one
        /// empty paragraph). TextArea.hpp uses this both to lay out one row per paragraph and, since
        /// it's the same "line" granularity TextEditState::apply_input() leaves EditKey::Up/Down
        /// for text_area() to handle itself, to find which paragraph the caret is in for vertical
        /// caret movement — see text_area()'s own doc comment for why paragraphs (hard line breaks)
        /// rather than wrapped visual lines is the granularity here.
        [[nodiscard]] vector<pair<usize, usize>> split_paragraphs(const UString &text);

        /// Stable per-widget id for the caret bar element render_line() creates on whichever line
        /// currently contains it — text_input()/text_area() use the same id to ask
        /// Context::scroll_into_view() to keep it on screen. `widget_id` is the text_input()/
        /// text_area() call's own ElementDecl::id.
        [[nodiscard]] UString caret_element_id(const UString &widget_id);

        /// Stable per-line id for render_line()'s own row element — text_input() (one line, always
        /// line_scalar_offset 0) and text_area() (one per paragraph, keyed by that paragraph's own
        /// scalar start) use this so a click can later look up *this exact line's* on-screen bounds
        /// via Context::element_bounds() and translate the click into a scalar index — see
        /// hit_test_line_scalar()'s own doc comment.
        [[nodiscard]] UString line_element_id(const UString &widget_id, usize line_scalar_offset);

        [[nodiscard]] const RichTextSpan *span_for_run(const vector<RichTextSpan> &spans,
                                                               usize global_scalar_index) noexcept;

        /// Renders one visual line/paragraph of `state.text()` as its own row element (opens and
        /// fills it — callers just call this once per line, they don't pre-open a row of their
        /// own). `line_text` is that line's own substring, `line_scalar_offset` is where it starts
        /// within the whole buffer (so state.caret()/selection/highlighted_spans(), all in whole-
        /// buffer scalar indices, can be mapped onto it).
        ///
        /// style.wrap_lines is false by default: every run renders at its natural unwrapped width
        /// (TextWrapMode::None) inside a Fit-width row that's free to exceed the box — the caller's
        /// own outer ClipConfig::horizontal scrolls it instead, the same convention a code editor or
        /// a plain single-line input uses. Set style.wrap_lines = true to word-wrap instead, which
        /// takes full effect only on a line with no highlighter match, active selection, or caret on
        /// it (rendered as a single ordinary Clay text element, wrapped normally within the row's
        /// own bounded `grow()` width) — any other line still has to split into multiple sibling
        /// elements (one per highlight/selection/caret run, so selection highlighting is just that
        /// run's own background_color and the caret is a plain 2px-wide element spliced into the
        /// flow — placed pixel-correctly by Clay's ordinary layout with no glyph-shaping or text-
        /// measurement code needed on our side at all), and Clay only ever wraps text within a
        /// single text element: letting several sibling Fit elements plus the caret's own fixed
        /// width all compete in Clay's width-fitting/shrink pass corrupts each run's *independent*
        /// wrap far worse than not wrapping at all (a shrunk run's wrap boundary has nothing to do
        /// with where the row itself would have wrapped), so those lines fall back to the same
        /// unwrapped/scrolled rendering regardless of wrap_lines. See text_area()'s own doc comment
        /// for the user-visible summary of this tradeoff.
        void render_line(Context &ctx, const UString &line_text, usize line_scalar_offset,
                                const TextEditStyle &style, const TextEditState &state, const UString &widget_id,
                                const UString &placeholder = UString{});

        /// Maps a click at `local_x` pixels — already relative to `line_text`'s own rendered left
        /// edge, i.e. `pointer.x - ctx.element_bounds(line_element_id(...))->position.x` — to a
        /// scalar index within `line_text`. Callers don't need to separately account for horizontal
        /// scroll: a scroll container's children are laid out with the current scroll offset already
        /// baked into their position (Clay applies childOffset while positioning, not as a separate
        /// paint-time transform), so the row's own last-frame bounds already reflect it.
        ///
        /// Mirrors render_line()'s own password-mask substitution (TextEditStyle::mask_characters):
        /// hit-tests against the same per-scalar mask_glyph repetition actually painted, then divides
        /// the resulting byte offset (into the masked string, not line_text) by the mask glyph's own
        /// fixed UTF-8 byte width to recover a scalar index — masking is always exactly one mask_glyph
        /// per scalar, so that division is exact.
        [[nodiscard]] usize hit_test_line_scalar(Context &ctx, const TextEditStyle &style,
                                                        const UString &line_text, f32 local_x);

        /// One resolved hit against a multi-paragraph buffer: `scalar` is the buffer-wide caret
        /// position the click/drag point maps to, and `paragraph_start`/`paragraph_length` are the
        /// bounds of whichever paragraph it landed in — triple-click line-select uses the latter two
        /// directly instead of re-deriving them.
        struct ParagraphHit {
            usize scalar = 0;
            usize paragraph_start = 0;
            usize paragraph_length = 0;
        };

        /// Multi-paragraph hit test: finds which paragraph row (from `paragraphs`, each a
        /// (scalar_start, scalar_length) pair from split_paragraphs()) sits closest — by vertical
        /// center — to `pointer`, then hit-tests the point's x within that paragraph's own last-
        /// committed bounds. Shared by text_area()'s click-to-position and drag-to-extend-selection,
        /// which both need exactly this "which row, then where in it" resolution against the same
        /// TextEditStyle. std::nullopt when none of `paragraphs`' rows have committed bounds yet
        /// (e.g. the very first frame text_area() renders).
        [[nodiscard]] std::optional<ParagraphHit>
        hit_test_paragraphs(Context &ctx, const TextEditStyle &style, const UString &text,
                            const vector<pair<usize, usize>> &paragraphs, const UString &widget_id, glm::vec2 pointer);

    } // namespace Detail

} // namespace SFT::UI
