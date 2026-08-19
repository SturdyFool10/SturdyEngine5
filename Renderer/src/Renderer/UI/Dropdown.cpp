#include <Renderer/UI/Dropdown.hpp>


namespace SFT::UI {

    /// Performs the dropdown part ID operation for `UI` using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    /// @param part `part` value used by the operation.
    /// @param option_index Zero-based index of the target element or entry.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    UString dropdown_part_id(const UString &id, DropdownVisualPart part, std::optional<usize> option_index) {
        if (part == DropdownVisualPart::Trigger)
            return id;
        if (part == DropdownVisualPart::Option && option_index.has_value())
            return UString{id.cpp_string() + "#" + std::to_string(*option_index)};
        const char *suffix = part == DropdownVisualPart::Anchor      ? "#anchor"
                             : part == DropdownVisualPart::Indicator ? "#indicator"
                             : part == DropdownVisualPart::List      ? "#list"
                             : part == DropdownVisualPart::Header    ? "#header"
                             : part == DropdownVisualPart::Footer    ? "#footer"
                             : part == DropdownVisualPart::Empty     ? "#empty"
                             : part == DropdownVisualPart::Tooltip   ? "#tooltip"
                                                                     : "#option";
        return UString{id.cpp_string() + suffix};
    }

    /// Performs the dropdown operation for `UI` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param id Identifier of the target object or resource.
    /// @param trigger_decl `trigger_decl` value used by the operation.
    /// @param style `style` value used by the operation.
    /// @param state `state` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    /// @param selected_index Zero-based index of the target element or entry.
    /// @param options Configuration values controlling the operation.
    /// @param enabled Whether the associated behavior is enabled.
    /// @param composition `composition` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    DropdownResult dropdown(Context &ctx, const UString &id, const ElementDecl &trigger_decl, const DropdownStyle &style, DropdownState &state, f32 delta_seconds, usize selected_index, span<const DropdownOption> options, bool enabled, const DropdownComposition &composition) {
        DropdownResult result{.selected_index = selected_index};
        const bool anchor_enabled = enabled && composition.anchor.enabled;
        const bool trigger_enabled = anchor_enabled && composition.trigger.enabled && composition.trigger.visible;
        if (!anchor_enabled || !composition.anchor.visible || !trigger_enabled) {
            state.open = false;
        }
        if (!composition.anchor.visible) {
            return result;
        }

        const std::optional<ElementBounds> trigger_bounds = ctx.element_bounds(id);
        const auto make_context = [&](DropdownVisualPart part, const UString &part_id, const PartVisualState &visual, std::optional<usize> option_index = std::nullopt, const DropdownOption *option = nullptr) {
            return DropdownPartContext{
                .part = part,
                .visual = visual,
                .id = part_id,
                .open = state.open,
                .selected_index = selected_index,
                .option_index = option_index,
                .option = option,
                .trigger_bounds = trigger_bounds,
                .bounds = ctx.element_bounds(part_id),
            };
        };

        PartVisualState anchor_visual{.enabled = anchor_enabled, .active = state.open};
        const UString anchor_id = dropdown_part_id(id, DropdownVisualPart::Anchor);
        DropdownPartContext anchor_context = make_context(DropdownVisualPart::Anchor, anchor_id, anchor_visual);
        ElementDecl anchor_decl{.sizing = {SizingAxis::fit(), SizingAxis::fit()}};
        if (!composition.anchor.render_default)
            clear_element_visual(anchor_decl);
        apply_part_visual(anchor_decl, composition.anchor.visual, anchor_visual);
        if (composition.anchor.alter_decl)
            composition.anchor.alter_decl(anchor_decl, anchor_context);
        anchor_decl.id = anchor_id;
        auto anchor = ctx.element(anchor_decl);
        (void)anchor;
        if (composition.anchor.build)
            composition.anchor.build(ctx, anchor_context);

        std::optional<usize> tooltip_option;
        bool trigger_hovered = false;
        UString tooltip_target = id;

        if (composition.trigger.visible) {
            trigger_hovered = trigger_enabled && ctx.hovered(id);
            const bool trigger_pressed = trigger_enabled && ctx.pointer_down(id);
            const bool trigger_clicked = trigger_enabled && ctx.clicked(id);
            PartVisualState trigger_visual{
                .enabled = trigger_enabled,
                .hovered = trigger_hovered,
                .pressed = trigger_pressed,
                .active = state.open,
                .selected = selected_index < options.size(),
            };
            DropdownPartContext trigger_context = make_context(
                DropdownVisualPart::Trigger,
                id,
                trigger_visual,
                selected_index < options.size() ? std::optional<usize>{selected_index} : std::nullopt,
                selected_index < options.size() ? &options[selected_index] : nullptr);

            const bool show_indicator = composition.indicator.visible &&
                                        (style.show_arrow_indicator || static_cast<bool>(composition.indicator.build));

            state.trigger_state.update(trigger_hovered, trigger_pressed, trigger_enabled, style.trigger, delta_seconds);
            ElementDecl trigger = trigger_decl;
            trigger.background_color = state.trigger_state.current_color();
            trigger.corner_radius = style.trigger.corner_radius;
            trigger.border = style.trigger.border;
            if (style.center_trigger_content_vertically)
                trigger.child_alignment.y = AlignY::Center;
            if (show_indicator) {


                const bool attaches_right = style.arrow_attach_point == FloatingAttachPoint::RightTop ||
                                            style.arrow_attach_point == FloatingAttachPoint::RightCenter ||
                                            style.arrow_attach_point == FloatingAttachPoint::RightBottom;
                const bool attaches_left = style.arrow_attach_point == FloatingAttachPoint::LeftTop ||
                                           style.arrow_attach_point == FloatingAttachPoint::LeftCenter ||
                                           style.arrow_attach_point == FloatingAttachPoint::LeftBottom;
                const auto reserved = static_cast<u16>(style.arrow_reserved_space);
                if (attaches_right)
                    trigger.padding.right = std::max<u16>(trigger.padding.right, reserved);
                else if (attaches_left)
                    trigger.padding.left = std::max<u16>(trigger.padding.left, reserved);
            }
            if (!composition.trigger.render_default)
                clear_element_visual(trigger);
            apply_part_visual(trigger, composition.trigger.visual, trigger_visual);
            if (composition.trigger.alter_decl)
                composition.trigger.alter_decl(trigger, trigger_context);
            if (trigger.cursor == CursorIcon::Auto) {
                trigger.cursor = trigger_enabled ? CursorIcon::Pointer : CursorIcon::NotAllowed;
            }
            trigger.id = id;
            auto trigger_scope = ctx.element(trigger);
            (void)trigger_scope;

            if (trigger_clicked)
                state.open = !state.open;
            if (composition.trigger.render_default && selected_index < options.size() && options[selected_index].build)
                options[selected_index].build(ctx);
            if (composition.trigger.build)
                composition.trigger.build(ctx, trigger_context);

            if (show_indicator) {
                const UString indicator_id = dropdown_part_id(id, DropdownVisualPart::Indicator);
                PartVisualState indicator_visual{.enabled = trigger_enabled, .active = state.open};
                DropdownPartContext indicator_context = make_context(
                    DropdownVisualPart::Indicator,
                    indicator_id,
                    indicator_visual);


                ElementDecl indicator_decl{
                    .sizing = {SizingAxis::fit(), SizingAxis::fit()},
                    .floating = FloatingConfig{
                        .attach_to = FloatingAttachTo::Parent,
                        .element_attach_point = style.arrow_attach_point,
                        .parent_attach_point = style.arrow_attach_point,
                        .offset = style.arrow_offset,
                        .capture_pointer = false,


                        .clip_to = FloatingClipTo::AttachedParent,
                    },
                };
                if (!composition.indicator.render_default)
                    clear_element_visual(indicator_decl);
                apply_part_visual(indicator_decl, composition.indicator.visual, indicator_visual);
                if (composition.indicator.alter_decl)
                    composition.indicator.alter_decl(indicator_decl, indicator_context);
                indicator_decl.id = indicator_id;
                auto indicator_scope = ctx.element(indicator_decl);
                (void)indicator_scope;
                if (composition.indicator.render_default && style.show_arrow_indicator) {
                    ctx.text(u8"▾"_ustr, TextStyle{.color = style.arrow_color, .font_id = style.arrow_font_id, .font_size = style.arrow_font_size});
                }
                if (composition.indicator.build)
                    composition.indicator.build(ctx, indicator_context);
            }
        }

        if (state.open && composition.list.visible) {
            const UString list_id = dropdown_part_id(id, DropdownVisualPart::List);
            PartVisualState list_visual{.enabled = anchor_enabled && composition.list.enabled, .active = true};
            DropdownPartContext list_context = make_context(DropdownVisualPart::List, list_id, list_visual);
            ElementDecl list_decl{
                .sizing = {SizingAxis::fit(), SizingAxis::fit()},
                .direction = LayoutDirection::TopToBottom,
                .background_color = style.list_background,
                .corner_radius = style.corner_radius,
                .border = style.border,
                .floating = FloatingConfig{
                    .attach_to = FloatingAttachTo::Parent,
                    .element_attach_point = FloatingAttachPoint::LeftTop,
                    .parent_attach_point = FloatingAttachPoint::LeftBottom,
                    .offset = {0.0f, 4.0f},
                    .z_index = static_cast<i16>(style.list_z_index),


                },
            };
            if (!composition.list.render_default)
                clear_element_visual(list_decl);
            apply_part_visual(list_decl, composition.list.visual, list_visual);
            if (composition.list.alter_decl)
                composition.list.alter_decl(list_decl, list_context);
            list_decl.id = list_id;
            auto list_scope = ctx.element(list_decl);
            (void)list_scope;
            if (composition.list.build)
                composition.list.build(ctx, list_context);

            const auto render_auxiliary = [&](DropdownVisualPart part, const PartSlot<DropdownPartContext> &slot) {
                if (!slot.visible)
                    return;
                const UString part_id = dropdown_part_id(id, part);
                PartVisualState visual{.enabled = anchor_enabled && slot.enabled, .active = state.open};
                DropdownPartContext part_context = make_context(part, part_id, visual);
                ElementDecl part_decl{.sizing = {SizingAxis::grow(), SizingAxis::fit()}};
                if (!slot.render_default)
                    clear_element_visual(part_decl);
                apply_part_visual(part_decl, slot.visual, visual);
                if (slot.alter_decl)
                    slot.alter_decl(part_decl, part_context);
                part_decl.id = part_id;
                auto scope = ctx.element(part_decl);
                (void)scope;
                if (slot.build)
                    slot.build(ctx, part_context);
            };

            render_auxiliary(DropdownVisualPart::Header, composition.header);
            if (options.empty()) {
                render_auxiliary(DropdownVisualPart::Empty, composition.empty);
            } else if (composition.option.visible) {
                for (usize i = 0; i < options.size(); ++i) {
                    const UString option_id = dropdown_part_id(id, DropdownVisualPart::Option, i);
                    const bool predicate_enabled = !composition.option_enabled || composition.option_enabled(i);
                    const bool option_enabled = anchor_enabled && composition.list.enabled &&
                                                composition.option.enabled && predicate_enabled;
                    const bool row_hovered = option_enabled && ctx.hovered(option_id);
                    const bool row_pressed = option_enabled && ctx.pointer_down(option_id);
                    if (option_enabled && ctx.clicked(option_id)) {
                        result.selected_index = i;
                        result.changed = i != selected_index;
                        state.open = false;
                    }
                    if (row_hovered) {
                        tooltip_option = i;
                        tooltip_target = option_id;
                    }
                    PartVisualState option_visual{
                        .enabled = option_enabled,
                        .hovered = row_hovered,
                        .pressed = row_pressed,
                        .selected = i == selected_index,
                    };
                    DropdownPartContext option_context = make_context(
                        DropdownVisualPart::Option,
                        option_id,
                        option_visual,
                        i,
                        &options[i]);
                    ElementDecl row_decl{
                        .sizing = {SizingAxis::grow(), SizingAxis::fit()},
                        .padding = Padding::symmetric(style.option_padding, style.option_padding / 2),
                        .background_color = row_hovered ? style.option_hovered : Color{0.0, 0.0, 0.0, 0.0},
                    };
                    if (!composition.option.render_default)
                        clear_element_visual(row_decl);
                    apply_part_visual(row_decl, composition.option.visual, option_visual);
                    if (composition.option.alter_decl)
                        composition.option.alter_decl(row_decl, option_context);
                    if (row_decl.cursor == CursorIcon::Auto) {
                        row_decl.cursor = option_enabled ? CursorIcon::Pointer : CursorIcon::NotAllowed;
                    }
                    row_decl.id = option_id;
                    auto row = ctx.element(row_decl);
                    (void)row;
                    if (composition.option.render_default && options[i].build)
                        options[i].build(ctx);
                    if (composition.option.build)
                        composition.option.build(ctx, option_context);
                }
            }
            render_auxiliary(DropdownVisualPart::Footer, composition.footer);
        }

        const bool show_tooltip = composition.tooltip.visible && composition.tooltip.build &&
                                  (trigger_hovered || tooltip_option.has_value());
        if (show_tooltip) {
            const UString tooltip_id = dropdown_part_id(id, DropdownVisualPart::Tooltip);
            const bool tooltip_enabled = anchor_enabled && composition.tooltip.enabled;
            PartVisualState tooltip_visual{.enabled = tooltip_enabled, .active = state.open};
            DropdownPartContext tooltip_context = make_context(
                DropdownVisualPart::Tooltip,
                tooltip_id,
                tooltip_visual,
                tooltip_option,
                tooltip_option.has_value() ? &options[*tooltip_option]
                                           : (selected_index < options.size() ? &options[selected_index] : nullptr));
            ElementDecl tooltip_decl{
                .sizing = {SizingAxis::fit(), SizingAxis::fit()},
                .floating = FloatingConfig{
                    .attach_to = FloatingAttachTo::ElementWithId,
                    .parent_id = tooltip_target,
                    .element_attach_point = FloatingAttachPoint::LeftBottom,
                    .parent_attach_point = FloatingAttachPoint::LeftTop,
                    .offset = {0.0f, -4.0f},
                    .z_index = static_cast<i16>(style.list_z_index + 1),
                    .capture_pointer = false,


                },
            };
            if (!composition.tooltip.render_default)
                clear_element_visual(tooltip_decl);
            apply_part_visual(tooltip_decl, composition.tooltip.visual, tooltip_visual);
            if (composition.tooltip.alter_decl)
                composition.tooltip.alter_decl(tooltip_decl, tooltip_context);
            tooltip_decl.id = tooltip_id;
            auto tooltip_scope = ctx.element(tooltip_decl);
            (void)tooltip_scope;
            composition.tooltip.build(ctx, tooltip_context);
        }
        return result;
    }

    /// Performs the dropdown operation for `UI` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param id Identifier of the target object or resource.
    /// @param trigger_decl `trigger_decl` value used by the operation.
    /// @param style `style` value used by the operation.
    /// @param state `state` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    /// @param selected_index Zero-based index of the target element or entry.
    /// @param options Configuration values controlling the operation.
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    DropdownResult dropdown(Context &ctx, const UString &id, const ElementDecl &trigger_decl, const DropdownStyle &style, DropdownState &state, f32 delta_seconds, usize selected_index, span<const DropdownOption> options, bool enabled) {
        return dropdown(ctx, id, trigger_decl, style, state, delta_seconds, selected_index, options, enabled, DropdownComposition{});
    }

} // namespace SFT::UI

