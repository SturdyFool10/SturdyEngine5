#include <Renderer/UI/Docking/DockLayout.hpp>


namespace SFT::UI::Docking::Detail {

    /// Performs the solve dock layout operation for `Detail` using the supplied arguments.
    ///
    /// @param tree `tree` value used by the operation.
    /// @param id Identifier of the target object or resource.
    /// @param rect `rect` value used by the operation.
    /// @param tab_strip_height `tab_strip_height` value used by the operation.
    /// @param divider_thickness `divider_thickness` value used by the operation.
    /// @param out `out` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void solve_dock_layout(const DockTree &tree, DockNodeId id, DockRect rect, f32 tab_strip_height,
                                   f32 divider_thickness, vector<DockNodeLayout> &out) {
        const DockNode *n = tree.node(id);
        if (n == nullptr) {
            return;
        }

        if (n->kind == DockNode::Kind::Leaf) {
            const f32 strip_h = std::min(tab_strip_height, rect.size.y);
            DockNodeLayout layout{};
            layout.node = id;
            layout.full_rect = rect;
            layout.tab_strip_rect = DockRect{rect.origin, glm::vec2{rect.size.x, strip_h}};
            layout.content_rect = DockRect{glm::vec2{rect.origin.x, rect.origin.y + strip_h},
                                            glm::vec2{rect.size.x, rect.size.y - strip_h}};
            out.push_back(layout);
            return;
        }

        const bool horizontal = n->split_axis == DockSplitAxis::Horizontal;
        const f32 extent = horizontal ? rect.size.x : rect.size.y;
        const f32 half_divider = divider_thickness * 0.5f;
        const f32 first_extent = std::max(extent * n->split_ratio - half_divider, 0.0f);
        const f32 second_extent = std::max(extent - first_extent - divider_thickness, 0.0f);

        DockRect first_rect = rect;
        DockRect divider_rect = rect;
        DockRect second_rect = rect;
        if (horizontal) {
            first_rect.size.x = first_extent;
            divider_rect.origin.x = rect.origin.x + first_extent;
            divider_rect.size.x = divider_thickness;
            second_rect.origin.x = divider_rect.origin.x + divider_thickness;
            second_rect.size.x = second_extent;
        } else {
            first_rect.size.y = first_extent;
            divider_rect.origin.y = rect.origin.y + first_extent;
            divider_rect.size.y = divider_thickness;
            second_rect.origin.y = divider_rect.origin.y + divider_thickness;
            second_rect.size.y = second_extent;
        }

        DockNodeLayout layout{};
        layout.node = id;
        layout.full_rect = rect;
        layout.divider_rect = divider_rect;
        out.push_back(layout);

        solve_dock_layout(tree, n->first_child, first_rect, tab_strip_height, divider_thickness, out);
        solve_dock_layout(tree, n->second_child, second_rect, tab_strip_height, divider_thickness, out);
    }

} // namespace SFT::UI::Docking::Detail

namespace SFT::UI::Docking {

    /// Computes dock layout using the supplied arguments and current state.
    ///
    /// @param tree `tree` value used by the operation.
    /// @param workspace_rect `workspace_rect` value used by the operation.
    /// @param tab_strip_height `tab_strip_height` value used by the operation.
    /// @param divider_thickness `divider_thickness` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    vector<DockNodeLayout> compute_dock_layout(const DockTree &tree, DockRect workspace_rect,
                                                                     f32 tab_strip_height, f32 divider_thickness) {
        vector<DockNodeLayout> out;
        Detail::solve_dock_layout(tree, tree.root(), workspace_rect, tab_strip_height, divider_thickness, out);
        return out;
    }

} // namespace SFT::UI::Docking

