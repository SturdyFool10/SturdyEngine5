#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <functional>
#include <span>
#include <vector>
#pragma endregion

#include "Context.hpp"
#include "Style.hpp"

using std::span;
using std::vector;

// CSS-masonry-style tiling container (Pinterest-style: items of varying size packed into tracks,
// each item going wherever there's currently the least fill, not a uniform grid cell) — built
// entirely on Context's own public API, the same "not a Clay primitive, so it isn't a Context
// method" reasoning as Button.hpp.
namespace SFT::UI {

    enum class MasonryDirection : u8 {
        Vertical,   // N columns, each stacking items downward — an item goes to whichever column
                    // has the smallest accumulated height so far.
        Horizontal, // N rows, each stacking items rightward — an item goes to whichever row has
                    // the smallest accumulated width so far.
    };

    struct MasonryConfig {
        MasonryDirection direction = MasonryDirection::Vertical;
        u32 track_count = 2;
        u16 track_gap = 10; // gap between tracks, perpendicular to the packing axis
        u16 item_gap = 10;  // gap between items within one track, along the packing axis
    };

    // One item to place. `extent` is its size along the packing axis (height for Vertical tracks,
    // width for Horizontal tracks) — masonry_layout() needs this up front to decide which track the
    // item goes into (the standard "next available space" masonry algorithm), before any Clay
    // element for it exists; there's no way to measure a not-yet-built element's size mid-layout.
    // `build` is called once content is safe to declare, with the matching track's element() scope
    // already open — call Context::element()/text()/image()/... inside it exactly as inside any
    // other container. Runs in track-grouped order, not necessarily `items`' original order (see
    // masonry_layout()'s own doc comment for why).
    struct MasonryItem {
        f32 extent = 0.0f;
        std::function<void(Context &)> build;
    };

    // Places `items` into `config.track_count` tracks and builds the whole thing: an outer element
    // (from `outer_decl`, with `.direction`/`.child_gap` overridden from `config` — everything else
    // on `outer_decl`, sizing/padding/background/..., passes through untouched, the same "style
    // fields get overridden, everything else is yours" convention UI::button() uses) containing one
    // element per track, each containing its assigned items in original relative order.
    //
    // Two-pass by necessity, not choice: Clay's element tree is a strict open/close stack (see
    // Context::element()'s own ElementScope) — a track can't be reopened once a later track (or a
    // later item) has been opened after it, so every item's track has to be decided (pure CPU, no
    // Clay calls) *before* any track opens. That's also why an item's content is a callback rather
    // than an inline scope the caller fills imperatively: which track an item lands in isn't known
    // until every earlier item's extent has been weighed too.
    //
    // Placement is O(items * track_count) — a linear scan per item for the least-filled track, fine
    // at the track counts a layout actually uses (a handful, not hundreds). This does *not* cull
    // offscreen items: Clay lays out everything it's given every frame (plans/clay-ui-renderer.md),
    // so a masonry grid with thousands of items needs the caller to only pass the visible-ish subset
    // into `items` if that matters — the same caveat Engine::UiImageCache's own doc comment flags
    // for a different reason (large-tree cost has to be designed out per-component, not assumed
    // away).
    inline void masonry_layout(Context &ctx, const ElementDecl &outer_decl, const MasonryConfig &config,
                               span<const MasonryItem> items) {
        const u32 track_count = std::max<u32>(config.track_count, 1);
        // Bucket item indices by track, preserving each track's items in original relative order.
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
