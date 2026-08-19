#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <vector>
#pragma endregion

#include <Renderer/UI/Docking/DockTypes.hpp>

using std::vector;


namespace SFT::UI::Docking {

    struct DockNodeLayout {
        DockNodeId node{};
        DockRect full_rect;
        DockRect tab_strip_rect;
        DockRect content_rect;
        DockRect divider_rect;
    };

    namespace Detail {

        /// Performs the solve dock layout operation using the supplied arguments.
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
                                       f32 divider_thickness, vector<DockNodeLayout> &out);

    } // namespace Detail

    /// Computes dock layout using the supplied arguments and current state.
    ///
    /// @param tree `tree` value used by the operation.
    /// @param workspace_rect `workspace_rect` value used by the operation.
    /// @param tab_strip_height `tab_strip_height` value used by the operation.
    /// @param divider_thickness `divider_thickness` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] vector<DockNodeLayout> compute_dock_layout(const DockTree &tree, DockRect workspace_rect,
                                                                     f32 tab_strip_height, f32 divider_thickness);

} // namespace SFT::UI::Docking
