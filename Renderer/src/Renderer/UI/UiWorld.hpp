#pragma once

#include <Foundation/Foundation.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <optional>

#include <Renderer/UI/UiInput.hpp>

namespace SFT::UI {

    /// A UI shown on a surface in the world: render the UI into an off-screen target, put that texture on a quad
    /// (or anything with UVs), and turn "where is the user pointing" into a pointer position on the UI.
    ///
    /// The engine does not decide how a user points at a world UI (a mouse ray from the camera, a VR controller, a
    /// gaze, a gamepad cursor, a physical button on a prop). It only asks that you produce a *ray* or a *uv*; these
    /// helpers turn either into the pixel position `UiInput` wants. Everything after that (hover, press, drag,
    /// scroll, text) is the same UI code as on screen.
    ///
    /// ```cpp
    /// const UI::UiWorldPlane panel{.origin = top_left, .u_axis = right * width, .v_axis = down * height};
    /// if (auto uv = UI::ui_uv_at_ray(panel, camera_ray_origin, camera_ray_direction)) {
    ///     ui.input().move_pointer(UI::ui_pixel_from_uv(*uv, ui_extent));
    ///     ui.input().set_pointer_down(trigger_pressed);
    /// } else {
    ///     ui.input().cancel_pointer();   // not pointing at the panel
    /// }
    /// ```
    struct UiWorldPlane {
        /// World position of the UI's top-left corner.
        glm::vec3 origin{0.0f};
        /// World vector from the top-left to the top-right corner (its length is the panel's width).
        glm::vec3 u_axis{1.0f, 0.0f, 0.0f};
        /// World vector from the top-left to the bottom-left corner (its length is the panel's height).
        glm::vec3 v_axis{0.0f, -1.0f, 0.0f};
    };

    /// Where a ray meets the plane, as (u, v) in [0, 1] across the panel (0,0 = top-left, 1,1 = bottom-right).
    /// Returns nothing when the ray is parallel to the panel, points away from it, or - unless
    /// `allow_outside` - lands outside the panel's edges. `direction` need not be normalised.
    [[nodiscard]] std::optional<glm::vec2> ui_uv_at_ray(const UiWorldPlane &plane, glm::vec3 ray_origin, glm::vec3 ray_direction,
                                                        bool allow_outside = false) noexcept;

    /// UI pixel position for a (u, v) coordinate, given the extent the UI is laid out at.
    [[nodiscard]] glm::vec2 ui_pixel_from_uv(glm::vec2 uv, glm::vec2 ui_extent) noexcept;

} // namespace SFT::UI
