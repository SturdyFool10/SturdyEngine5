#include <Renderer/UI/Masonry.hpp>


namespace SFT::UI {

    /// Performs the masonry layout operation for `UI` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param outer_decl `outer_decl` value used by the operation.
    /// @param config Configuration values controlling the operation.
    /// @param items `items` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void masonry_layout(Context &ctx, const ElementDecl &outer_decl, const MasonryConfig &config,
                               span<const MasonryItem> items) {
        const u32 track_count = std::max<u32>(config.track_count, 1);

        vector<f32> track_fill(track_count, 0.0f);
        vector<vector<usize>> track_items(track_count);
        for (usize i = 0; i < items.size(); ++i) {
            const auto least = std::min_element(track_fill.begin(), track_fill.end());
            const usize track = static_cast<usize>(least - track_fill.begin());
            track_items[track].push_back(i);
            track_fill[track] += items[i].extent + static_cast<f32>(config.item_gap);
        }

        const bool vertical = config.direction == MasonryDirection::Vertical;
        ElementDecl outer = outer_decl;
        outer.direction = vertical ? LayoutDirection::LeftToRight : LayoutDirection::TopToBottom;
        outer.child_gap = config.track_gap;
        auto outer_scope = ctx.element(outer);
        (void)outer_scope;

        for (u32 t = 0; t < track_count; ++t) {
            auto track_scope = ctx.element(ElementDecl{
                .sizing = vertical ? Sizing{SizingAxis::grow(), SizingAxis::fit()}
                                   : Sizing{SizingAxis::fit(), SizingAxis::grow()},
                .child_gap = config.item_gap,
                .direction = vertical ? LayoutDirection::TopToBottom : LayoutDirection::LeftToRight,
            });
            (void)track_scope;
            for (usize index : track_items[t]) {
                items[index].build(ctx);
            }
        }
    }

} // namespace SFT::UI

