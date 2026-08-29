#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <deque>
#include <span>
#include <string>
#include <vector>
#pragma endregion

#include <Renderer/UI/Graph.hpp>

using std::deque;
using std::span;
using std::string;
using std::vector;

// A purpose-built convenience widget for profiler-style visualization — "each frame is a column of
// several labeled, stacked subunits" (e.g. CPU/GPU stage timings). Implemented as a thin wrapper
// that builds a `GraphDesc{.type = GraphType::Bar, .bar_stack_mode = BarStackMode::Stacked}` and
// calls `UI::graph()`, so it reuses all of the stroke/fill primitives and axis machinery rather than
// being a separate render path — see this feature's own implementation plan for the full reasoning.
namespace SFT::UI {

    struct FrameGraphSegmentDef {
        string name;
        Color color;
    };

    struct FrameGraphDesc {
        /// The labeled subunits, stacked bottom-to-top in this order for every frame column (e.g.
        /// "CPU: scene prep", "GPU: shadow pass", ...).
        vector<FrameGraphSegmentDef> segments;

        /// How many trailing frames `frame_graph_push()` keeps before evicting the oldest.
        usize window_size = 120;

        AxisConfig y_axis{};
        Color background_color{0.07, 0.08, 0.1, 1.0};
        CornerRadius corner_radius{.top_left = 6.0f, .top_right = 6.0f, .bottom_left = 6.0f, .bottom_right = 6.0f};

        f32 axis_margin_left = 48.0f;
        f32 axis_margin_bottom = 10.0f; // frame columns are unlabeled by default; a dense frame graph
        f32 axis_margin_top = 10.0f;    // doesn't have room for one tick per frame
        f32 axis_margin_right = 10.0f;

        /// Forwarded to the underlying GraphDesc — see its own doc comment.
        FontId font_id = 0;
        u16 label_font_size = 11;
        bool show_legend = true;

        /// Fraction of a frame-column's slot left empty as a gap — frame graphs are typically much
        /// denser than an ordinary bar chart, so this defaults narrower than GraphDesc's own.
        f32 bar_group_gap_fraction = 0.15f;
    };

    /// The rolling history `frame_graph()` reads from. Owned by the caller (e.g. one per dock panel/
    /// surface), pushed to once per frame via `frame_graph_push()`.
    struct FrameGraphState {
        /// One entry per pushed frame, oldest first; each inner vector holds one value per
        /// `FrameGraphDesc::segments` entry, in that same order.
        deque<vector<f64>> history;
    };

    /// Appends one frame's sample to `state`'s rolling history, evicting the oldest frame once
    /// `desc.window_size` is exceeded.
    ///
    /// @param state `state` value used by the operation.
    /// @param desc Description of the resource or operation to perform.
    /// @param segment_values One value per `desc.segments` entry, in that order. A short array is
    ///        zero-padded for the missing trailing segments; a long one is truncated.
    ///
    /// @note This function does not throw exceptions.
    void frame_graph_push(FrameGraphState &state, const FrameGraphDesc &desc, span<const f64> segment_values);

    /// Draws a frame graph widget as a leaf element sized/positioned by `decl`, same as graph().
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param decl `decl` value used by the operation.
    /// @param desc Description of the resource or operation to perform.
    /// @param state `state` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] GraphResult frame_graph(Context &ctx, const ElementDecl &decl, const FrameGraphDesc &desc,
                                          const FrameGraphState &state);

} // namespace SFT::UI
