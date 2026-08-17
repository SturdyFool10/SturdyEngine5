#include <UI/src/UI/ScrollArea.hpp>


namespace SFT::UI {

    UString scroll_area_part_id(const UString &id, ScrollAreaVisualPart part) {
        if (part == ScrollAreaVisualPart::Viewport)
            return id;
        const char *suffix = part == ScrollAreaVisualPart::VerticalTrack     ? "#v-track"
                             : part == ScrollAreaVisualPart::VerticalThumb   ? "#v-thumb"
                             : part == ScrollAreaVisualPart::HorizontalTrack ? "#h-track"
                                                                             : "#h-thumb";
        return UString{id.cpp_string() + suffix};
    }

    ScrollAreaState::Axis &DetailScrollAreaAccess::vertical(ScrollAreaState &state) noexcept { return state.vertical_; }

    ScrollAreaState::Axis &DetailScrollAreaAccess::horizontal(ScrollAreaState &state) noexcept { return state.horizontal_; }

    ScrollAreaResult scroll_area(Context &ctx, const UString &id, const ElementDecl &decl,
                                        const ScrollbarStyle &style, ScrollAreaState &state, f32 delta_seconds,
                                        const ScrollAreaBuilder &build_content, bool enabled,
                                        const ScrollAreaComposition &composition) {
        ScrollAreaResult result{};
        if (!composition.viewport.visible) {
            return result;
        }

        PartVisualState viewport_visual{.enabled = enabled};
        ScrollAreaPartContext viewport_context{
            .part = ScrollAreaVisualPart::Viewport,
            .visual = viewport_visual,
            .id = id,
            .bounds = ctx.element_bounds(id),
        };
        ElementDecl viewport_decl = decl;
        if (!composition.viewport.render_default)
            clear_element_visual(viewport_decl);
        apply_part_visual(viewport_decl, composition.viewport.visual, viewport_visual);
        if (composition.viewport.alter_decl)
            composition.viewport.alter_decl(viewport_decl, viewport_context);
        viewport_decl.id = id;
        auto scope = ctx.element(viewport_decl);
        (void)scope;
        if (build_content)
            build_content(ctx);
        if (composition.viewport.build)
            composition.viewport.build(ctx, viewport_context);

        const Context::ScrollMetrics metrics = ctx.scroll_metrics(id);
        if (!metrics.found) {
            return result;
        }

        Detail::update_scrollbar_axis(ctx, id, true, style, DetailScrollAreaAccess::vertical(state), delta_seconds,
                                      enabled, metrics, composition.vertical_track, composition.vertical_thumb,
                                      result.vertical_dragging);
        Detail::update_scrollbar_axis(ctx, id, false, style, DetailScrollAreaAccess::horizontal(state), delta_seconds,
                                      enabled, metrics, composition.horizontal_track, composition.horizontal_thumb,
                                      result.horizontal_dragging);
        return result;
    }

    ScrollAreaResult scroll_area(Context &ctx, const UString &id, const ElementDecl &decl,
                                        const ScrollbarStyle &style, ScrollAreaState &state, f32 delta_seconds,
                                        const ScrollAreaBuilder &build_content, bool enabled) {
        return scroll_area(ctx, id, decl, style, state, delta_seconds, build_content, enabled, ScrollAreaComposition{});
    }

} // namespace SFT::UI

namespace SFT::UI::Detail {

    void update_scrollbar_axis(Context &ctx, const UString &container_id, bool vertical,
                                      const ScrollbarStyle &style, ScrollAreaState::Axis &axis,
                                      f32 delta_seconds, bool enabled, const Context::ScrollMetrics &metrics,
                                      const PartSlot<ScrollAreaPartContext> &track_slot,
                                      const PartSlot<ScrollAreaPartContext> &thumb_slot, bool &dragging_out) {
        const f32 content = vertical ? metrics.content_size.y : metrics.content_size.x;
        const f32 container = vertical ? metrics.container_size.y : metrics.container_size.x;
        const f32 offset = vertical ? metrics.offset.y : metrics.offset.x;
        const bool axis_clips = vertical ? metrics.vertical : metrics.horizontal;
        const bool needed = axis_clips && content > container + 0.5f;

        if (!needed || style.visibility == ScrollbarVisibility::AlwaysHidden || !track_slot.visible ||
            !thumb_slot.visible) {
            axis.dragging = false;
            axis.idle_seconds = 1.0e6f;
            axis.last_offset_valid = false;


            if (axis.opacity > 0.0f) {
                axis.opacity_target = 0.0f;
                axis.opacity_elapsed += delta_seconds;
                const f32 t = Detail::eased_progress(axis.opacity_elapsed, style.fade_out_seconds, nullptr);
                axis.opacity = std::lerp(axis.opacity_start, axis.opacity_target, t);
            }
            return;
        }

        const UString track_id = scroll_area_part_id(container_id, vertical ? ScrollAreaVisualPart::VerticalTrack
                                                                             : ScrollAreaVisualPart::HorizontalTrack);
        const UString thumb_id = scroll_area_part_id(container_id, vertical ? ScrollAreaVisualPart::VerticalThumb
                                                                             : ScrollAreaVisualPart::HorizontalThumb);

        const bool track_hovered = enabled && ctx.hovered(track_id);
        const bool thumb_hovered = enabled && ctx.hovered(thumb_id);
        const bool container_hovered = enabled && ctx.hovered(container_id);




        const bool offset_changed = axis.last_offset_valid && std::abs(offset - axis.last_offset) > 0.01f;
        axis.last_offset = offset;
        axis.last_offset_valid = true;

        const bool active = enabled && (container_hovered || track_hovered || thumb_hovered || axis.dragging ||
                                        offset_changed);
        if (active) {
            axis.idle_seconds = 0.0f;
        } else {
            axis.idle_seconds += delta_seconds;
        }

        const bool visible_target = style.visibility == ScrollbarVisibility::AlwaysVisible ||
                                    axis.idle_seconds < style.idle_delay_seconds;
        const f32 target = visible_target ? 1.0f : 0.0f;
        if (!axis.opacity_initialized || target != axis.opacity_target) {
            axis.opacity_start = axis.opacity;
            axis.opacity_target = target;
            axis.opacity_elapsed = 0.0f;
            axis.opacity_initialized = true;
        }
        axis.opacity_elapsed += delta_seconds;
        const f32 duration = target > axis.opacity_start ? style.fade_in_seconds : style.fade_out_seconds;
        const f32 t = Detail::eased_progress(axis.opacity_elapsed, duration, nullptr);
        axis.opacity = std::lerp(axis.opacity_start, axis.opacity_target, t);

        if (axis.opacity <= 0.001f && !axis.dragging) {
            return;
        }

        const f32 track_length = (vertical ? metrics.container_size.y : metrics.container_size.x) - style.margin * 2.0f;
        const f32 visible_fraction = std::clamp(container / content, 0.0f, 1.0f);
        const f32 thumb_length = std::clamp(track_length * visible_fraction, style.min_thumb_length,
                                            std::max(track_length, style.min_thumb_length));
        const f32 travel = std::max(track_length - thumb_length, 0.0f);


        const f32 max_scroll = std::max(content - container, 0.0f);
        const f32 scrolled_fraction = max_scroll > 0.0f ? std::clamp(-offset / max_scroll, 0.0f, 1.0f) : 0.0f;
        const f32 thumb_start = style.margin + scrolled_fraction * travel;

        const bool thumb_pressed = enabled && ctx.clicked(thumb_id);
        if (thumb_pressed && ctx.try_capture_pointer(thumb_id)) {
            axis.dragging = true;
            const f32 pointer_along = vertical ? ctx.pointer_position().y : ctx.pointer_position().x;
            axis.grab_offset = pointer_along - thumb_start;
        }
        if (axis.dragging && !ctx.has_pointer_capture(thumb_id)) {
            axis.dragging = false;
        }
        if (axis.dragging) {
            if (ctx.pointer_cancelled_this_frame() ||
                (!ctx.pointer_is_down() && !ctx.pointer_pressed_this_frame())) {
                axis.dragging = false;
                ctx.release_pointer(thumb_id);
            } else {
                const f32 pointer_along = vertical ? ctx.pointer_position().y : ctx.pointer_position().x;
                const f32 new_start = std::clamp(pointer_along - axis.grab_offset, style.margin,
                                                 style.margin + travel);
                const f32 new_fraction = travel > 0.0f ? (new_start - style.margin) / travel : 0.0f;
                const f32 new_offset = -new_fraction * max_scroll;
                ctx.set_scroll_offset(container_id, vertical ? glm::vec2{metrics.offset.x, new_offset}
                                                             : glm::vec2{new_offset, metrics.offset.y});
            }
        } else {


            const bool track_clicked = enabled && ctx.clicked(track_id);
            if (track_clicked) {
                const f32 pointer_along = vertical ? ctx.pointer_position().y : ctx.pointer_position().x;
                const f32 new_start = std::clamp(pointer_along - thumb_length * 0.5f, style.margin,
                                                 style.margin + travel);
                const f32 new_fraction = travel > 0.0f ? (new_start - style.margin) / travel : 0.0f;
                const f32 new_offset = -new_fraction * max_scroll;
                ctx.set_scroll_offset(container_id, vertical ? glm::vec2{metrics.offset.x, new_offset}
                                                             : glm::vec2{new_offset, metrics.offset.y});
            }
        }
        dragging_out = axis.dragging;

        const bool expanded = thumb_hovered || track_hovered || axis.dragging;
        const f32 thickness = std::lerp(style.idle_thickness, style.hovered_thickness, expanded ? 1.0f : 0.0f);
        const Color thumb_color = axis.dragging  ? style.thumb_dragging_color
                                  : thumb_hovered ? style.thumb_hovered_color
                                                   : style.thumb_color;
        const f32 opacity = axis.opacity;

        const auto make_context = [&](ScrollAreaVisualPart part, const UString &part_id,
                                     const PartVisualState &visual) {
            return ScrollAreaPartContext{
                .part = part,
                .visual = visual,
                .id = part_id,
                .value_fraction = static_cast<f64>(scrolled_fraction),
                .visible_fraction = static_cast<f64>(visible_fraction),
                .bounds = ctx.element_bounds(part_id),
            };
        };

        const FloatingAttachPoint track_attach = vertical ? FloatingAttachPoint::RightTop : FloatingAttachPoint::LeftBottom;
        const glm::vec2 track_size = vertical ? glm::vec2{thickness, track_length} : glm::vec2{track_length, thickness};
        const glm::vec2 track_offset = vertical ? glm::vec2{-style.margin, style.margin}
                                                : glm::vec2{style.margin, -style.margin};

        PartVisualState track_visual{.enabled = enabled, .hovered = track_hovered};
        ScrollAreaPartContext track_context =
            make_context(vertical ? ScrollAreaVisualPart::VerticalTrack : ScrollAreaVisualPart::HorizontalTrack,
                        track_id, track_visual);
        ElementDecl track_decl{
            .sizing = {SizingAxis::fixed(track_size.x), SizingAxis::fixed(track_size.y)},
            .background_color = Color{style.track_color.r, style.track_color.g, style.track_color.b,
                                      style.track_color.a * opacity},
            .corner_radius = CornerRadius::all(style.corner_radius),
            .floating = FloatingConfig{
                .attach_to = FloatingAttachTo::Parent,
                .element_attach_point = track_attach,
                .parent_attach_point = track_attach,
                .offset = track_offset,
                .z_index = 50,
                .clip_to = FloatingClipTo::AttachedParent,
            },
        };
        if (!track_slot.render_default)
            clear_element_visual(track_decl);
        apply_part_visual(track_decl, track_slot.visual, track_visual);
        if (track_slot.alter_decl)
            track_slot.alter_decl(track_decl, track_context);
        if (track_decl.cursor == CursorIcon::Auto) {
            track_decl.cursor = enabled ? CursorIcon::Pointer : CursorIcon::NotAllowed;
        }
        track_decl.id = track_id;
        auto track_scope = ctx.element(track_decl);
        (void)track_scope;
        if (track_slot.build)
            track_slot.build(ctx, track_context);

        PartVisualState thumb_visual{.enabled = enabled, .hovered = thumb_hovered, .active = axis.dragging};
        ScrollAreaPartContext thumb_context =
            make_context(vertical ? ScrollAreaVisualPart::VerticalThumb : ScrollAreaVisualPart::HorizontalThumb,
                        thumb_id, thumb_visual);
        const glm::vec2 thumb_size = vertical ? glm::vec2{thickness, thumb_length} : glm::vec2{thumb_length, thickness};
        const glm::vec2 thumb_offset = vertical ? glm::vec2{-style.margin, thumb_start}
                                                : glm::vec2{thumb_start, -style.margin};
        ElementDecl thumb_decl{
            .sizing = {SizingAxis::fixed(thumb_size.x), SizingAxis::fixed(thumb_size.y)},
            .background_color = Color{thumb_color.r, thumb_color.g, thumb_color.b, thumb_color.a * opacity},
            .corner_radius = CornerRadius::all(style.corner_radius),
            .floating = FloatingConfig{
                .attach_to = FloatingAttachTo::Parent,
                .element_attach_point = track_attach,
                .parent_attach_point = track_attach,
                .offset = thumb_offset,
                .z_index = 51,
                .clip_to = FloatingClipTo::AttachedParent,
            },
        };
        if (!thumb_slot.render_default)
            clear_element_visual(thumb_decl);
        apply_part_visual(thumb_decl, thumb_slot.visual, thumb_visual);
        if (thumb_slot.alter_decl)
            thumb_slot.alter_decl(thumb_decl, thumb_context);
        if (thumb_decl.cursor == CursorIcon::Auto) {
            thumb_decl.cursor = enabled ? (axis.dragging ? CursorIcon::Grabbing : CursorIcon::Grab)
                                        : CursorIcon::NotAllowed;
        }
        thumb_decl.id = thumb_id;
        auto thumb_scope = ctx.element(thumb_decl);
        (void)thumb_scope;
        if (thumb_slot.build)
            thumb_slot.build(ctx, thumb_context);
    }

} // namespace SFT::UI::Detail

