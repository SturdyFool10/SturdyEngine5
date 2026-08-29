#include <Renderer/UI/FrameGraph.hpp>

#pragma region Imports
#include <algorithm>
#pragma endregion

namespace SFT::UI {

    /// Appends one frame's sample to `state`'s rolling history for `FrameGraphState` using the
    /// supplied arguments.
    ///
    /// @param state `state` value used by the operation.
    /// @param desc Description of the resource or operation to perform.
    /// @param segment_values One value per `desc.segments` entry, in that order.
    ///
    /// @note This function does not throw exceptions.
    void frame_graph_push(FrameGraphState &state, const FrameGraphDesc &desc, span<const f64> segment_values) {
        vector<f64> frame(desc.segments.size(), 0.0);
        for (usize i = 0; i < frame.size() && i < segment_values.size(); ++i) {
            frame[i] = segment_values[i];
        }
        state.history.push_back(std::move(frame));
        while (state.history.size() > desc.window_size) {
            state.history.pop_front();
        }
    }

    /// Draws a frame graph widget for `UI` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param decl `decl` value used by the operation.
    /// @param desc Description of the resource or operation to perform.
    /// @param state `state` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    GraphResult frame_graph(Context &ctx, const ElementDecl &decl, const FrameGraphDesc &desc,
                            const FrameGraphState &state) {
        const usize segment_count = desc.segments.size();
        const usize frame_count = state.history.size();

        // Transpose the per-frame history (one vector per frame) into one contiguous array per
        // segment — that's the shape GraphType::Bar's series already expect (one value per category,
        // where each category here is a frame column).
        vector<vector<f64>> per_segment(segment_count, vector<f64>(frame_count, 0.0));
        usize frame_index = 0;
        for (const vector<f64> &frame : state.history) {
            for (usize s = 0; s < segment_count && s < frame.size(); ++s) {
                per_segment[s][frame_index] = frame[s];
            }
            ++frame_index;
        }

        GraphDesc graph_desc{};
        graph_desc.type = GraphType::Bar;
        graph_desc.bar_stack_mode = BarStackMode::Stacked;
        graph_desc.bar_group_gap_fraction = desc.bar_group_gap_fraction;
        graph_desc.x_axis.is_categorical = true;
        // One gridline/label per frame would be far too dense for anything but a tiny window.
        graph_desc.x_axis.show_gridlines = false;
        graph_desc.y_axis = desc.y_axis;
        graph_desc.background_color = desc.background_color;
        graph_desc.corner_radius = desc.corner_radius;
        graph_desc.axis_margin_left = desc.axis_margin_left;
        graph_desc.axis_margin_bottom = desc.axis_margin_bottom;
        graph_desc.axis_margin_top = desc.axis_margin_top;
        graph_desc.axis_margin_right = desc.axis_margin_right;
        graph_desc.font_id = desc.font_id;
        graph_desc.label_font_size = desc.label_font_size;
        graph_desc.show_legend = desc.show_legend;

        graph_desc.series.reserve(segment_count);
        for (usize s = 0; s < segment_count; ++s) {
            graph_desc.series.push_back(SeriesRef{
                .name = desc.segments[s].name,
                .y = per_segment[s],
                .color = desc.segments[s].color,
            });
        }

        GraphState scratch_state{};
        return graph(ctx, decl, graph_desc, scratch_state);
    }

} // namespace SFT::UI
