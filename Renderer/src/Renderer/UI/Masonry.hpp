#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <functional>
#include <span>
#include <vector>
#pragma endregion

#include <Renderer/UI/Context.hpp>
#include <Renderer/UI/Style.hpp>

using std::span;
using std::vector;


namespace SFT::UI {

    enum class MasonryDirection : u8 {
        Vertical,

        Horizontal,

    };

    struct MasonryConfig {
        MasonryDirection direction = MasonryDirection::Vertical;
        u32 track_count = 2;
        u16 track_gap = 10;
        u16 item_gap = 10;
    };


    struct MasonryItem {
        f32 extent = 0.0f;
        std::function<void(Context &)> build;
    };


    /// Performs the masonry layout operation for `UI` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param outer_decl `outer_decl` value used by the operation.
    /// @param config Configuration values controlling the operation.
    /// @param items `items` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void masonry_layout(Context &ctx, const ElementDecl &outer_decl, const MasonryConfig &config,
                               span<const MasonryItem> items);

} // namespace SFT::UI
