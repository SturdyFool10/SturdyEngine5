#pragma once

#include <Foundation/Foundation.hpp>

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

#include <Renderer/Text/Text.hpp>

#include <Renderer/UI/Button.hpp>
#include <Renderer/UI/Context.hpp>
#include <Renderer/UI/Style.hpp>

using std::function;
using std::pair;
using std::set;
using std::string_view;
using std::vector;


namespace SFT::UI {


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


        bool ime_enabled = true;
    };


    struct TextEditKeyBinding {
        EditKey trigger = EditKey::Left;
        EditKey command = EditKey::Left;
        bool enabled = true;
    };

    struct TextEditBindings {
        vector<TextEditKeyBinding> keys;

        /// Performs the enabled operation for `TextEditBindings` using the supplied arguments.
        ///
        /// @param trigger `trigger` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool enabled(EditKey trigger) const noexcept;

        /// Resolves the requested value into the concrete value used by the engine.
        ///
        /// @param trigger `trigger` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] EditKey resolve(EditKey trigger) const noexcept;
    };

    struct TextEditInput {


        string_view typed_text;


        vector<EditKey> keys;
        bool shift_held = false;

        bool word_modifier_held = false;


        string_view composition_text;


        bool composing = false;


        function<UString()> get_clipboard_text;
        function<void(const UString &)> set_clipboard_text;
    };


    struct RichTextSpan {
        usize scalar_start = 0;
        usize scalar_length = 0;
        Color color{1.0, 1.0, 1.0, 1.0};
        FontId font_id = 0;


        bool use_font_id = false;


        f32 font_size_scale = 1.0f;
        bool underline = false;
        bool strikethrough = false;
    };


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


        f32 caret_blink_seconds = 0.53f;
        f32 transition_seconds = 0.25f;
        ColorBlendSpace color_space = ColorBlendSpace::Oklab;
        EasingFn easing = Easing::cubic_in_out;


        bool mask_characters = false;
        const char *mask_glyph = "•";


        Highlighter highlighter = nullptr;
        TextEditFeatures features{};
        TextEditBindings bindings{};


        bool wrap_lines = false;
    };

    namespace Detail {

        /// Performs the strip newlines operation using the supplied arguments.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString strip_newlines(const UString &text);

    } // namespace Detail


    class TextEditState {
      public:
        /// Returns the current or globally available text value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const UString &text() const noexcept;


        /// Sets the text for this `TextEditState`.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_text(UString text) noexcept;

        /// Returns the current or globally available caret value.
        ///
        /// @return Returns the current caret value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize caret() const noexcept;
        /// Reports whether this `TextEditState` has selection.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool has_selection() const noexcept;
        /// Returns the current or globally available selection min value.
        ///
        /// @return Returns the current selection min value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize selection_min() const noexcept;
        /// Returns the current or globally available selection max value.
        ///
        /// @return Returns the current selection max value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize selection_max() const noexcept;
        /// Returns the current or globally available selected text value.
        ///
        /// @return Returns the current selected text value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString selected_text() const;

        /// Returns the current or globally available focused value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool focused() const noexcept;
        /// Sets the focused for this `TextEditState`.
        ///
        /// @param focused `focused` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_focused(bool focused) noexcept;


        /// Returns the current or globally available composition text value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] string_view composition_text() const noexcept;

        /// Inserts the supplied value or range at the requested position.
        ///
        /// @param value Value consumed by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void insert(const UString &value);

        /// Performs the delete selection operation for `TextEditState` using the supplied arguments.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void delete_selection();

        /// Performs the backspace operation for `TextEditState` using the supplied arguments.
        ///
        /// @param word `word` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void backspace(bool word);

        /// Performs the delete forward operation for `TextEditState` using the supplied arguments.
        ///
        /// @param word `word` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void delete_forward(bool word);


        /// Moves caret using the supplied arguments and current state.
        ///
        /// @param direction `direction` value used by the operation.
        /// @param extend `extend` value used by the operation.
        /// @param word `word` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void move_caret(isize direction, bool extend, bool word) noexcept;

        /// Moves to start using the supplied arguments and current state.
        ///
        /// @param extend `extend` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void move_to_start(bool extend) noexcept;

        /// Moves to end using the supplied arguments and current state.
        ///
        /// @param extend `extend` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void move_to_end(bool extend) noexcept;

        /// Selects all that best satisfies the supplied requirements.
        ///
        /// @note This function does not throw exceptions.
        void select_all() noexcept;


        /// Selects word at that best satisfies the supplied requirements.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        ///
        /// @note This function does not throw exceptions.
        void select_word_at(usize scalar_index) noexcept;


        /// Selects range that best satisfies the supplied requirements.
        ///
        /// @param start First position or element included in the operation.
        /// @param end End boundary for the operation; where applicable this is one-past-the-last element.
        ///
        /// @note This function does not throw exceptions.
        void select_range(usize start, usize end) noexcept;


        /// Registers click using the supplied arguments and current state.
        ///
        /// @param position `position` value used by the operation.
        /// @param allow_multi_click `allow_multi_click` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        u8 register_click(glm::vec2 position, bool allow_multi_click) noexcept;


        /// Sets the caret to for this `TextEditState`.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        /// @param extend `extend` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_caret_to(usize scalar_index, bool extend) noexcept;

        struct ApplyResult {
            bool changed = false;


            bool submitted = false;
        };


        /// Applies input using the supplied arguments and current state.
        ///
        /// @param input `input` value used by the operation.
        /// @param multiline `multiline` value used by the operation.
        /// @param features `features` value used by the operation.
        /// @param bindings `bindings` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        ApplyResult apply_input(const TextEditInput &input, bool multiline, const TextEditFeatures &features = {},
                                const TextEditBindings &bindings = {});


        /// Updates visual from the supplied values.
        ///
        /// @param hovered `hovered` value used by the operation.
        /// @param enabled Whether the associated behavior is enabled.
        /// @param style `style` value used by the operation.
        /// @param delta_seconds `delta_seconds` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void update_visual(bool hovered, bool enabled, const TextEditStyle &style, f32 delta_seconds) noexcept;

        /// Returns the current or globally available current color value.
        ///
        /// @return Returns the current current color value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Color current_color() const noexcept;

        /// Performs the caret blink on operation for `TextEditState` using the supplied arguments.
        ///
        /// @param blink_seconds `blink_seconds` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool caret_blink_on(f32 blink_seconds) const noexcept;


        /// Performs the highlighted spans operation for `TextEditState` using the supplied arguments.
        ///
        /// @param highlighter `highlighter` value used by the operation.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] const vector<RichTextSpan> &highlighted_spans(const Highlighter &highlighter) const;

      private:
        /// Performs the word boundary before operation for `TextEditState` using the supplied arguments.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize word_boundary_before(usize scalar_index) const;

        /// Performs the word boundary after operation for `TextEditState` using the supplied arguments.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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


        /// Splits paragraphs using the supplied arguments and current state.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] vector<pair<usize, usize>> split_paragraphs(const UString &text);


        /// Performs the caret element ID operation using the supplied arguments.
        ///
        /// @param widget_id Identifier of the target object or resource.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString caret_element_id(const UString &widget_id);


        /// Performs the line element ID operation using the supplied arguments.
        ///
        /// @param widget_id Identifier of the target object or resource.
        /// @param line_scalar_offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString line_element_id(const UString &widget_id, usize line_scalar_offset);

        /// Performs the span for run operation using the supplied arguments.
        ///
        /// @param spans `spans` value used by the operation.
        /// @param global_scalar_index Zero-based index of the target element or entry.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const RichTextSpan *span_for_run(const vector<RichTextSpan> &spans,
                                                               usize global_scalar_index) noexcept;


        /// Renders line using the current rendering state.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param line_text `line_text` value used by the operation.
        /// @param line_scalar_offset Offset from the beginning of the relevant range or buffer.
        /// @param style `style` value used by the operation.
        /// @param state `state` value used by the operation.
        /// @param widget_id Identifier of the target object or resource.
        /// @param placeholder `placeholder` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void render_line(Context &ctx, const UString &line_text, usize line_scalar_offset,
                                const TextEditStyle &style, const TextEditState &state, const UString &widget_id,
                                const UString &placeholder = UString{});


        /// Performs the hit test line scalar operation using the supplied arguments.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param style `style` value used by the operation.
        /// @param line_text `line_text` value used by the operation.
        /// @param local_x `local_x` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize hit_test_line_scalar(Context &ctx, const TextEditStyle &style,
                                                        const UString &line_text, f32 local_x);


        struct ParagraphHit {
            usize scalar = 0;
            usize paragraph_start = 0;
            usize paragraph_length = 0;
        };


        /// Performs the hit test paragraphs operation for `Detail` using the supplied arguments.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param style `style` value used by the operation.
        /// @param text Text consumed by the operation.
        /// @param paragraphs `paragraphs` value used by the operation.
        /// @param widget_id Identifier of the target object or resource.
        /// @param pointer Pointer to the object or storage used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        [[nodiscard]] std::optional<ParagraphHit>
        hit_test_paragraphs(Context &ctx, const TextEditStyle &style, const UString &text,
                            const vector<pair<usize, usize>> &paragraphs, const UString &widget_id, glm::vec2 pointer);

    } // namespace Detail

} // namespace SFT::UI
