#include <UI/src/UI/TextEdit.hpp>


namespace SFT::UI {

    bool TextEditBindings::enabled(EditKey trigger) const noexcept {
        for (const TextEditKeyBinding &binding : keys) {
            if (binding.trigger == trigger) {
                return binding.enabled;
            }
        }
        return true;
    }

    EditKey TextEditBindings::resolve(EditKey trigger) const noexcept {
        for (const TextEditKeyBinding &binding : keys) {
            if (binding.trigger == trigger) {
                return binding.command;
            }
        }
        return trigger;
    }

    const UString &TextEditState::text() const noexcept { return text_; }

    void TextEditState::set_text(UString text) noexcept {
        text_ = std::move(text);
        caret_ = text_.size();
        selection_anchor_ = caret_;
    }

    usize TextEditState::caret() const noexcept { return caret_; }

    bool TextEditState::has_selection() const noexcept { return selection_anchor_ != caret_; }

    usize TextEditState::selection_min() const noexcept { return std::min(caret_, selection_anchor_); }

    usize TextEditState::selection_max() const noexcept { return std::max(caret_, selection_anchor_); }

    UString TextEditState::selected_text() const {
        return has_selection() ? text_.substr(selection_min(), selection_max() - selection_min()) : UString{};
    }

    bool TextEditState::focused() const noexcept { return focused_; }

    void TextEditState::set_focused(bool focused) noexcept { focused_ = focused; }

    string_view TextEditState::composition_text() const noexcept { return composition_text_; }

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

    void TextEditState::delete_selection() {
        if (!has_selection()) {
            return;
        }
        const usize start = selection_min();
        text_.erase(start, selection_max() - start);
        caret_ = start;
        selection_anchor_ = start;
    }

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

    void TextEditState::move_to_start(bool extend) noexcept {
        caret_ = 0;
        if (!extend) {
            selection_anchor_ = caret_;
        }
    }

    void TextEditState::move_to_end(bool extend) noexcept {
        caret_ = text_.size();
        if (!extend) {
            selection_anchor_ = caret_;
        }
    }

    void TextEditState::select_all() noexcept {
        selection_anchor_ = 0;
        caret_ = text_.size();
    }

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

    void TextEditState::select_range(usize start, usize end) noexcept {
        selection_anchor_ = std::min(start, text_.size());
        caret_ = std::min(end, text_.size());
    }

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

    void TextEditState::set_caret_to(usize scalar_index, bool extend) noexcept {
        caret_ = std::min(scalar_index, text_.size());
        if (!extend) {
            selection_anchor_ = caret_;
        }
    }

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
            }
        }

        if (result.changed) {
            blink_elapsed_ = 0.0f;
        }
        return result;
    }

    void TextEditState::update_visual(bool hovered, bool enabled, const TextEditStyle &style, f32 delta_seconds) noexcept {
        const Color &target = !enabled ? style.disabled : focused_ ? style.focused : hovered ? style.hovered : style.idle;
        color_.update(target, delta_seconds, style.transition_seconds, style.color_space, style.easing);
        blink_elapsed_ += delta_seconds;
        time_since_last_click_ += delta_seconds;
    }

    Color TextEditState::current_color() const noexcept { return color_.current(); }

    bool TextEditState::caret_blink_on(f32 blink_seconds) const noexcept {
        if (blink_seconds <= 0.0f) {
            return true;
        }
        const f32 period = blink_seconds * 2.0f;
        const f32 phase = std::fmod(blink_elapsed_, period);
        return phase < blink_seconds;
    }

    const vector<RichTextSpan> &TextEditState::highlighted_spans(const Highlighter &highlighter) const {
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

    UString strip_newlines(const UString &text) {
        UString result;
        for (char32_t scalar : text.codepoints()) {
            if (scalar != U'\n' && scalar != U'\r') {
                result.append(scalar);
            }
        }
        return result;
    }

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

    UString caret_element_id(const UString &widget_id) {
        return UString{widget_id.cpp_string() + "#caret"};
    }

    UString line_element_id(const UString &widget_id, usize line_scalar_offset) {
        return UString{widget_id.cpp_string() + "#line" + std::to_string(line_scalar_offset)};
    }

    const RichTextSpan *span_for_run(const vector<RichTextSpan> &spans,
                                                           usize global_scalar_index) noexcept {
        for (const RichTextSpan &span : spans) {
            if (global_scalar_index >= span.scalar_start && global_scalar_index < span.scalar_start + span.scalar_length) {
                return &span;
            }
        }
        return nullptr;
    }

    void render_line(Context &ctx, const UString &line_text, usize line_scalar_offset,
                            const TextEditStyle &style, const TextEditState &state, const UString &widget_id,
                            const UString &placeholder) {
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

        auto row = ctx.element(ElementDecl{
            .sizing = can_wrap ? Sizing{SizingAxis::grow(), SizingAxis::fit()} : Sizing{SizingAxis::fit(), SizingAxis::fit()},
            .id = line_element_id(widget_id, line_scalar_offset),
        });
        (void)row;







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
                emit_decoration(                  false);
            }
            if (span != nullptr && span->strikethrough) {
                emit_decoration(                  true);
            }


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

    [[nodiscard]] std::optional<ParagraphHit>
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

} // namespace SFT::UI::Detail

