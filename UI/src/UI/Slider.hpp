#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <string>
#pragma endregion

#include "Context.hpp"
#include "Style.hpp"
#include "WidgetComposition.hpp"

using std::optional;
using std::span;

// Immediate-mode numeric range control with the interaction/value semantics expected from an HTML
// input[type=range]: bounded values, min-relative step snapping (or step="any"), track clicks,
// captured thumb dragging, disabled state, focus, repeated keyboard intents, Home/End/Page steps,
// arbitrary datalist-like tick marks, and separate live-change/gesture-commit signals. UI remains
// input-backend-agnostic: callers translate physical keys into SliderKey just as TextEditInput does.
namespace SFT::UI {

    enum class SliderOrientation : u8 { Horizontal,
                                        Vertical };

    // Numeric intents rather than physical keys. Increment always raises the value and Decrement
    // always lowers it, regardless of orientation/reversal; the platform layer decides which arrow
    // key maps to which intent.
    enum class SliderKey : u8 { Decrement,
                                Increment,
                                PageDecrement,
                                PageIncrement,
                                Minimum,
                                Maximum };

    struct SliderInput {
        span<const SliderKey> keys{};
        bool request_focus = false;
        bool request_blur = false;
    };

    struct SliderTick {
        f64 value = 0.0;
    };

    struct SliderConfig {
        f64 min = 0.0;
        f64 max = 100.0;
        // A positive value snaps to min + N*step. std::nullopt is HTML's step="any". Invalid
        // numeric steps fall back to HTML's default of 1 rather than silently becoming continuous.
        optional<f64> step = 1.0;
        // Used only for keyboard intents. Omitted: numeric step, or 1% of the range for step="any".
        optional<f64> keyboard_step;
        // Omitted: max(10 * keyboard step, 10% of the range).
        optional<f64> page_step;
        SliderOrientation orientation = SliderOrientation::Horizontal;
        // Horizontal defaults to min-left/max-right. Vertical defaults to min-bottom/max-top;
        // reversed swaps those visual endpoints without changing numeric keyboard semantics.
        bool reversed = false;
        span<const SliderTick> ticks{};
        // Generates regular marks from min by step, capped by max_generated_ticks. Arbitrary ticks
        // above are visual suggestions only, matching datalist behavior; neither kind changes snap.
        bool show_step_ticks = false;
        usize max_generated_ticks = 128;
    };

    struct SliderStyle {
        Color track{0.20, 0.21, 0.26, 1.0};
        Color track_disabled{0.20, 0.20, 0.20, 0.5};
        Color fill{0.35, 0.55, 0.85, 1.0};
        Color fill_disabled{0.30, 0.32, 0.36, 0.5};
        Color thumb{0.92, 0.93, 0.95, 1.0};
        Color thumb_hovered{1.0, 1.0, 1.0, 1.0};
        Color thumb_dragging{0.75, 0.85, 1.0, 1.0};
        Color thumb_disabled{0.65, 0.66, 0.68, 0.6};
        Color tick{0.48, 0.50, 0.56, 1.0};
        f32 track_thickness = 6.0f;
        f32 thumb_size = 18.0f;
        f32 tick_thickness = 1.0f;
        f32 tick_length = 8.0f;
        BorderStyle focused_border{.color = Color{0.55, 0.72, 1.0, 1.0}, .width = BorderWidth::all(1)};
    };

    enum class SliderVisualPart : u8 {
        Root,
        Track,
        Fill,
        Thumb,
        Marker,
        MarkerLabel,
        Label,
        Tooltip,
    };

    [[nodiscard]] inline UString slider_part_id(const ustr &id, SliderVisualPart part, usize occurrence = 0) {
        if (part == SliderVisualPart::Root)
            return UString{id};
        const char *suffix = part == SliderVisualPart::Track         ? "#track"
                             : part == SliderVisualPart::Fill        ? "#fill"
                             : part == SliderVisualPart::Thumb       ? "#thumb"
                             : part == SliderVisualPart::Marker      ? "#marker:"
                             : part == SliderVisualPart::MarkerLabel ? "#marker-label:"
                             : part == SliderVisualPart::Label       ? "#label"
                                                                     : "#tooltip";
        const bool repeated = part == SliderVisualPart::Marker || part == SliderVisualPart::MarkerLabel;
        UString result{id};
        result.append(suffix);
        if (repeated) {
            const auto occurrence_text = std::to_string(occurrence);
            result.append(occurrence_text.c_str());
        }
        return result;
    }

    [[nodiscard]] inline UString slider_part_id(const UString &id, SliderVisualPart part, usize occurrence = 0) {
        return slider_part_id(id.as_ustr(), part, occurrence);
    }

    struct SliderPartContext {
        SliderVisualPart part = SliderVisualPart::Root;
        PartVisualState visual{};
        UString id;
        SliderOrientation orientation = SliderOrientation::Horizontal;
        bool reversed = false;
        f64 value = 0.0;
        f64 min = 0.0;
        f64 max = 0.0;
        f64 value_fraction = 0.0;
        f64 screen_fraction = 0.0;
        optional<f64> marker_value;
        usize marker_index = 0;
        bool generated_marker = false;
        optional<ElementBounds> bounds;
    };

    struct SliderComposition {
        PartSlot<SliderPartContext> root{};
        PartSlot<SliderPartContext> track{};
        PartSlot<SliderPartContext> fill{};
        PartSlot<SliderPartContext> thumb{};
        PartSlot<SliderPartContext> marker{};
        PartSlot<SliderPartContext> marker_label{.visible = false, .render_default = false};
        PartSlot<SliderPartContext> label{.visible = false, .render_default = false};
        PartSlot<SliderPartContext> tooltip{.visible = false, .render_default = false};
    };

    // Persistent gesture state, one instance per logical slider. Value remains caller-owned and is
    // returned through SliderResult, consistent with dropdown()/toggle widgets in this package.
    class SliderState {
      public:
        [[nodiscard]] bool dragging() const noexcept { return dragging_; }

      private:
        friend struct DetailSliderAccess;
        bool dragging_ = false;
        bool changed_during_gesture_ = false;
        bool drag_from_thumb_ = false;
        f32 grab_offset_ = 0.0f;
    };

    struct SliderResult {
        f64 value = 0.0;
        // True only when user interaction changed the value this frame.
        bool changed = false;
        // Pointer: true on release if that gesture changed the value. Keyboard: true with changed.
        bool committed = false;
        // True when an invalid/out-of-range/non-step-aligned caller value was normalized. This is
        // deliberately separate from changed so programmatic sanitization does not masquerade as UI.
        bool adjusted = false;
        bool cancelled = false;
        bool hovered = false;
        bool dragging = false;
        bool focused = false;
    };

    struct DetailSliderAccess {
        static bool &dragging(SliderState &state) noexcept { return state.dragging_; }
        static bool &gesture_changed(SliderState &state) noexcept { return state.changed_during_gesture_; }
        static bool &drag_from_thumb(SliderState &state) noexcept { return state.drag_from_thumb_; }
        static f32 &grab_offset(SliderState &state) noexcept { return state.grab_offset_; }
    };

    namespace Detail {

        struct SliderRange {
            f64 min = 0.0;
            f64 max = 100.0;
            optional<f64> step = 1.0;
        };

        [[nodiscard]] inline SliderRange slider_range(const SliderConfig &config) noexcept {
            SliderRange result;
            result.min = std::isfinite(config.min) ? config.min : 0.0;
            result.max = std::isfinite(config.max) ? config.max : 100.0;
            if (result.max < result.min) {
                result.max = result.min;
            }
            if (!config.step.has_value()) {
                result.step = std::nullopt;
            } else {
                const f64 step = *config.step;
                result.step = std::isfinite(step) && step > 0.0 ? step : 1.0;
            }
            return result;
        }

        [[nodiscard]] inline f64 sanitize_slider_value(f64 value, const SliderRange &range) noexcept {
            if (!std::isfinite(value)) {
                value = range.min + (range.max - range.min) * 0.5;
            }
            value = std::clamp(value, range.min, range.max);
            if (range.step.has_value() && range.max > range.min) {
                const f64 steps = (value - range.min) / *range.step;
                const f64 max_steps = std::floor((range.max - range.min) / *range.step);
                // floor(x + .5) resolves exact ties toward the greater valid value, as HTML does.
                // Clamp the *index*, not the resulting value: max itself is not a valid value when
                // it is off the min-relative step grid (e.g. min=0, max=10, step=6 => max value 6).
                const f64 snapped_steps = std::clamp(std::floor(steps + 0.5), 0.0, max_steps);
                value = range.min + snapped_steps * *range.step;
            }
            return value;
        }

        [[nodiscard]] inline bool slider_values_equal(f64 lhs, f64 rhs, const SliderRange &range) noexcept {
            if (lhs == rhs) {
                return true;
            }
            const f64 scale = std::max({1.0, std::abs(lhs), std::abs(rhs), std::abs(range.max - range.min)});
            return std::abs(lhs - rhs) <= std::numeric_limits<f64>::epsilon() * scale * 8.0;
        }

        [[nodiscard]] inline f64 slider_fraction(f64 value, const SliderRange &range) noexcept {
            return range.max > range.min ? std::clamp((value - range.min) / (range.max - range.min), 0.0, 1.0) : 0.0;
        }

        [[nodiscard]] inline f32 declared_axis_size(const ElementDecl &decl, SliderOrientation orientation) noexcept {
            const SizingAxis &axis = orientation == SliderOrientation::Horizontal ? decl.sizing.width : decl.sizing.height;
            return axis.kind == SizingKind::Fixed ? std::max(axis.value, 0.0f) : 0.0f;
        }

        [[nodiscard]] inline f64 pointer_slider_value(const Context &ctx, const ElementBounds &bounds, const SliderConfig &config, const SliderRange &range, f32 thumb_size, f32 grab_offset) noexcept {
            const bool horizontal = config.orientation == SliderOrientation::Horizontal;
            const f32 length = horizontal ? bounds.size.x : bounds.size.y;
            const f32 origin = horizontal ? bounds.position.x : bounds.position.y;
            const f32 pointer = horizontal ? ctx.pointer_position().x : ctx.pointer_position().y;
            const f32 travel = std::max(length - thumb_size, 0.0f);
            if (travel <= 0.0f || range.max <= range.min) {
                return range.min;
            }
            const f64 screen_fraction = std::clamp(static_cast<f64>((pointer - origin - thumb_size * 0.5f - grab_offset) / travel), 0.0, 1.0);
            f64 value_fraction = screen_fraction;
            if (horizontal) {
                if (config.reversed)
                    value_fraction = 1.0 - value_fraction;
            } else if (!config.reversed) {
                value_fraction = 1.0 - value_fraction;
            }
            return sanitize_slider_value(range.min + value_fraction * (range.max - range.min), range);
        }

        [[nodiscard]] inline f64 keyboard_step(const SliderConfig &config, const SliderRange &range) noexcept {
            if (config.keyboard_step.has_value() && std::isfinite(*config.keyboard_step) && *config.keyboard_step > 0.0) {
                return *config.keyboard_step;
            }
            if (range.step.has_value()) {
                return *range.step;
            }
            return std::max((range.max - range.min) / 100.0, std::numeric_limits<f64>::epsilon());
        }

        inline void render_slider_mark(Context &ctx, SliderOrientation orientation, f64 screen_fraction, f32 travel, f32 thumb_size, const SliderStyle &style) {
            ElementDecl mark{
                .sizing = orientation == SliderOrientation::Horizontal
                              ? Sizing{SizingAxis::fixed(style.tick_thickness), SizingAxis::fixed(style.tick_length)}
                              : Sizing{SizingAxis::fixed(style.tick_length), SizingAxis::fixed(style.tick_thickness)},
                .background_color = style.tick,
                .floating = FloatingConfig{
                    .attach_to = FloatingAttachTo::Parent,
                    .element_attach_point = FloatingAttachPoint::CenterCenter,
                    .parent_attach_point = orientation == SliderOrientation::Horizontal
                                               ? FloatingAttachPoint::LeftCenter
                                               : FloatingAttachPoint::CenterTop,
                    .offset = orientation == SliderOrientation::Horizontal
                                  ? glm::vec2{thumb_size * 0.5f + static_cast<f32>(screen_fraction) * travel, 0.0f}
                                  : glm::vec2{0.0f, thumb_size * 0.5f + static_cast<f32>(screen_fraction) * travel},
                    .capture_pointer = false,
                    // A tick mark is part of the track's own body, not a popover — clip it with
                    // whatever ancestor (e.g. a scrollable panel) clips the track itself.
                    .clip_to = FloatingClipTo::AttachedParent,
                },
            };
            auto scope = ctx.element(mark);
            (void)scope;
        }

    } // namespace Detail

    // `decl.id` must be non-empty and stable. The whole declared box is the hit target; its previous
    // frame bounds determine pointer mapping, so Grow/Percent sliders work as well as fixed ones.
    [[nodiscard]] inline SliderResult slider(Context &ctx, const ElementDecl &decl, const SliderConfig &config, const SliderStyle &style, SliderState &state, f64 value, const SliderInput &input, bool enabled, const SliderComposition &composition) {
        const Detail::SliderRange range = Detail::slider_range(config);
        SliderResult result{.value = Detail::sanitize_slider_value(value, range)};
        result.adjusted = !std::isfinite(value) || !Detail::slider_values_equal(result.value, value, range);

        const UString thumb_id = slider_part_id(decl.id.as_ustr(), SliderVisualPart::Thumb);
        const bool root_visible = composition.root.visible;
        const bool slider_enabled = enabled && root_visible && composition.root.enabled;
        const bool track_enabled = slider_enabled && composition.track.visible && composition.track.enabled;
        const bool thumb_enabled = slider_enabled && composition.thumb.visible && composition.thumb.enabled;
        result.hovered = slider_enabled && (ctx.hovered(decl.id) ||
                                            (composition.thumb.visible && ctx.hovered(thumb_id)));

        bool &dragging = DetailSliderAccess::dragging(state);
        bool &gesture_changed = DetailSliderAccess::gesture_changed(state);
        bool &drag_from_thumb = DetailSliderAccess::drag_from_thumb(state);
        f32 &grab_offset = DetailSliderAccess::grab_offset(state);

        if (dragging && ((drag_from_thumb && !thumb_enabled) || (!drag_from_thumb && !track_enabled))) {
            if (ctx.has_pointer_capture(decl.id))
                ctx.release_pointer(decl.id);
            dragging = false;
            gesture_changed = false;
            result.cancelled = true;
        }

        if (!slider_enabled || decl.id.empty()) {
            if (ctx.has_pointer_capture(decl.id)) {
                ctx.release_pointer(decl.id);
            }
            ctx.clear_focus(decl.id);
            dragging = false;
            gesture_changed = false;
        } else {
            if (dragging && !ctx.has_pointer_capture(decl.id)) {
                // The widget may have been omitted on the release frame; Context drops orphaned
                // capture, so clear caller-owned gesture state when it reappears.
                dragging = false;
                gesture_changed = false;
                result.cancelled = true;
            }
            if (input.request_blur) {
                ctx.clear_focus(decl.id);
            }
            if (input.request_focus) {
                ctx.focus(decl.id);
            }

            const bool thumb_pressed = thumb_enabled && ctx.clicked(thumb_id);
            const bool track_pressed = track_enabled && ctx.clicked(decl.id);
            const bool control_pressed = thumb_pressed || track_pressed;
            const optional<ElementBounds> bounds = ctx.element_bounds(decl.id);
            const f32 thumb_size = std::max(style.thumb_size, 1.0f);
            if (control_pressed && bounds.has_value() && ctx.try_capture_pointer(decl.id)) {
                ctx.focus(decl.id);
                dragging = true;
                gesture_changed = false;
                drag_from_thumb = thumb_pressed;
                grab_offset = 0.0f;
                if (thumb_pressed) {
                    const f64 value_fraction = Detail::slider_fraction(result.value, range);
                    f64 screen_fraction = value_fraction;
                    if (config.orientation == SliderOrientation::Horizontal) {
                        if (config.reversed)
                            screen_fraction = 1.0 - screen_fraction;
                    } else if (!config.reversed) {
                        screen_fraction = 1.0 - screen_fraction;
                    }
                    const bool horizontal = config.orientation == SliderOrientation::Horizontal;
                    const f32 length = horizontal ? bounds->size.x : bounds->size.y;
                    const f32 origin = horizontal ? bounds->position.x : bounds->position.y;
                    const f32 pointer = horizontal ? ctx.pointer_position().x : ctx.pointer_position().y;
                    const f32 thumb_center = origin + thumb_size * 0.5f + static_cast<f32>(screen_fraction) * std::max(length - thumb_size, 0.0f);
                    grab_offset = pointer - thumb_center;
                }
            }

            if (dragging && ctx.has_pointer_capture(decl.id)) {
                if (ctx.pointer_cancelled_this_frame()) {
                    result.cancelled = true;
                    dragging = false;
                    gesture_changed = false;
                    ctx.release_pointer(decl.id);
                } else if (bounds.has_value()) {
                    const f64 next = Detail::pointer_slider_value(ctx, *bounds, config, range, thumb_size, grab_offset);
                    if (!Detail::slider_values_equal(next, result.value, range)) {
                        result.value = next;
                        result.changed = true;
                        gesture_changed = true;
                    }
                }

                if (dragging && (ctx.pointer_released_this_frame() || (!ctx.pointer_is_down() && !ctx.pointer_pressed_this_frame()))) {
                    result.committed = gesture_changed;
                    dragging = false;
                    gesture_changed = false;
                    ctx.release_pointer(decl.id);
                }
            }

            if (ctx.pointer_pressed_this_frame() && !result.hovered && !dragging) {
                ctx.clear_focus(decl.id);
            }

            if (ctx.has_focus(decl.id) && !dragging) {
                const f64 small_step = Detail::keyboard_step(config, range);
                const f64 page_step = config.page_step.has_value() && std::isfinite(*config.page_step) && *config.page_step > 0.0
                                          ? *config.page_step
                                          : std::max(small_step * 10.0, (range.max - range.min) / 10.0);
                for (SliderKey key : input.keys) {
                    f64 next = result.value;
                    switch (key) {
                        case SliderKey::Decrement:
                            next -= small_step;
                            break;
                        case SliderKey::Increment:
                            next += small_step;
                            break;
                        case SliderKey::PageDecrement:
                            next -= page_step;
                            break;
                        case SliderKey::PageIncrement:
                            next += page_step;
                            break;
                        case SliderKey::Minimum:
                            next = range.min;
                            break;
                        case SliderKey::Maximum:
                            next = range.max;
                            break;
                    }
                    next = Detail::sanitize_slider_value(next, range);
                    if (!Detail::slider_values_equal(next, result.value, range)) {
                        result.value = next;
                        result.changed = true;
                        result.committed = true;
                    }
                }
            }
        }

        result.dragging = dragging && ctx.has_pointer_capture(decl.id);
        result.focused = slider_enabled && ctx.has_focus(decl.id);

        const f64 value_fraction = Detail::slider_fraction(result.value, range);
        f64 screen_fraction = value_fraction;
        if (config.orientation == SliderOrientation::Horizontal) {
            if (config.reversed)
                screen_fraction = 1.0 - screen_fraction;
        } else if (!config.reversed) {
            screen_fraction = 1.0 - screen_fraction;
        }

        if (!root_visible)
            return result;

        const optional<ElementBounds> bounds = ctx.element_bounds(decl.id);
        const f32 length = bounds.has_value()
                               ? (config.orientation == SliderOrientation::Horizontal ? bounds->size.x : bounds->size.y)
                               : Detail::declared_axis_size(decl, config.orientation);
        const f32 thumb_size = std::max(style.thumb_size, 1.0f);
        const f32 travel = std::max(length - thumb_size, 0.0f);
        const bool horizontal = config.orientation == SliderOrientation::Horizontal;

        const auto make_context = [&](SliderVisualPart part, const UString &part_id, const PartVisualState &visual, f64 part_screen_fraction = 0.0, optional<f64> marker_value = std::nullopt, usize marker_index = 0, bool generated_marker = false) {
            return SliderPartContext{
                .part = part,
                .visual = visual,
                .id = part_id,
                .orientation = config.orientation,
                .reversed = config.reversed,
                .value = result.value,
                .min = range.min,
                .max = range.max,
                .value_fraction = value_fraction,
                .screen_fraction = part_screen_fraction,
                .marker_value = marker_value,
                .marker_index = marker_index,
                .generated_marker = generated_marker,
                .bounds = ctx.element_bounds(part_id),
            };
        };

        PartVisualState root_visual{
            .enabled = slider_enabled,
            .hovered = result.hovered,
            .active = result.dragging,
            .focused = result.focused,
        };
        SliderPartContext root_context = make_context(
            SliderVisualPart::Root,
            decl.id,
            root_visual,
            screen_fraction);
        ElementDecl root = decl;
        root.child_alignment = {AlignX::Center, AlignY::Center};
        if (result.focused)
            root.border = style.focused_border;
        if (!composition.root.render_default)
            clear_element_visual(root);
        apply_part_visual(root, composition.root.visual, root_visual);
        if (composition.root.alter_decl)
            composition.root.alter_decl(root, root_context);
        if (root.cursor == CursorIcon::Auto) {
            root.cursor = !slider_enabled ? CursorIcon::NotAllowed
                                         : result.dragging ? CursorIcon::Grabbing
                                                           : CursorIcon::Pointer;
        }
        root.id = decl.id;
        auto root_scope = ctx.element(root);
        (void)root_scope;

        const auto render_slot = [&](const PartSlot<SliderPartContext> &slot, ElementDecl part_decl, SliderPartContext part_context) {
            if (!slot.visible)
                return;
            if (!slot.render_default)
                clear_element_visual(part_decl);
            apply_part_visual(part_decl, slot.visual, part_context.visual);
            if (slot.alter_decl)
                slot.alter_decl(part_decl, part_context);
            part_decl.id = part_context.id;
            auto scope = ctx.element(part_decl);
            (void)scope;
            if (slot.build)
                slot.build(ctx, part_context);
        };

        if (composition.label.visible) {
            const UString label_id = slider_part_id(decl.id.as_ustr(), SliderVisualPart::Label);
            PartVisualState label_visual{.enabled = slider_enabled && composition.label.enabled};
            SliderPartContext label_context = make_context(
                SliderVisualPart::Label,
                label_id,
                label_visual,
                screen_fraction);
            render_slot(composition.label,
                        ElementDecl{
                            .sizing = {SizingAxis::fit(), SizingAxis::fit()},
                            .floating = FloatingConfig{
                                .attach_to = FloatingAttachTo::Parent,
                                .element_attach_point = FloatingAttachPoint::RightCenter,
                                .parent_attach_point = FloatingAttachPoint::LeftCenter,
                                .offset = {-8.0f, 0.0f},
                                .capture_pointer = false,
                                // Unlike Tooltip below, this is a persistent value readout glued to
                                // the slider's own body, not a transient popover — clip it with
                                // whatever ancestor clips the slider itself.
                                .clip_to = FloatingClipTo::AttachedParent,
                            },
                        },
                        label_context);
        }

        const UString track_id = slider_part_id(decl.id.as_ustr(), SliderVisualPart::Track);
        PartVisualState track_visual{
            .enabled = track_enabled,
            .hovered = track_enabled && ctx.hovered(decl.id),
            .pressed = track_enabled && ctx.pointer_down(decl.id),
            .active = result.dragging && !drag_from_thumb,
            .focused = result.focused,
        };
        SliderPartContext track_context = make_context(
            SliderVisualPart::Track,
            track_id,
            track_visual,
            screen_fraction);
        render_slot(composition.track,
                    ElementDecl{
                        .sizing = horizontal ? Sizing{SizingAxis::fixed(travel), SizingAxis::fixed(style.track_thickness)}
                                             : Sizing{SizingAxis::fixed(style.track_thickness), SizingAxis::fixed(travel)},
                        .background_color = track_enabled ? style.track : style.track_disabled,
                        .corner_radius = CornerRadius::all(style.track_thickness * 0.5f),
                    },
                    track_context);

        if (composition.fill.visible) {
            const f32 fill_length = static_cast<f32>(value_fraction) * travel;
            const UString fill_id = slider_part_id(decl.id.as_ustr(), SliderVisualPart::Fill);
            PartVisualState fill_visual{
                .enabled = slider_enabled && composition.fill.enabled,
                .hovered = result.hovered,
                .active = result.dragging,
                .focused = result.focused,
            };
            SliderPartContext fill_context = make_context(
                SliderVisualPart::Fill,
                fill_id,
                fill_visual,
                screen_fraction);
            render_slot(composition.fill,
                        ElementDecl{
                            .sizing = horizontal ? Sizing{SizingAxis::fixed(fill_length), SizingAxis::fixed(style.track_thickness)}
                                                 : Sizing{SizingAxis::fixed(style.track_thickness), SizingAxis::fixed(fill_length)},
                            .background_color = fill_visual.enabled ? style.fill : style.fill_disabled,
                            .corner_radius = CornerRadius::all(style.track_thickness * 0.5f),
                            .floating = FloatingConfig{
                                .attach_to = FloatingAttachTo::Parent,
                                .element_attach_point = horizontal
                                                            ? (config.reversed ? FloatingAttachPoint::RightCenter : FloatingAttachPoint::LeftCenter)
                                                            : (config.reversed ? FloatingAttachPoint::CenterTop : FloatingAttachPoint::CenterBottom),
                                .parent_attach_point = horizontal
                                                           ? (config.reversed ? FloatingAttachPoint::RightCenter : FloatingAttachPoint::LeftCenter)
                                                           : (config.reversed ? FloatingAttachPoint::CenterTop : FloatingAttachPoint::CenterBottom),
                                .offset = horizontal
                                              ? glm::vec2{config.reversed ? -thumb_size * 0.5f : thumb_size * 0.5f, 0.0f}
                                              : glm::vec2{0.0f, config.reversed ? thumb_size * 0.5f : -thumb_size * 0.5f},
                                .capture_pointer = false,
                                // The filled portion is part of the track's own body — clip it with
                                // whatever ancestor clips the track.
                                .clip_to = FloatingClipTo::AttachedParent,
                            },
                        },
                        fill_context);
        }

        usize marker_index = 0;
        const auto render_marker = [&](f64 tick_value, bool generated) {
            if (!composition.marker.visible || !std::isfinite(tick_value) ||
                tick_value < range.min || tick_value > range.max)
                return;
            f64 fraction = Detail::slider_fraction(tick_value, range);
            if (horizontal) {
                if (config.reversed)
                    fraction = 1.0 - fraction;
            } else if (!config.reversed) {
                fraction = 1.0 - fraction;
            }
            const glm::vec2 offset = horizontal
                                         ? glm::vec2{thumb_size * 0.5f + static_cast<f32>(fraction) * travel, 0.0f}
                                         : glm::vec2{0.0f, thumb_size * 0.5f + static_cast<f32>(fraction) * travel};
            const UString marker_id = slider_part_id(decl.id.as_ustr(), SliderVisualPart::Marker, marker_index);
            PartVisualState marker_visual{.enabled = slider_enabled && composition.marker.enabled};
            SliderPartContext marker_context = make_context(
                SliderVisualPart::Marker,
                marker_id,
                marker_visual,
                fraction,
                tick_value,
                marker_index,
                generated);
            render_slot(composition.marker,
                        ElementDecl{
                            .sizing = horizontal
                                          ? Sizing{SizingAxis::fixed(style.tick_thickness), SizingAxis::fixed(style.tick_length)}
                                          : Sizing{SizingAxis::fixed(style.tick_length), SizingAxis::fixed(style.tick_thickness)},
                            .background_color = style.tick,
                            .floating = FloatingConfig{
                                .attach_to = FloatingAttachTo::Parent,
                                .element_attach_point = FloatingAttachPoint::CenterCenter,
                                .parent_attach_point = horizontal ? FloatingAttachPoint::LeftCenter : FloatingAttachPoint::CenterTop,
                                .offset = offset,
                                .capture_pointer = false,
                                // Part of the track's own body — clip with whatever ancestor clips
                                // the track.
                                .clip_to = FloatingClipTo::AttachedParent,
                            },
                        },
                        marker_context);

            if (composition.marker_label.visible) {
                const UString label_id = slider_part_id(decl.id.as_ustr(), SliderVisualPart::MarkerLabel, marker_index);
                PartVisualState marker_label_visual{
                    .enabled = slider_enabled && composition.marker_label.enabled,
                };
                SliderPartContext label_context = make_context(
                    SliderVisualPart::MarkerLabel,
                    label_id,
                    marker_label_visual,
                    fraction,
                    tick_value,
                    marker_index,
                    generated);
                const glm::vec2 label_offset = horizontal
                                                   ? offset + glm::vec2{0.0f, style.tick_length * 0.5f + 2.0f}
                                                   : offset + glm::vec2{style.tick_length * 0.5f + 2.0f, 0.0f};
                render_slot(composition.marker_label,
                            ElementDecl{
                                .sizing = {SizingAxis::fit(), SizingAxis::fit()},
                                .floating = FloatingConfig{
                                    .attach_to = FloatingAttachTo::Parent,
                                    .element_attach_point = horizontal ? FloatingAttachPoint::CenterTop : FloatingAttachPoint::LeftCenter,
                                    .parent_attach_point = horizontal ? FloatingAttachPoint::LeftCenter : FloatingAttachPoint::CenterTop,
                                    .offset = label_offset,
                                    .capture_pointer = false,
                                    // A tick's own annotation — part of the track's body, not a
                                    // popover — clip with whatever ancestor clips the track.
                                    .clip_to = FloatingClipTo::AttachedParent,
                                },
                            },
                            label_context);
            }
            ++marker_index;
        };
        for (const SliderTick &tick : config.ticks)
            render_marker(tick.value, false);
        if (config.show_step_ticks && range.step.has_value() && *range.step > 0.0 && config.max_generated_ticks > 0) {
            const f64 count_f = std::floor((range.max - range.min) / *range.step) + 1.0;
            const usize count = static_cast<usize>(std::min<f64>(count_f, static_cast<f64>(config.max_generated_ticks)));
            for (usize i = 0; i < count; ++i)
                render_marker(range.min + static_cast<f64>(i) * *range.step, true);
        }

        if (composition.thumb.visible) {
            const bool thumb_hovered = thumb_enabled && result.hovered;
            PartVisualState thumb_visual{
                .enabled = thumb_enabled,
                .hovered = thumb_hovered,
                .pressed = thumb_enabled && ctx.pointer_down(thumb_id),
                .active = result.dragging && drag_from_thumb,
                .focused = result.focused,
            };
            const Color thumb_color = !thumb_enabled    ? style.thumb_disabled
                                      : result.dragging ? style.thumb_dragging
                                      : thumb_hovered   ? style.thumb_hovered
                                                        : style.thumb;
            SliderPartContext thumb_context = make_context(
                SliderVisualPart::Thumb,
                thumb_id,
                thumb_visual,
                screen_fraction);
            render_slot(composition.thumb,
                        ElementDecl{
                            .sizing = {SizingAxis::fixed(thumb_size), SizingAxis::fixed(thumb_size)},
                            .background_color = thumb_color,
                            .corner_radius = CornerRadius::all(thumb_size * 0.5f),
                            .floating = FloatingConfig{
                                .attach_to = FloatingAttachTo::Parent,
                                .element_attach_point = FloatingAttachPoint::CenterCenter,
                                .parent_attach_point = horizontal ? FloatingAttachPoint::LeftCenter : FloatingAttachPoint::CenterTop,
                                .offset = horizontal
                                              ? glm::vec2{thumb_size * 0.5f + static_cast<f32>(screen_fraction) * travel, 0.0f}
                                              : glm::vec2{0.0f, thumb_size * 0.5f + static_cast<f32>(screen_fraction) * travel},
                                // The thumb is the slider's own body, not a popover — if an ancestor
                                // (e.g. a scrollable panel) clips the track, the thumb must clip right
                                // along with it instead of painting on top of the clipped-away region.
                                .clip_to = FloatingClipTo::AttachedParent,
                            },
                            // More specific than the root's own Pointer default (Grab, not just
                            // Pointer) — this *is* the drag handle, not just "somewhere clickable."
                            .cursor = !thumb_enabled ? CursorIcon::NotAllowed
                                                     : thumb_visual.active ? CursorIcon::Grabbing
                                                                           : CursorIcon::Grab,
                        },
                        thumb_context);
        }

        if (composition.tooltip.visible && composition.tooltip.build &&
            (result.hovered || result.focused || result.dragging)) {
            const UString tooltip_id = slider_part_id(decl.id.as_ustr(), SliderVisualPart::Tooltip);
            PartVisualState tooltip_visual{
                .enabled = slider_enabled && composition.tooltip.enabled,
                .hovered = result.hovered,
                .active = result.dragging,
                .focused = result.focused,
            };
            SliderPartContext tooltip_context = make_context(
                SliderVisualPart::Tooltip,
                tooltip_id,
                tooltip_visual,
                screen_fraction);
            render_slot(composition.tooltip,
                        ElementDecl{
                            .sizing = {SizingAxis::fit(), SizingAxis::fit()},
                            .floating = FloatingConfig{
                                .attach_to = FloatingAttachTo::ElementWithId,
                                .parent_id = composition.thumb.visible ? thumb_id : decl.id,
                                .element_attach_point = FloatingAttachPoint::LeftBottom,
                                .parent_attach_point = FloatingAttachPoint::LeftTop,
                                .offset = {0.0f, -4.0f},
                                .z_index = 100,
                                .capture_pointer = false,
                                // Deliberately unclipped (Dropdown.hpp's tooltip carries the same
                                // reasoning): a tooltip cut off by its own slider's scroll-container
                                // ancestor would defeat the point of a tooltip.
                            },
                        },
                        tooltip_context);
        }

        if (composition.root.build)
            composition.root.build(ctx, root_context);
        return result;
    }

    [[nodiscard]] inline SliderResult slider(Context &ctx, const ElementDecl &decl, const SliderConfig &config, const SliderStyle &style, SliderState &state, f64 value, const SliderInput &input = {}, bool enabled = true) {
        return slider(ctx, decl, config, style, state, value, input, enabled, SliderComposition{});
    }

} // namespace SFT::UI
