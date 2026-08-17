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


namespace SFT::UI {

    enum class SliderOrientation : u8 { Horizontal,
                                        Vertical };


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


        optional<f64> step = 1.0;

        optional<f64> keyboard_step;

        optional<f64> page_step;
        SliderOrientation orientation = SliderOrientation::Horizontal;


        bool reversed = false;
        span<const SliderTick> ticks{};


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

    /// Performs the slider part ID operation using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    /// @param part `part` value used by the operation.
    /// @param occurrence `occurrence` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] UString slider_part_id(const ustr &id, SliderVisualPart part, usize occurrence = 0);

    /// Performs the slider part ID operation using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    /// @param part `part` value used by the operation.
    /// @param occurrence `occurrence` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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


    class SliderState {
      public:
        /// Returns the current or globally available dragging value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
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

        bool changed = false;

        bool committed = false;


        bool adjusted = false;
        bool cancelled = false;
        bool hovered = false;
        bool dragging = false;
        bool focused = false;
    };

    struct DetailSliderAccess {
        /// Performs the dragging operation for `DetailSliderAccess` using the supplied arguments.
        ///
        /// @param state `state` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static bool &dragging(SliderState &state) noexcept;
        /// Performs the gesture changed operation for `DetailSliderAccess` using the supplied arguments.
        ///
        /// @param state `state` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static bool &gesture_changed(SliderState &state) noexcept;
        /// Performs the drag from thumb operation for `DetailSliderAccess` using the supplied arguments.
        ///
        /// @param state `state` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static bool &drag_from_thumb(SliderState &state) noexcept;
        /// Computes the grab offset required by the supplied values.
        ///
        /// @param state `state` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static f32 &grab_offset(SliderState &state) noexcept;
    };

    namespace Detail {

        struct SliderRange {
            f64 min = 0.0;
            f64 max = 100.0;
            optional<f64> step = 1.0;
        };

        /// Performs the slider range operation using the supplied arguments.
        ///
        /// @param config Configuration values controlling the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SliderRange slider_range(const SliderConfig &config) noexcept;

        /// Performs the sanitize slider value operation using the supplied arguments.
        ///
        /// @param value Value consumed by the operation.
        /// @param range Range of values to process.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f64 sanitize_slider_value(f64 value, const SliderRange &range) noexcept;

        /// Performs the slider values equal operation using the supplied arguments.
        ///
        /// @param lhs Left-hand operand.
        /// @param rhs Right-hand operand.
        /// @param range Range of values to process.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool slider_values_equal(f64 lhs, f64 rhs, const SliderRange &range) noexcept;

        /// Performs the slider fraction operation using the supplied arguments.
        ///
        /// @param value Value consumed by the operation.
        /// @param range Range of values to process.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f64 slider_fraction(f64 value, const SliderRange &range) noexcept;

        /// Returns the requested declared axis size.
        ///
        /// @param decl `decl` value used by the operation.
        /// @param orientation `orientation` value used by the operation.
        ///
        /// @return Returns the requested count or size.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 declared_axis_size(const ElementDecl &decl, SliderOrientation orientation) noexcept;

        /// Performs the pointer slider value operation using the supplied arguments.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param bounds `bounds` value used by the operation.
        /// @param config Configuration values controlling the operation.
        /// @param range Range of values to process.
        /// @param thumb_size Requested or available size for the operation.
        /// @param grab_offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f64 pointer_slider_value(const Context &ctx, const ElementBounds &bounds, const SliderConfig &config, const SliderRange &range, f32 thumb_size, f32 grab_offset) noexcept;

        /// Performs the keyboard step operation using the supplied arguments.
        ///
        /// @param config Configuration values controlling the operation.
        /// @param range Range of values to process.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f64 keyboard_step(const SliderConfig &config, const SliderRange &range) noexcept;

        /// Renders slider mark using the current rendering state.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param orientation `orientation` value used by the operation.
        /// @param screen_fraction `screen_fraction` value used by the operation.
        /// @param travel `travel` value used by the operation.
        /// @param thumb_size Requested or available size for the operation.
        /// @param style `style` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void render_slider_mark(Context &ctx, SliderOrientation orientation, f64 screen_fraction, f32 travel, f32 thumb_size, const SliderStyle &style);

    } // namespace Detail


    /// Performs the slider operation using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param decl `decl` value used by the operation.
    /// @param config Configuration values controlling the operation.
    /// @param style `style` value used by the operation.
    /// @param state `state` value used by the operation.
    /// @param value Value consumed by the operation.
    /// @param input `input` value used by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    /// @param composition `composition` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] SliderResult slider(Context &ctx, const ElementDecl &decl, const SliderConfig &config, const SliderStyle &style, SliderState &state, f64 value, const SliderInput &input, bool enabled, const SliderComposition &composition);

    [[nodiscard]] SliderResult slider(Context &ctx, const ElementDecl &decl, const SliderConfig &config, const SliderStyle &style, SliderState &state, f64 value, const SliderInput &input = {}, bool enabled = true);

} // namespace SFT::UI
