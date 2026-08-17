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

/// A scroll container with an egui-style floating scrollbar: thin, invisible track, a translucent
/// thumb that fades in when the pointer enters the scrollable area (or while actively dragging/
/// scrolling) and fades back out shortly after — never a permanent fixture stealing layout space or
/// visual attention the way a classic OS scrollbar does. Built on Context::scroll_metrics()/
/// set_scroll_offset(), which read/write Clay's own live scroll-container state (clay.h's own doc
/// comment on Clay_ScrollContainerData::scrollPosition calls this exact use case out: "external
/// functionality that modifies scroll position, such as scroll bars").
namespace SFT::UI {

    enum class ScrollbarVisibility : u8 {
        WhenNeeded,
        AlwaysVisible,
        AlwaysHidden,
    };

    struct ScrollbarStyle {
        /// Cross-axis thickness at rest vs. while hovered/dragging — egui's own "expand on hover"
        /// feel: thin and unobtrusive until you're actually about to use it.
        f32 idle_thickness = 4.0f;
        f32 hovered_thickness = 8.0f;
        /// Fully transparent by default — an egui-style floating bar has no visible track, just a
        /// thumb, so it never reads as a permanent UI fixture the way an inset OS scrollbar does.
        Color track_color{0.0, 0.0, 0.0, 0.0};
        Color thumb_color{1.0, 1.0, 1.0, 0.28};
        Color thumb_hovered_color{1.0, 1.0, 1.0, 0.45};
        Color thumb_dragging_color{1.0, 1.0, 1.0, 0.65};
        f32 corner_radius = 3.0f;
        /// Inset from the viewport's own edges — keeps the bar from touching the container's own
        /// border/corner radius.
        f32 margin = 2.0f;
        f32 min_thumb_length = 24.0f;
        /// Snappy to show, lazy to hide — matches egui's own feel (appearing should never feel
        /// laggy; disappearing too quickly reads as flickery).
        f32 fade_in_seconds = 0.1f;
        f32 fade_out_seconds = 0.4f;
        /// How long the bar stays fully visible after the pointer leaves and scrolling stops before
        /// it starts fading out.
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

    [[nodiscard]] UString scroll_area_part_id(const UString &id, ScrollAreaVisualPart part);

    struct ScrollAreaPartContext {
        ScrollAreaVisualPart part = ScrollAreaVisualPart::Viewport;
        PartVisualState visual{};
        UString id;
        /// Where the thumb sits along the track, 0 (start) .. 1 (end).
        f64 value_fraction = 0.0;
        /// The thumb's length as a fraction of the track's own length (container/content ratio).
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

    /// Persistent per-scroll-area animation/gesture state — one instance per logical scroll_area()
    /// call, same convention as SliderState/ToggleState.
    class ScrollAreaState {
      public:
        /// Public so Detail::update_scrollbar_axis (a free function, not a member) can name it in its
        /// own signature — access to any actual *instance* of one still goes through
        /// DetailScrollAreaAccess below, same as SliderState/DetailSliderAccess (Slider.hpp).
        struct Axis {
            f32 opacity = 0.0f;
            f32 opacity_start = 0.0f;
            f32 opacity_target = 0.0f;
            f32 opacity_elapsed = 0.0f;
            bool opacity_initialized = false;
            /// Large so a scrollbar never shows itself before any real interaction has happened.
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
        static ScrollAreaState::Axis &vertical(ScrollAreaState &state) noexcept;
        static ScrollAreaState::Axis &horizontal(ScrollAreaState &state) noexcept;
    };

    struct ScrollAreaResult {
        bool vertical_dragging = false;
        bool horizontal_dragging = false;
    };

    using ScrollAreaBuilder = std::function<void(Context &)>;

    namespace Detail {

        /// One axis' worth of fade/hover/drag/render logic — horizontal and vertical are identical
        /// up to which fields of glm::vec2 they read, so this is instantiated twice (`vertical` toggles
        /// between them) rather than duplicating the whole body per axis.
        void update_scrollbar_axis(Context &ctx, const UString &container_id, bool vertical,
                                          const ScrollbarStyle &style, ScrollAreaState::Axis &axis,
                                          f32 delta_seconds, bool enabled, const Context::ScrollMetrics &metrics,
                                          const PartSlot<ScrollAreaPartContext> &track_slot,
                                          const PartSlot<ScrollAreaPartContext> &thumb_slot, bool &dragging_out);

    } // namespace Detail

    /// Opens a Clay scroll container (`decl.clip.horizontal`/`.vertical` select which axes scroll,
    /// same as a plain ctx.element() call), invokes `build_content` to declare its scrollable
    /// content, then draws a floating scrollbar for each active, actually-overflowing axis on top of
    /// it. `id` must be stable across frames — every part's own hit-test id is derived from it (same
    /// convention as ElementDecl::id elsewhere in this package).
    ScrollAreaResult scroll_area(Context &ctx, const UString &id, const ElementDecl &decl,
                                        const ScrollbarStyle &style, ScrollAreaState &state, f32 delta_seconds,
                                        const ScrollAreaBuilder &build_content, bool enabled,
                                        const ScrollAreaComposition &composition);

    ScrollAreaResult scroll_area(Context &ctx, const UString &id, const ElementDecl &decl,
                                        const ScrollbarStyle &style, ScrollAreaState &state, f32 delta_seconds,
                                        const ScrollAreaBuilder &build_content, bool enabled = true);

} // namespace SFT::UI
