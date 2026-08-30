#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <string>
#include <vector>
#pragma endregion

#include <Renderer/UI/Context.hpp>
#include <Renderer/UI/GraphAxis.hpp>

using std::span;
using std::string;
using std::vector;

namespace SFT::UI {

    // Scene3D lands in a later stage (see this feature's own implementation plan's staged build
    // order) — every other type here is implemented.
    enum class GraphType : u8 { Line, Area, Bar, Scatter, Pie };

    enum class BarStackMode : u8 { Grouped, Stacked };

    /// One named data series bound to a graph. `x`/`y` must be the same length; points are drawn in
    /// array order (not sorted) — a caller wanting a left-to-right line should supply `x` in ascending
    /// order itself. Both spans must stay valid for the duration of the graph() call (they are read
    /// synchronously, not retained).
    ///
    /// Only the fields relevant to `GraphDesc::type` are read: `line_width`/`feather_px` (Line/Area),
    /// `area_fill_opacity` (Area), `marker_radius` (Scatter). `GraphType::Bar` reads `y` (one value per
    /// `x_axis.categories` entry, in order) and ignores `x` entirely — the category axis supplies bar
    /// positions instead.
    struct SeriesRef {
        string name;
        span<const f64> x;
        span<const f64> y;
        Color color{0.4, 0.7, 1.0, 1.0};
        f32 line_width = 2.0f;
        f32 feather_px = 1.0f;

        /// `GraphType::Area` only: alpha multiplier for the filled region between the line and the
        /// data-space y=0 baseline (clamped to the plot edge if 0 is out of range, same as the axis
        /// zero-crossing lines). 0 disables the fill, leaving just the line.
        f32 area_fill_opacity = 0.35f;

        /// `GraphType::Scatter` only: marker radius in pixels. 0 falls back to a small default (3px)
        /// rather than drawing nothing, since picking Scatter is a clear signal the caller wants
        /// visible markers.
        f32 marker_radius = 0.0f;

        /// `GraphType::Line`/`GraphType::Area` only: forwarded to this series' line StrokePath as
        /// `StrokeStyle::glow_intensity` — `0` draws the line normally; a positive value routes it
        /// through the engine's real bloom pipeline instead (see StrokeStyle's own doc comment,
        /// UiStroke.hpp), so just this one series gets a genuine glow independent of the rest of the
        /// chart.
        f32 glow_intensity = 0.0f;
    };

    /// One wedge of a `GraphType::Pie` chart. Pie charts have no Cartesian axes at all, so they bind
    /// `GraphDesc::pie_slices` instead of `series`/`x_axis`/`y_axis`.
    struct PieSlice {
        string name;
        f64 value = 0.0;
        Color color{0.55, 0.6, 0.68, 1.0};
    };

    struct PieStyle {
        /// 0 draws a solid pie; a value in `(0, 1)` cuts a proportional hole in the middle for a donut.
        f32 hole_ratio = 0.0f;
        f32 start_angle_degrees = -90.0f; // 12 o'clock
        f32 gap_degrees = 0.0f;
        f32 feather_px = 1.0f;
    };

    struct GraphDesc {
        GraphType type = GraphType::Line;
        AxisConfig x_axis{};
        AxisConfig y_axis{};
        vector<SeriesRef> series;

        /// `GraphType::Bar` only. Grouped places each series' bar side-by-side within a category's
        /// slot; Stacked accumulates them on top of each other (and autoscales the value axis from
        /// each category's summed total, not each series independently).
        BarStackMode bar_stack_mode = BarStackMode::Grouped;
        f32 bar_group_gap_fraction = 0.2f;   // fraction of a category's slot width left empty between categories
        f32 bar_series_gap_fraction = 0.08f; // Grouped only: fraction of slot width between each series' bar

        vector<PieSlice> pie_slices;
        PieStyle pie_style{};

        Color background_color{0.07, 0.08, 0.1, 1.0};
        CornerRadius corner_radius{.top_left = 6.0f, .top_right = 6.0f, .bottom_left = 6.0f, .bottom_right = 6.0f};

        // Fixed margin, in pixels, reserved for axis labels/gridlines around the plot area. Wide
        // enough by default to fit tick-label text; a caller with unusually long labels may need to
        // grow axis_margin_left further. Pie charts ignore these (no axes to reserve space for).
        f32 axis_margin_left = 48.0f;
        f32 axis_margin_bottom = 24.0f;
        f32 axis_margin_top = 10.0f;
        f32 axis_margin_right = 10.0f;

        /// Font tick labels, axis titles, and the legend are drawn with. `0` is whatever font the
        /// caller registered under id 0 (Context::register_font) — same "id 0 is the default"
        /// convention the rest of this UI's text drawing uses.
        FontId font_id = 0;
        u16 label_font_size = 11;
        u16 title_font_size = 12;

        /// Draws a small swatch+name legend (from `series` for Line/Area/Bar/Scatter, from
        /// `pie_slices` for Pie), floating in the widget's top-right corner.
        bool show_legend = true;
    };

    struct GraphState {
        // Reserved for interaction state (hover/selection) added in a later stage; empty for now.
    };

    struct GraphResult {
        /// False on the first frame a graph() call appears at a given `decl.id` (Clay hasn't resolved
        /// its layout bounds yet, so there is nothing to draw), or if the resolved plot area collapsed
        /// to zero size. True every frame after that.
        bool drawn = false;
    };

    /// Draws a graph/plot widget (chart type selected by `desc.type`) as a leaf element sized/
    /// positioned by `decl`, same as image()/svg(). Bounds are read back via
    /// Context::element_bounds(decl.id), so — like every other bounds-dependent widget in this UI
    /// (Slider.cpp, ColorPicker.cpp, ...) — the plot content is one frame behind a layout change (e.g.
    /// the panel being resized), not the current frame's own just-computed size.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param decl `decl` value used by the operation.
    /// @param desc Description of the resource or operation to perform.
    /// @param state `state` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] GraphResult graph(Context &ctx, const ElementDecl &decl, const GraphDesc &desc, GraphState &state);

} // namespace SFT::UI
