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

/// Immediate-mode numeric range control with the interaction/value semantics expected from an HTML
/// input[type=range]: bounded values, min-relative step snapping (or step="any"), track clicks,
/// captured thumb dragging, disabled state, focus, repeated keyboard intents, Home/End/Page steps,
/// arbitrary datalist-like tick marks, and separate live-change/gesture-commit signals. UI remains
/// input-backend-agnostic: callers translate physical keys into SliderKey just as TextEditInput does.
namespace SFT::UI {

    enum class SliderOrientation : u8 { Horizontal,
                                        Vertical };

    /// Numeric intents rather than physical keys. Increment always raises the value and Decrement
    /// always lowers it, regardless of orientation/reversal; the platform layer decides which arrow
    /// key maps to which intent.
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
        /// A positive value snaps to min + N*step. std::nullopt is HTML's step="any". Invalid
        /// numeric steps fall back to HTML's default of 1 rather than silently becoming continuous.
        optional<f64> step = 1.0;
        /// Used only for keyboard intents. Omitted: numeric step, or 1% of the range for step="any".
        optional<f64> keyboard_step;
        /// Omitted: max(10 * keyboard step, 10% of the range).
        optional<f64> page_step;
        SliderOrientation orientation = SliderOrientation::Horizontal;
        /// Horizontal defaults to min-left/max-right. Vertical defaults to min-bottom/max-top;
        /// reversed swaps those visual endpoints without changing numeric keyboard semantics.
        bool reversed = false;
        span<const SliderTick> ticks{};
        /// Generates regular marks from min by step, capped by max_generated_ticks. Arbitrary ticks
        /// above are visual suggestions only, matching datalist behavior; neither kind changes snap.
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

    [[nodiscard]] UString slider_part_id(const ustr &id, SliderVisualPart part, usize occurrence = 0);

    [[nodiscard]] UString slider_part_id(const UString &id, SliderVisualPart part, usize occurrence = 0);

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

    /// Persistent gesture state, one instance per logical slider. Value remains caller-owned and is
    /// returned through SliderResult, consistent with dropdown()/toggle widgets in this package.
    class SliderState {
      public:
        [[nodiscard]] bool dragging() const noexcept;

      private:
        friend struct DetailSliderAccess;
        bool dragging_ = false;
        bool changed_during_gesture_ = false;
        bool drag_from_thumb_ = false;
        f32 grab_offset_ = 0.0f;
    };

    struct SliderResult {
        f64 value = 0.0;
        /// True only when user interaction changed the value this frame.
        bool changed = false;
        /// Pointer: true on release if that gesture changed the value. Keyboard: true with changed.
        bool committed = false;
        /// True when an invalid/out-of-range/non-step-aligned caller value was normalized. This is
        /// deliberately separate from changed so programmatic sanitization does not masquerade as UI.
        bool adjusted = false;
        bool cancelled = false;
        bool hovered = false;
        bool dragging = false;
        bool focused = false;
    };

    struct DetailSliderAccess {
        static bool &dragging(SliderState &state) noexcept;
        static bool &gesture_changed(SliderState &state) noexcept;
        static bool &drag_from_thumb(SliderState &state) noexcept;
        static f32 &grab_offset(SliderState &state) noexcept;
    };

    namespace Detail {

        struct SliderRange {
            f64 min = 0.0;
            f64 max = 100.0;
            optional<f64> step = 1.0;
        };

        [[nodiscard]] SliderRange slider_range(const SliderConfig &config) noexcept;

        [[nodiscard]] f64 sanitize_slider_value(f64 value, const SliderRange &range) noexcept;

        [[nodiscard]] bool slider_values_equal(f64 lhs, f64 rhs, const SliderRange &range) noexcept;

        [[nodiscard]] f64 slider_fraction(f64 value, const SliderRange &range) noexcept;

        [[nodiscard]] f32 declared_axis_size(const ElementDecl &decl, SliderOrientation orientation) noexcept;

        [[nodiscard]] f64 pointer_slider_value(const Context &ctx, const ElementBounds &bounds, const SliderConfig &config, const SliderRange &range, f32 thumb_size, f32 grab_offset) noexcept;

        [[nodiscard]] f64 keyboard_step(const SliderConfig &config, const SliderRange &range) noexcept;

        void render_slider_mark(Context &ctx, SliderOrientation orientation, f64 screen_fraction, f32 travel, f32 thumb_size, const SliderStyle &style);

    } // namespace Detail

    /// `decl.id` must be non-empty and stable. The whole declared box is the hit target; its previous
    /// frame bounds determine pointer mapping, so Grow/Percent sliders work as well as fixed ones.
    [[nodiscard]] SliderResult slider(Context &ctx, const ElementDecl &decl, const SliderConfig &config, const SliderStyle &style, SliderState &state, f64 value, const SliderInput &input, bool enabled, const SliderComposition &composition);

    [[nodiscard]] SliderResult slider(Context &ctx, const ElementDecl &decl, const SliderConfig &config, const SliderStyle &style, SliderState &state, f64 value, const SliderInput &input = {}, bool enabled = true);

} // namespace SFT::UI
