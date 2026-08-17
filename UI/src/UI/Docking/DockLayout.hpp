#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <vector>
#pragma endregion

#include "DockTypes.hpp"

using std::vector;

/// Pure, stateless dock-tree layout solve — no Context/Clay dependency, same "usable/testable
/// standalone" reasoning as DockTypes.hpp itself. DockWorkspace calls this once per frame and
/// immediately hit-tests its own (non-stale) result against this same frame's pointer position —
/// unlike Context::hovered()'s one-frame-stale contract, this is plain top-down arithmetic over
/// already-known geometry, so there's no staleness to reason about here.
namespace SFT::UI::Docking {

    struct DockNodeLayout {
        DockNodeId node{};
        DockRect full_rect;
        DockRect tab_strip_rect;
        DockRect content_rect;
        DockRect divider_rect;
    };

    namespace Detail {

        void solve_dock_layout(const DockTree &tree, DockNodeId id, DockRect rect, f32 tab_strip_height,
                                       f32 divider_thickness, vector<DockNodeLayout> &out);

    } // namespace Detail

    [[nodiscard]] vector<DockNodeLayout> compute_dock_layout(const DockTree &tree, DockRect workspace_rect,
                                                                     f32 tab_strip_height, f32 divider_thickness);

} // namespace SFT::UI::Docking
