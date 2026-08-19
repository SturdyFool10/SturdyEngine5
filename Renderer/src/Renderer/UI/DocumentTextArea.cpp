#include <Renderer/UI/DocumentTextArea.hpp>


namespace SFT::UI {

    /// Returns the current or globally available document value.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    Text::Document &DocumentTextAreaState::document() noexcept { return document_; }

    /// Returns the current or globally available document value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    const Text::Document &DocumentTextAreaState::document() const noexcept { return document_; }

    /// Returns the current or globally available caret value.
    ///
    /// @return Returns the current caret value.
    /// @note This function does not throw exceptions.
    Text::ByteOffset DocumentTextAreaState::caret() const noexcept { return caret_; }

    /// Returns the current or globally available focused value.
    ///
    /// @return Returns the current focused value.
    /// @note This function does not throw exceptions.
    bool DocumentTextAreaState::focused() const noexcept { return focused_; }

    /// Sets the focused for this `UI`.
    ///
    /// @param focused `focused` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void DocumentTextAreaState::set_focused(bool focused) noexcept { focused_ = focused; }

    /// Sets the text for this `UI`.
    ///
    /// @param utf8 `utf8` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void DocumentTextAreaState::set_text(string_view utf8) {
        document_ = Text::Document{utf8};
        caret_ = Text::ByteOffset{document_.snapshot().byte_size()};
    }

    /// Performs the text area operation for `UI` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param decl `decl` value used by the operation.
    /// @param style `style` value used by the operation.
    /// @param state `state` value used by the operation.
    /// @param input `input` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    /// @param scrollbar_style `scrollbar_style` value used by the operation.
    /// @param scroll_state `scroll_state` value used by the operation.
    /// @param line_height `line_height` value used by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    DocumentTextAreaResult text_area(Context &ctx, const ElementDecl &decl,
                                                           const TextEditStyle &style, DocumentTextAreaState &state,
                                                           const TextEditInput &input, f32 delta_seconds,
                                                           const ScrollbarStyle &scrollbar_style,
                                                           ScrollAreaState &scroll_state, f32 line_height,
                                                           bool enabled) {
        const Text::DocumentSnapshot snapshot = state.document().snapshot();
        const bool hovered = enabled && ctx.hovered(decl.id);
        if (enabled && ctx.clicked(decl.id)) state.set_focused(true);
        else if (ctx.clicked_outside(decl.id)) state.set_focused(false);

        const Color &target = !enabled ? style.disabled : state.focused() ? style.focused : hovered ? style.hovered : style.idle;
        state.color_.update(target, delta_seconds, style.transition_seconds, style.color_space, style.easing);
        state.blink_elapsed_ += delta_seconds;

        bool changed = false;
        if (enabled && state.focused()) {
            if (style.features.typing && !input.typed_text.empty()) {
                changed |= Detail::replace_document_range(state, {state.caret_, state.caret_}, input.typed_text);
            }
            for (EditKey key : input.keys) {
                if (!style.bindings.enabled(key)) continue;
                key = style.bindings.resolve(key);
                const Text::DocumentSnapshot current = state.document().snapshot();
                switch (key) {
                case EditKey::Left:
                    if (style.features.navigation) state.caret_ = Detail::previous_scalar(current, state.caret_);
                    break;
                case EditKey::Right:
                    if (style.features.navigation) state.caret_ = Detail::next_scalar(current, state.caret_);
                    break;
                case EditKey::Backspace:
                    if (style.features.deletion && state.caret_.value != 0) {
                        const Text::ByteOffset previous = Detail::previous_scalar(current, state.caret_);
                        changed |= Detail::replace_document_range(state, {previous, state.caret_}, {});
                    }
                    break;
                case EditKey::Delete:
                    if (style.features.deletion && state.caret_.value != current.byte_size()) {
                        changed |= Detail::replace_document_range(state, {state.caret_, Detail::next_scalar(current, state.caret_)}, {});
                    }
                    break;
                case EditKey::Enter:
                    if (style.features.submission) changed |= Detail::replace_document_range(state, {state.caret_, state.caret_}, "\n");
                    break;
                case EditKey::Home:
                case EditKey::End: {
                    if (!style.features.navigation) break;
                    const auto point = current.offset_to_point(state.caret_);
                    if (!point) break;
                    const auto range = current.line_range(point->line);
                    if (range) state.caret_ = key == EditKey::Home ? range->start : range->end;
                    break;
                }
                case EditKey::Up:
                case EditKey::Down: {
                    if (!style.features.navigation) break;
                    const auto point = current.offset_to_point(state.caret_);
                    if (!point) break;
                    const isize line = static_cast<isize>(point->line) + (key == EditKey::Up ? -1 : 1);
                    if (line >= 0 && line < static_cast<isize>(current.line_count())) {
                        if (const auto destination = current.point_to_offset({static_cast<usize>(line), point->scalar_column})) {
                            state.caret_ = *destination;
                        } else if (const auto range = current.line_range(static_cast<usize>(line))) {
                            state.caret_ = range->end;
                        }
                    }
                    break;
                }
                case EditKey::Escape:
                    if (style.features.escape_to_unfocus) state.set_focused(false);
                    break;
                default: break;
                }
            }
        }

        const Text::DocumentSnapshot render_snapshot = state.document().snapshot();
        const Context::ScrollMetrics metrics = ctx.scroll_metrics(decl.id);
        const f32 viewport_height = metrics.found ? metrics.container_size.y : line_height * 24.0f;
        const usize overscan = 3;
        const usize first_line = metrics.found ? static_cast<usize>(std::max(0.0f, -metrics.offset.y) / line_height) : 0;
        const usize start_line = first_line > overscan ? first_line - overscan : 0;
        const usize line_count = render_snapshot.line_count();
        const usize visible_count = std::min(line_count - std::min(start_line, line_count),
                                             static_cast<usize>(std::ceil(viewport_height / line_height)) + overscan * 2 + 1);

        ElementDecl styled = decl;
        styled.background_color = state.color_.current();
        styled.corner_radius = style.corner_radius;
        styled.border = state.focused() ? style.border_focused : style.border_idle;
        styled.direction = LayoutDirection::TopToBottom;
        styled.clip = {.horizontal = style.features.horizontal_scroll, .vertical = style.features.vertical_scroll};
        if (styled.cursor == CursorIcon::Auto) {
            styled.cursor = enabled ? CursorIcon::Text : CursorIcon::NotAllowed;
        }
        ScrollbarStyle resolved_scrollbar = scrollbar_style;
        if (!style.features.scrollbars) resolved_scrollbar.visibility = ScrollbarVisibility::AlwaysHidden;

        (void)scroll_area(ctx, decl.id, styled, resolved_scrollbar, scroll_state, delta_seconds, [&](Context &inner) {
            const auto spacer = [&](f32 height) {
                if (height <= 0.0f) return;
                auto scope = inner.element(ElementDecl{.sizing = {SizingAxis::fixed(0.0f), SizingAxis::fixed(height)}});
                (void)scope;
            };
            spacer(static_cast<f32>(start_line) * line_height);
            for (usize line = start_line; line < start_line + visible_count; ++line) {
                const auto range = render_snapshot.line_range(line);
                if (!range) break;
                const std::string bytes = render_snapshot.slice(*range).flatten();
                const UString text{bytes};
                auto row = inner.element(ElementDecl{
                    .sizing = {SizingAxis::fit(), SizingAxis::fixed(line_height)},
                    .id = Detail::line_element_id(decl.id, range->start.value),
                });
                (void)row;
                inner.text(text.as_ustr(), TextStyle{.color = style.text_color, .font_id = style.font_id,
                                                      .font_size = style.font_size, .wrap_mode = TextWrapMode::None});
            }
            spacer(static_cast<f32>(line_count - (start_line + visible_count)) * line_height);
        }, enabled);

        return {.changed = changed, .focused = state.focused(), .revision = render_snapshot.revision(),
                .first_rendered_line = start_line, .rendered_line_count = visible_count};
    }

} // namespace SFT::UI

namespace SFT::UI::Detail {

    /// Performs the previous scalar operation for `Detail` using the supplied arguments.
    ///
    /// @param snapshot `snapshot` value used by the operation.
    /// @param offset Offset from the beginning of the relevant range or buffer.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    Text::ByteOffset previous_scalar(const Text::DocumentSnapshot &snapshot,
                                                           Text::ByteOffset offset) {
        if (offset.value == 0) return offset;
        const usize begin = offset.value > 4 ? offset.value - 4 : 0;
        const std::string bytes = snapshot.slice({{begin}, offset}).flatten();
        usize local = bytes.size() - 1;
        while (local > 0 && (static_cast<unsigned char>(bytes[local]) & 0xc0u) == 0x80u) --local;
        return Text::ByteOffset{begin + local};
    }

    /// Performs the next scalar operation for `Detail` using the supplied arguments.
    ///
    /// @param snapshot `snapshot` value used by the operation.
    /// @param offset Offset from the beginning of the relevant range or buffer.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    Text::ByteOffset next_scalar(const Text::DocumentSnapshot &snapshot,
                                                       Text::ByteOffset offset) {
        if (offset.value >= snapshot.byte_size()) return offset;
        const std::string byte = snapshot.slice({{offset.value}, {offset.value + 1}}).flatten();
        const unsigned char first = static_cast<unsigned char>(byte.front());
        const usize width = first < 0x80u ? 1 : first < 0xe0u ? 2 : first < 0xf0u ? 3 : 4;
        return Text::ByteOffset{std::min(offset.value + width, snapshot.byte_size())};
    }

    /// Replaces document range using the supplied arguments and current state.
    ///
    /// @param state `state` value used by the operation.
    /// @param range Range of values to process.
    /// @param replacement `replacement` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool replace_document_range(DocumentTextAreaState &state, Text::TextRange range,
                                       string_view replacement) {
        Text::EditTransaction transaction{state.document_.revision()};
        transaction.replace(range, replacement);
        const auto applied = state.document_.apply(transaction);
        if (!applied) return false;
        state.caret_ = Text::ByteOffset{range.start.value + replacement.size()};
        return true;
    }

} // namespace SFT::UI::Detail

