#include <Renderer/UI/TextEdit.hpp>

#include <algorithm>
#include <optional>


namespace SFT::UI {

    /// Performs the enabled operation for `UI` using the supplied arguments.
    ///
    /// @param trigger `trigger` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool TextEditBindings::enabled(EditKey trigger) const noexcept {
        for (const TextEditKeyBinding &binding : keys) {
            if (binding.trigger == trigger) {
                return binding.enabled;
            }
        }
        return true;
    }

    /// Resolves the requested value into the concrete value used by the engine.
    ///
    /// @param trigger `trigger` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    EditKey TextEditBindings::resolve(EditKey trigger) const noexcept {
        for (const TextEditKeyBinding &binding : keys) {
            if (binding.trigger == trigger) {
                return binding.command;
            }
        }
        return trigger;
    }

    /// Returns the current or globally available text value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    const UString &TextEditState::text() const noexcept { return text_; }

    /// Sets the text for this `UI`.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void TextEditState::set_text(UString text) noexcept {
        text_ = std::move(text);
        caret_ = text_.size();
        selection_anchor_ = caret_;
    }

    /// Returns the current or globally available caret value.
    ///
    /// @return Returns the current caret value.
    /// @note This function does not throw exceptions.
    usize TextEditState::caret() const noexcept { return caret_; }

    /// Reports whether this `UI` has selection.
    ///
    /// @return Returns the current has selection value.
    /// @note This function does not throw exceptions.
    bool TextEditState::has_selection() const noexcept { return selection_anchor_ != caret_; }

    /// Returns the current or globally available selection min value.
    ///
    /// @return Returns the current selection min value.
    /// @note This function does not throw exceptions.
    usize TextEditState::selection_min() const noexcept { return std::min(caret_, selection_anchor_); }

    /// Returns the current or globally available selection max value.
    ///
    /// @return Returns the current selection max value.
    /// @note This function does not throw exceptions.
    usize TextEditState::selection_max() const noexcept { return std::max(caret_, selection_anchor_); }

    /// Returns the current or globally available selected text value.
    ///
    /// @return Returns the current selected text value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString TextEditState::selected_text() const {
        return has_selection() ? text_.substr(selection_min(), selection_max() - selection_min()) : UString{};
    }

    /// Returns the current or globally available focused value.
    ///
    /// @return Returns the current focused value.
    /// @note This function does not throw exceptions.
    bool TextEditState::focused() const noexcept { return focused_; }

    /// Sets the focused for this `UI`.
    ///
    /// @param focused `focused` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void TextEditState::set_focused(bool focused) noexcept { focused_ = focused; }

    /// Returns the current or globally available composition text value.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    string_view TextEditState::composition_text() const noexcept { return composition_text_; }

    /// Inserts the supplied value or range at the requested position.
    ///
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void TextEditState::insert(const UString &value) {
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

    /// Returns the current or globally available delete selection value.
    ///
    /// @return Returns the current delete selection value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void TextEditState::delete_selection() {
        if (!has_selection()) {
            return;
        }
        const usize start = selection_min();
        text_.erase(start, selection_max() - start);
        caret_ = start;
        selection_anchor_ = start;
    }

    /// Performs the backspace operation for `UI` using the supplied arguments.
    ///
    /// @param word `word` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void TextEditState::backspace(bool word) {
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

    /// Performs the delete forward operation for `UI` using the supplied arguments.
    ///
    /// @param word `word` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void TextEditState::delete_forward(bool word) {
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

    /// Moves caret using the supplied arguments and current state.
    ///
    /// @param direction `direction` value used by the operation.
    /// @param extend `extend` value used by the operation.
    /// @param word `word` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void TextEditState::move_caret(isize direction, bool extend, bool word) noexcept {
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

    /// Moves to start using the supplied arguments and current state.
    ///
    /// @param extend `extend` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void TextEditState::move_to_start(bool extend) noexcept {
        caret_ = 0;
        if (!extend) {
            selection_anchor_ = caret_;
        }
    }

    /// Moves to end using the supplied arguments and current state.
    ///
    /// @param extend `extend` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void TextEditState::move_to_end(bool extend) noexcept {
        caret_ = text_.size();
        if (!extend) {
            selection_anchor_ = caret_;
        }
    }

    /// Selects all that best satisfies the supplied requirements.
    ///
    /// @return Returns the current select all value.
    /// @note This function does not throw exceptions.
    void TextEditState::select_all() noexcept {
        selection_anchor_ = 0;
        caret_ = text_.size();
    }

    /// Selects word at that best satisfies the supplied requirements.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void TextEditState::select_word_at(usize scalar_index) noexcept {
        if (text_.empty()) {
            caret_ = 0;
            selection_anchor_ = 0;
            return;
        }
        const usize probe = std::min(scalar_index, text_.size() - 1);
        selection_anchor_ = word_boundary_before(probe + 1);
        caret_ = word_boundary_after(probe);
    }

    /// Selects range that best satisfies the supplied requirements.
    ///
    /// @param start First position or element included in the operation.
    /// @param end End boundary for the operation; where applicable this is one-past-the-last element.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void TextEditState::select_range(usize start, usize end) noexcept {
        selection_anchor_ = std::min(start, text_.size());
        caret_ = std::min(end, text_.size());
    }

    /// Registers click using the supplied arguments and current state.
    ///
    /// @param position `position` value used by the operation.
    /// @param allow_multi_click `allow_multi_click` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    u8 TextEditState::register_click(glm::vec2 position, bool allow_multi_click) noexcept {
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

    /// Sets the caret to for this `UI`.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    /// @param extend `extend` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void TextEditState::set_caret_to(usize scalar_index, bool extend) noexcept {
        caret_ = std::min(scalar_index, text_.size());
        if (!extend) {
            selection_anchor_ = caret_;
        }
    }

    /// Applies input using the supplied arguments and current state.
    ///
    /// @param input `input` value used by the operation.
    /// @param multiline `multiline` value used by the operation.
    /// @param features `features` value used by the operation.
    /// @param bindings `bindings` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    TextEditState::ApplyResult TextEditState::apply_input(const TextEditInput &input, bool multiline, const TextEditFeatures &features,
                            const TextEditBindings &bindings) {
        ApplyResult result{};
        if (!focused_) {
            composition_text_.clear();
            return result;
        }


        if (features.ime_enabled) {
            composition_text_.assign(input.composition_text);
        } else {
            composition_text_.clear();
        }

        // Undo bookkeeping: remember the state before this frame's edits; it becomes an undo step if they change the
        // text (or extends the current typing step).
        const bool has_input = !input.typed_text.empty() || !input.keys.empty();
        const bool only_typing = !input.typed_text.empty() && input.keys.empty();
        const bool typed_whitespace = std::ranges::any_of(input.typed_text, [](char c) { return c == ' ' || c == '\t'; });
        std::optional<EditSnapshot> before;
        if (has_input) {
            before = snapshot();
        }

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


                break;
            case EditKey::Enter:


                if (features.ime_enabled && input.composing) {
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


                if (features.ime_enabled && input.composing) {
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
            case EditKey::Undo:
                if (features.typing || features.deletion) {
                    if (!undo_stack_.empty()) {
                        redo_stack_.push_back(snapshot());
                        restore(undo_stack_.back());
                        undo_stack_.pop_back();
                        result.changed = true;
                    }
                    coalescing_typing_ = false;
                    before.reset(); // the history move is not itself an undoable edit
                }
                break;
            case EditKey::Redo:
                if (features.typing || features.deletion) {
                    if (!redo_stack_.empty()) {
                        undo_stack_.push_back(snapshot());
                        restore(redo_stack_.back());
                        redo_stack_.pop_back();
                        result.changed = true;
                    }
                    coalescing_typing_ = false;
                    before.reset();
                }
                break;
            }
        }

        if (before && text_ != before->text) {
            if (!(only_typing && coalescing_typing_)) {
                undo_stack_.push_back(std::move(*before));
                if (undo_stack_.size() > max_undo_steps) {
                    undo_stack_.erase(undo_stack_.begin());
                }
            }
            redo_stack_.clear();
            coalescing_typing_ = only_typing && !typed_whitespace;
        } else if (before && !input.keys.empty()) {
            coalescing_typing_ = false; // navigation or selection ends a typing run
        }

        if (result.changed) {
            blink_elapsed_ = 0.0f;
        }
        return result;
    }

    TextEditState::EditSnapshot TextEditState::snapshot() const {
        return EditSnapshot{text_, caret_, selection_anchor_};
    }

    void TextEditState::restore(const EditSnapshot &snapshot) {
        text_ = snapshot.text;
        const usize length = text_.size();
        caret_ = std::min(snapshot.caret, length);
        selection_anchor_ = std::min(snapshot.selection_anchor, length);
    }

    /// Updates visual from the supplied values.
    ///
    /// @param hovered `hovered` value used by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    /// @param style `style` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void TextEditState::update_visual(bool hovered, bool enabled, const TextEditStyle &style, f32 delta_seconds) noexcept {
        const Color &target = !enabled ? style.disabled : focused_ ? style.focused : hovered ? style.hovered : style.idle;
        color_.update(target, delta_seconds, style.transition_seconds, style.color_space, style.easing);
        blink_elapsed_ += delta_seconds;
        time_since_last_click_ += delta_seconds;
    }

    /// Returns the current or globally available current color value.
    ///
    /// @return Returns the current current color value.
    /// @note This function does not throw exceptions.
    Color TextEditState::current_color() const noexcept { return color_.current(); }

    /// Performs the caret blink on operation for `UI` using the supplied arguments.
    ///
    /// @param blink_seconds `blink_seconds` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool TextEditState::caret_blink_on(f32 blink_seconds) const noexcept {
        if (blink_seconds <= 0.0f) {
            return true;
        }
        const f32 period = blink_seconds * 2.0f;
        const f32 phase = std::fmod(blink_elapsed_, period);
        return phase < blink_seconds;
    }

    /// Performs the highlighted spans operation for `UI` using the supplied arguments.
    ///
    /// @param highlighter `highlighter` value used by the operation.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    const vector<RichTextSpan> &TextEditState::highlighted_spans(const Highlighter &highlighter, u64 revision) const {
        if (!highlighter) {
            highlight_cache_spans_.clear();
            highlight_cache_key_.clear();
            return highlight_cache_spans_;
        }
        if (highlight_cache_key_ != text_ || highlight_cache_revision_ != revision) {
            highlight_cache_key_ = text_;
            highlight_cache_revision_ = revision;
            highlight_cache_spans_ = highlighter(text_);
        }
        return highlight_cache_spans_;
    }

    void TextEditState::store_visual_layout(usize paragraph_start, VisualLayout layout) const {
        visual_layouts_[paragraph_start] = std::move(layout);
    }

    const TextEditState::VisualLayout *TextEditState::visual_layout(usize paragraph_start) const {
        const auto found = visual_layouts_.find(paragraph_start);
        return found == visual_layouts_.end() ? nullptr : &found->second;
    }

    /// Performs the word boundary before operation for `UI` using the supplied arguments.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize TextEditState::word_boundary_before(usize scalar_index) const {
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

    /// Performs the word boundary after operation for `UI` using the supplied arguments.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize TextEditState::word_boundary_after(usize scalar_index) const {
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

} // namespace SFT::UI

namespace SFT::UI::Detail {

    /// Performs the strip newlines operation for `Detail` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString strip_newlines(const UString &text) {
        UString result;
        for (char32_t scalar : text.codepoints()) {
            if (scalar != U'\n' && scalar != U'\r') {
                result.append(scalar);
            }
        }
        return result;
    }

    /// Splits paragraphs using the supplied arguments and current state.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    vector<pair<usize, usize>> split_paragraphs(const UString &text) {
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

    /// Performs the caret element ID operation for `Detail` using the supplied arguments.
    ///
    /// @param widget_id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString caret_element_id(const UString &widget_id) {
        return UString{widget_id.cpp_string() + "#caret"};
    }

    /// Performs the line element ID operation for `Detail` using the supplied arguments.
    ///
    /// @param widget_id Identifier of the target object or resource.
    /// @param line_scalar_offset Offset from the beginning of the relevant range or buffer.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString line_element_id(const UString &widget_id, usize line_scalar_offset) {
        return UString{widget_id.cpp_string() + "#line" + std::to_string(line_scalar_offset)};
    }

    /// Performs the span for run operation for `Detail` using the supplied arguments.
    ///
    /// @param spans `spans` value used by the operation.
    /// @param global_scalar_index Zero-based index of the target element or entry.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function does not throw exceptions.
    const RichTextSpan *span_for_run(const vector<RichTextSpan> &spans,
                                                           usize global_scalar_index) noexcept {
        for (const RichTextSpan &span : spans) {
            if (global_scalar_index >= span.scalar_start && global_scalar_index < span.scalar_start + span.scalar_length) {
                return &span;
            }
        }
        return nullptr;
    }

    UString visual_row_element_id(const UString &widget_id, usize paragraph_offset, usize row) {
        return UString{widget_id.cpp_string() + "#vl" + std::to_string(paragraph_offset) + "." + std::to_string(row)};
    }

    /// The font, size and colour a run draws with: the widget's own, overridden by the span covering it.
    TextStyle run_text_style(const TextEditStyle &style, const RichTextSpan *span, TextWrapMode wrap) {
        const f32 font_scale = span != nullptr ? span->font_size_scale : 1.0f;
        TextStyle out{
            .color = span != nullptr ? span->color : style.text_color,
            .font_id = span != nullptr && span->use_font_id ? span->font_id : style.font_id,
            .font_size = static_cast<u16>(std::clamp(std::round(static_cast<f32>(style.font_size) * std::max(font_scale, 0.1f)), 1.0f,
                                                     static_cast<f32>(std::numeric_limits<u16>::max()))),
            .letter_spacing = style.letter_spacing,
            .line_height = style.line_height,
            .wrap_mode = wrap,
        };
        if (span != nullptr && (span->underline || span->strikethrough)) {
            TextDecorationLine lines = TextDecorationLine::None;
            if (span->underline) {
                lines = lines | TextDecorationLine::Underline;
            }
            if (span->strikethrough) {
                lines = lines | TextDecorationLine::LineThrough;
            }
            out.decoration = TextDecoration{
                .lines = lines,
                .color = span->use_line_color ? std::optional<Color>{span->line_color} : std::nullopt,
                .style = span->line_style,
                .thickness = span->line_thickness,
            };
        }
        return out;
    }

    /// Whether a scalar is where a wrapped line may break after.
    bool is_break_space(char32_t scalar) {
        return scalar == U' ' || scalar == U'\t' || scalar == 0x3000;
    }

    /// The scalars of `text` (decoded UTF-8; malformed bytes count as one scalar each).
    vector<char32_t> decode_scalars(const std::string &text) {
        vector<char32_t> scalars;
        scalars.reserve(text.size());
        for (usize i = 0; i < text.size();) {
            const auto lead = static_cast<unsigned char>(text[i]);
            usize length = 1;
            char32_t value = lead;
            if (lead >= 0xF0) {
                length = 4;
                value = lead & 0x07;
            } else if (lead >= 0xE0) {
                length = 3;
                value = lead & 0x0F;
            } else if (lead >= 0xC0) {
                length = 2;
                value = lead & 0x1F;
            }
            if (i + length > text.size()) {
                length = 1;
                value = lead;
            }
            for (usize k = 1; k < length; ++k) {
                value = (value << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
            }
            scalars.push_back(value);
            i += length;
        }
        return scalars;
    }

    /// One span-style piece of a paragraph: a range drawn in one font, size and colour.
    struct StylePiece {
        usize start = 0;
        usize end = 0;
        const RichTextSpan *span = nullptr;
        TextStyle text_style;
    };

    /// Where a paragraph breaks into visual lines for `available_width`: greedy, at the Unicode line-break opportunities (UAX #14, so
    /// CJK text and hyphens break where they should, not only at spaces). A unit (the text between two opportunities) moves to the next
    /// line when it no longer fits; its trailing spaces hang past the edge. A unit wider than a whole line is broken between
    /// characters. Units keep together across a change of style. Offsets are local to the paragraph. A width of zero or less
    /// means "unknown": no breaks.
    vector<usize> break_lines(Context &ctx, const UString &paragraph, const vector<StylePiece> &pieces, f32 available_width) {
        vector<usize> starts{0};
        const usize length = paragraph.size();
        if (available_width <= 0.0f || length == 0) {
            return starts;
        }
        const vector<char32_t> scalars = decode_scalars(paragraph.cpp_string());
        const auto width_of = [&](usize from, usize to) {
            f32 total = 0.0f;
            for (const StylePiece &piece : pieces) {
                const usize a = std::max(from, piece.start);
                const usize b = std::min(to, piece.end);
                if (b > a && !(piece.span != nullptr && piece.span->hidden)) {
                    const UString text = paragraph.substr(a, b - a);
                    total += ctx.measure_text(piece.text_style, text.cpp_string_view()).x;
                }
            }
            return total;
        };
        // Scalar offsets after which a line may break.
        vector<usize> opportunities;
        for (const Text::LineBreakOpportunity &opportunity : Text::line_break_opportunities(paragraph.as_ustr())) {
            const usize scalar = paragraph.scalar_index_of_byte(opportunity.byte_index);
            if (scalar > 0 && scalar < length && (opportunities.empty() || opportunities.back() < scalar)) {
                opportunities.push_back(scalar);
            }
        }
        opportunities.push_back(length);

        f32 used = 0.0f;
        bool line_has_text = false;
        usize at = 0;
        for (const usize unit_end : opportunities) {
            if (unit_end <= at) {
                continue;
            }
            usize word_end = unit_end;
            while (word_end > at && word_end <= scalars.size() && is_break_space(scalars[word_end - 1])) {
                --word_end;
            }
            const f32 word_width = width_of(at, word_end);
            const f32 space_width = unit_end > word_end ? width_of(word_end, unit_end) : 0.0f;
            if (line_has_text && used + word_width > available_width) {
                starts.push_back(at);
                used = 0.0f;
                line_has_text = false;
            }
            if (!line_has_text && word_width > available_width) {
                for (usize k = at; k < word_end; ++k) {
                    const f32 glyph = width_of(k, k + 1);
                    if (used + glyph > available_width && used > 0.0f) {
                        starts.push_back(k);
                        used = 0.0f;
                    }
                    used += glyph;
                }
                used += space_width;
            } else {
                used += word_width + space_width;
            }
            line_has_text = true;
            at = unit_end;
        }
        return starts;
    }

    u64 layout_key(const UString &paragraph, const vector<StylePiece> &pieces, f32 available_width) {
        const auto mix = [](u64 seed, u64 value) { return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2)); };
        u64 key = std::hash<std::string>{}(paragraph.cpp_string());
        key = mix(key, static_cast<u64>(std::llround(available_width * 2.0f)));
        for (const StylePiece &piece : pieces) {
            key = mix(key, piece.start);
            key = mix(key, piece.end);
            key = mix(key, piece.text_style.font_id);
            key = mix(key, piece.text_style.font_size);
        }
        return key;
    }

    /// The pieces of one style that make up a paragraph (cut where the spans start and end).
    vector<StylePiece> style_pieces(const TextEditStyle &style, const vector<RichTextSpan> &spans, usize paragraph_offset,
                                    usize paragraph_length) {
        set<usize> cuts{0, paragraph_length};
        for (const RichTextSpan &span : spans) {
            const isize local_start = static_cast<isize>(span.scalar_start) - static_cast<isize>(paragraph_offset);
            const isize local_end = local_start + static_cast<isize>(span.scalar_length);
            if (local_end <= 0 || local_start >= static_cast<isize>(paragraph_length)) {
                continue;
            }
            cuts.insert(static_cast<usize>(std::clamp<isize>(local_start, 0, static_cast<isize>(paragraph_length))));
            cuts.insert(static_cast<usize>(std::clamp<isize>(local_end, 0, static_cast<isize>(paragraph_length))));
        }
        const vector<usize> sorted(cuts.begin(), cuts.end());
        vector<StylePiece> pieces;
        for (usize i = 0; i + 1 < sorted.size(); ++i) {
            if (sorted[i + 1] == sorted[i]) {
                continue;
            }
            const RichTextSpan *span = span_for_run(spans, sorted[i] + paragraph_offset);
            pieces.push_back(StylePiece{sorted[i], sorted[i + 1], span, run_text_style(style, span, TextWrapMode::None)});
        }
        return pieces;
    }

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
                            const UString &placeholder) {
        const usize line_len = line_text.size();
        const vector<RichTextSpan> &spans = state.highlighted_spans(style.highlighter, style.highlighter_revision);

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


        const Color caret_render_color =
            state.caret_blink_on(style.caret_blink_seconds) ? style.caret_color : Color{0.0, 0.0, 0.0, 0.0};


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


                    .clip_to = FloatingClipTo::AttachedParent,
                },
            });
            (void)caret_bar;
            (void)anchor;
        };


        const string_view composition = state.composition_text();
        const bool show_composition = caret_on_this_line && !composition.empty();
        const auto emit_composition = [&]() {
            if (!show_composition) {
                return;
            }


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


        const bool single_run = sorted_cuts.size() <= 2;
        const bool can_wrap = style.wrap_lines && single_run;
        const TextWrapMode run_wrap_mode = can_wrap ? TextWrapMode::Words : TextWrapMode::None;

        // Wrapped text is laid out here, not by the text element: the engine can only wrap one run of one style, but a paragraph with
        // bold, links, superscript or a caret/selection inside is several runs. The paragraph is broken into visual lines by measuring
        // its words (once per text/style/width, see `VisualLayout`), each visual line is a row of runs aligned on their bottoms, and a
        // run's `baseline_shift` lifts or lowers it inside that row. The width comes from the previous frame's layout of this
        // paragraph; until there is one a plain single-style paragraph falls through to the text element's own wrapping.
        if (style.wrap_lines && !style.mask_characters && line_len > 0) {
            // The width to wrap to is the widget's viewport as of the previous frame (not the paragraph's own width: inside a scroll
            // area that is as wide as its content). A widget that is not a scroll container (a text input) has no such metrics.
            const Context::ScrollMetrics viewport = ctx.scroll_metrics(widget_id);
            f32 available = viewport.found ? viewport.container_size.x : 0.0f;
            if (available <= 0.0f) {
                available = style.wrap_width_hint;
            }
            if (available <= 0.0f) {
                if (const std::optional<ElementBounds> previous = ctx.element_bounds(line_element_id(widget_id, line_scalar_offset))) {
                    available = previous->size.x;
                }
            }
            const vector<StylePiece> pieces = style_pieces(style, spans, line_scalar_offset, line_len);
            if (available > 0.0f || !(single_run && pieces.size() <= 1)) {
                if (available <= 0.0f) {
                    state.request_relayout();
                }
                const u64 key = layout_key(line_text, pieces, available);
                const TextEditState::VisualLayout *cached = state.visual_layout(line_scalar_offset);
                vector<usize> starts;
                if (cached != nullptr && cached->key == key) {
                    starts = cached->line_starts;
                } else {
                    starts = break_lines(ctx, line_text, pieces, available);
                    state.store_visual_layout(line_scalar_offset, TextEditState::VisualLayout{key, starts});
                }

                auto paragraph_scope = ctx.element(ElementDecl{
                    .sizing = {SizingAxis::grow(), SizingAxis::fit()},
                    .direction = LayoutDirection::TopToBottom,
                    .id = line_element_id(widget_id, line_scalar_offset),
                });
                (void)paragraph_scope;
                for (usize k = 0; k < starts.size(); ++k) {
                    const usize row_start = starts[k];
                    const usize row_end = k + 1 < starts.size() ? starts[k + 1] : line_len;
                    auto visual_row = ctx.element(ElementDecl{
                        .sizing = {SizingAxis::grow(), SizingAxis::fit()},
                        .id = visual_row_element_id(widget_id, line_scalar_offset, k),
                    });
                    (void)visual_row;
                    {
                        auto strut = ctx.element(ElementDecl{
                            .sizing = {SizingAxis::fixed(0.0f),
                                       SizingAxis::fixed(std::max(caret_height, static_cast<f32>(style.line_height)))},
                        });
                        (void)strut;
                    }
                    vector<usize> bounds{row_start};
                    for (const usize cut : sorted_cuts) {
                        if (cut > row_start && cut < row_end) {
                            bounds.push_back(cut);
                        }
                    }
                    bounds.push_back(row_end);

                    // Runs sit on one baseline: each is pushed down from the top of the row by however much less than the highest
                    // reach (its font's ascent plus its raise) it has. A run's baseline is the top of its box plus its ascent.
                    struct RowRun {
                        const RichTextSpan *span = nullptr;
                        TextStyle style;
                        f32 reach = 0.0f;
                    };
                    vector<RowRun> row_runs;
                    f32 highest = 0.0f;
                    for (usize i = 0; i + 1 < bounds.size(); ++i) {
                        RowRun run;
                        run.span = span_for_run(spans, bounds[i] + line_scalar_offset);
                        run.style = run_text_style(style, run.span, TextWrapMode::None);
                        const f32 lift = (run.span != nullptr ? run.span->baseline_shift : 0.0f) * static_cast<f32>(style.font_size);
                        run.reach = ctx.text_ascent(run.style) + lift;
                        if (!(run.span != nullptr && run.span->hidden)) {
                            highest = std::max(highest, run.reach);
                        }
                        row_runs.push_back(run);
                    }
                    for (usize i = 0; i + 1 < bounds.size(); ++i) {
                        const usize run_start = bounds[i];
                        const usize run_end = bounds[i + 1];
                        if (caret_on_this_line && run_start == static_cast<usize>(caret_local)) {
                            emit_composition();
                            emit_caret();
                        }
                        const RowRun &run = row_runs[i];
                        if (run.span != nullptr && run.span->hidden) {
                            continue;
                        }
                        const u16 drop = static_cast<u16>(std::clamp(std::round(highest - run.reach), 0.0f, 65535.0f));
                        const bool run_selected = has_sel && run_start >= sel_min && run_end <= sel_max;
                        auto lowered = ctx.element(ElementDecl{.padding = Padding{.top = drop}});
                        (void)lowered;
                        auto text_scope = ctx.element(ElementDecl{
                            .background_color = run_selected ? style.selection_color : Color{0.0, 0.0, 0.0, 0.0},
                        });
                        (void)text_scope;
                        const UString run_text = line_text.substr(run_start, run_end - run_start);
                        ctx.text(run_text.as_ustr(), run.style);
                    }
                    if (caret_on_this_line && k + 1 == starts.size() && static_cast<usize>(caret_local) == line_len) {
                        emit_composition();
                        emit_caret();
                    }
                }
                return;
            }
        }

        auto row = ctx.element(ElementDecl{
            .sizing = can_wrap ? Sizing{SizingAxis::grow(), SizingAxis::fit()} : Sizing{SizingAxis::fit(), SizingAxis::fit()},
            .id = line_element_id(widget_id, line_scalar_offset),
        });
        (void)row;


        {
            auto strut = ctx.element(ElementDecl{
                .sizing = {SizingAxis::fixed(0.0f), SizingAxis::fixed(std::max(caret_height, static_cast<f32>(style.line_height)))},
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
            const bool run_selected = has_sel && run_start >= sel_min && run_end <= sel_max;
            auto run_scope = ctx.element(ElementDecl{
                .background_color = run_selected ? style.selection_color : Color{0.0, 0.0, 0.0, 0.0},
            });
            (void)run_scope;


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
            ctx.text(run_text.as_ustr(), run_text_style(style, span, run_wrap_mode));
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

    /// Performs the hit test line scalar operation for `Detail` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param style `style` value used by the operation.
    /// @param line_text `line_text` value used by the operation.
    /// @param local_x `local_x` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize hit_test_line_scalar(Context &ctx, const TextEditStyle &style,
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

} // namespace SFT::UI::Detail


namespace SFT::UI::Detail {

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
    /// @note Normal inability to produce a value is represented by an empty optional.
    [[nodiscard]] std::optional<ParagraphHit>
    hit_test_paragraphs(Context &ctx, const TextEditStyle &style, const UString &text,
                        const vector<pair<usize, usize>> &paragraphs, const UString &widget_id, glm::vec2 pointer,
                        const TextEditState *state) {
        // A wrapped paragraph is several rows (see render_line): the nearest row of any paragraph wins.
        struct Candidate {
            usize paragraph = 0;
            std::optional<usize> row;
            ElementBounds bounds;
        };
        std::optional<Candidate> best;
        f32 best_distance = 0.0f;
        const auto consider = [&](usize paragraph, std::optional<usize> row, const ElementBounds &bounds) {
            const f32 center_y = bounds.position.y + bounds.size.y * 0.5f;
            const f32 distance = std::abs(pointer.y - center_y);
            if (!best || distance < best_distance) {
                best_distance = distance;
                best = Candidate{paragraph, row, bounds};
            }
        };
        for (usize i = 0; i < paragraphs.size(); ++i) {
            const usize pstart = paragraphs[i].first;
            const TextEditState::VisualLayout *layout = state != nullptr ? state->visual_layout(pstart) : nullptr;
            bool used_rows = false;
            if (layout != nullptr) {
                for (usize row = 0; row < layout->line_starts.size(); ++row) {
                    if (const std::optional<ElementBounds> bounds = ctx.element_bounds(visual_row_element_id(widget_id, pstart, row))) {
                        consider(i, row, *bounds);
                        used_rows = true;
                    }
                }
            }
            if (!used_rows) {
                if (const std::optional<ElementBounds> bounds = ctx.element_bounds(line_element_id(widget_id, pstart))) {
                    consider(i, std::nullopt, *bounds);
                }
            }
        }
        if (!best) {
            return std::nullopt;
        }
        const auto &[pstart, plen] = paragraphs[best->paragraph];
        const UString paragraph_text = text.substr(pstart, plen);
        const f32 local_x = pointer.x - best->bounds.position.x;
        usize scalar_in_paragraph = 0;
        usize char_in_paragraph = 0;
        bool inside = false;
        const TextEditState::VisualLayout *layout = state != nullptr ? state->visual_layout(pstart) : nullptr;
        if (best->row && layout != nullptr && state != nullptr) {
            // Walk the runs of the row, in their own fonts and sizes, until the pointer's x falls inside one.
            const usize row_start = layout->line_starts[*best->row];
            const usize row_end = *best->row + 1 < layout->line_starts.size() ? layout->line_starts[*best->row + 1] : plen;
            const vector<RichTextSpan> &spans = state->highlighted_spans(style.highlighter, style.highlighter_revision);
            const vector<StylePiece> pieces = style_pieces(style, spans, pstart, plen);
            f32 remaining = local_x;
            scalar_in_paragraph = row_end;
            char_in_paragraph = row_end > row_start ? row_end - 1 : row_start;
            for (const StylePiece &piece : pieces) {
                const usize a = std::max(row_start, piece.start);
                const usize b = std::min(row_end, piece.end);
                if (b <= a || (piece.span != nullptr && piece.span->hidden)) {
                    continue;
                }
                const UString run_text = paragraph_text.substr(a, b - a);
                const f32 run_width = ctx.measure_text(piece.text_style, run_text.cpp_string_view()).x;
                if (remaining <= run_width || b == row_end) {
                    const usize byte_offset = ctx.hit_test_text_byte_offset(piece.text_style, run_text.cpp_string_view(), remaining);
                    const usize within = run_text.scalar_index_of_byte(byte_offset);
                    scalar_in_paragraph = a + within;
                    // The character the pointer is over: the boundary is the nearest one, so the character is the one before it when
                    // the pointer is left of that boundary.
                    const f32 boundary_x = within == 0 ? 0.0f
                                                       : ctx.measure_text(piece.text_style, run_text.substr(0, within).cpp_string_view()).x;
                    const usize character = (remaining < boundary_x && within > 0) ? within - 1 : within;
                    char_in_paragraph = a + std::min(character, run_text.size() - 1);
                    inside = remaining >= 0.0f && remaining <= run_width &&
                             pointer.y >= best->bounds.position.y && pointer.y <= best->bounds.position.y + best->bounds.size.y;
                    break;
                }
                remaining -= run_width;
            }
        } else {
            scalar_in_paragraph = hit_test_line_scalar(ctx, style, paragraph_text, local_x);
            char_in_paragraph = std::min(scalar_in_paragraph, plen > 0 ? plen - 1 : 0);
            inside = local_x >= 0.0f && pointer.y >= best->bounds.position.y && pointer.y <= best->bounds.position.y + best->bounds.size.y;
        }
        return ParagraphHit{
            .scalar = pstart + std::min(scalar_in_paragraph, plen),
            .paragraph_start = pstart,
            .paragraph_length = plen,
            .char_under = pstart + char_in_paragraph,
            .inside = inside,
        };
    }

} // namespace SFT::UI::Detail

