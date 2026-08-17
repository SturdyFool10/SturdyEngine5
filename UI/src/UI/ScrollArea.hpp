#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <cmath>
#include <functional>
#pragma endregion

#include "Button.hpp"
#include "Context.hpp"
#include "Style.hpp"
#include "WidgetComposition.hpp"


namespace SFT::UI {

    enum class ScrollbarVisibility : u8 {
        WhenNeeded,
        AlwaysVisible,
        AlwaysHidden,
    };

    struct ScrollbarStyle {


        f32 idle_thickness = 4.0f;
        f32 hovered_thickness = 8.0f;


        Color track_color{0.0, 0.0, 0.0, 0.0};
        Color thumb_color{1.0, 1.0, 1.0, 0.28};
        Color thumb_hovered_color{1.0, 1.0, 1.0, 0.45};
        Color thumb_dragging_color{1.0, 1.0, 1.0, 0.65};
        f32 corner_radius = 3.0f;


        f32 margin = 2.0f;
        f32 min_thumb_length = 24.0f;


        f32 fade_in_seconds = 0.1f;
        f32 fade_out_seconds = 0.4f;


        f32 idle_delay_seconds = 0.4f;
        ScrollbarVisibility visibility = ScrollbarVisibility::WhenNeeded;
    };

    enum class ScrollAreaVisualPart : u8 {
        Viewport,
        VerticalTrack,
        VerticalThumb,
        HorizontalTrack,
        HorizontalThumb,
    };

    /// Scrolls area part ID using the supplied arguments and current state.
    ///
    /// @param id Identifier of the target object or resource.
    /// @param part `part` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] UString scroll_area_part_id(const UString &id, ScrollAreaVisualPart part);

    struct ScrollAreaPartContext {
        ScrollAreaVisualPart part = ScrollAreaVisualPart::Viewport;
        PartVisualState visual{};
        UString id;

        f64 value_fraction = 0.0;

        f64 visible_fraction = 1.0;
        std::optional<ElementBounds> bounds;
    };

    struct ScrollAreaComposition {
        PartSlot<ScrollAreaPartContext> viewport{};
        PartSlot<ScrollAreaPartContext> vertical_track{};
        PartSlot<ScrollAreaPartContext> vertical_thumb{};
        PartSlot<ScrollAreaPartContext> horizontal_track{};
        PartSlot<ScrollAreaPartContext> horizontal_thumb{};
    };


    class ScrollAreaState {
      public:


        struct Axis {
            f32 opacity = 0.0f;
            f32 opacity_start = 0.0f;
            f32 opacity_target = 0.0f;
            f32 opacity_elapsed = 0.0f;
            bool opacity_initialized = false;

            f32 idle_seconds = 1.0e6f;
            f32 last_offset = 0.0f;
            bool last_offset_valid = false;
            bool dragging = false;
            f32 grab_offset = 0.0f;
        };

      private:
        friend struct DetailScrollAreaAccess;

        Axis vertical_{};
        Axis horizontal_{};
    };

    struct DetailScrollAreaAccess {
        /// Performs the vertical operation for `DetailScrollAreaAccess` using the supplied arguments.
        ///
        /// @param state `state` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static ScrollAreaState::Axis &vertical(ScrollAreaState &state) noexcept;
        /// Performs the horizontal operation for `DetailScrollAreaAccess` using the supplied arguments.
        ///
        /// @param state `state` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static ScrollAreaState::Axis &horizontal(ScrollAreaState &state) noexcept;
    };

    struct ScrollAreaResult {
        bool vertical_dragging = false;
        bool horizontal_dragging = false;
    };

    using ScrollAreaBuilder = std::function<void(Context &)>;

    namespace Detail {


        /// Updates scrollbar axis from the supplied values.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param container_id Identifier of the target object or resource.
        /// @param vertical `vertical` value used by the operation.
        /// @param style `style` value used by the operation.
        /// @param axis `axis` value used by the operation.
        /// @param delta_seconds `delta_seconds` value used by the operation.
        /// @param enabled Whether the associated behavior is enabled.
        /// @param metrics `metrics` value used by the operation.
        /// @param track_slot Binding or storage slot addressed by the operation.
        /// @param thumb_slot Binding or storage slot addressed by the operation.
        /// @param dragging_out `dragging_out` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void update_scrollbar_axis(Context &ctx, const UString &container_id, bool vertical,
                                          const ScrollbarStyle &style, ScrollAreaState::Axis &axis,
                                          f32 delta_seconds, bool enabled, const Context::ScrollMetrics &metrics,
                                          const PartSlot<ScrollAreaPartContext> &track_slot,
                                          const PartSlot<ScrollAreaPartContext> &thumb_slot, bool &dragging_out);

    } // namespace Detail


    /// Scrolls area using the supplied arguments and current state.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param id Identifier of the target object or resource.
    /// @param decl `decl` value used by the operation.
    /// @param style `style` value used by the operation.
    /// @param state `state` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    /// @param build_content `build_content` value used by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    /// @param composition `composition` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    ScrollAreaResult scroll_area(Context &ctx, const UString &id, const ElementDecl &decl,
                                        const ScrollbarStyle &style, ScrollAreaState &state, f32 delta_seconds,
                                        const ScrollAreaBuilder &build_content, bool enabled,
                                        const ScrollAreaComposition &composition);

    /// Scrolls area using the supplied arguments and current state.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param id Identifier of the target object or resource.
    /// @param decl `decl` value used by the operation.
    /// @param style `style` value used by the operation.
    /// @param state `state` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    /// @param build_content `build_content` value used by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    ScrollAreaResult scroll_area(Context &ctx, const UString &id, const ElementDecl &decl,
                                        const ScrollbarStyle &style, ScrollAreaState &state, f32 delta_seconds,
                                        const ScrollAreaBuilder &build_content, bool enabled = true);

} // namespace SFT::UI
