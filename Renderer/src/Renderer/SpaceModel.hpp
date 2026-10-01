#pragma once

#include <Foundation/Foundation.hpp>

#include <functional>
#include <optional>
#include <string>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <Renderer/Scene.hpp>

#include <Renderer/DrawItems.hpp>

namespace SFT::Renderer {

    /// How the renderer sees space, as data. The default is the ordinary Euclidean space every built-in pass
    /// assumes; a game with another geometry supplies its own and the renderer follows:
    ///
    /// * **Vertex transform.** `shader_source` replaces the `sturdy_space` Slang module (see
    ///   `Shaders/sturdy_space.slang`): the G-buffer, object-history, instanced, colour and displaced geometry
    ///   shaders call `sturdySpaceModelToWorld` / `sturdySpaceWorldToClip` / `sturdySpaceTransformDirection` and
    ///   nothing else, so replacing them bends every built-in geometry pass.
    /// * **Visibility.** `item_visible` replaces the frustum test used by the depth prepass, the G-buffer pass and
    ///   `Renderer::record_draw_items` (an ItemCuller is handed to it with the view's matrices and camera). With a
    ///   custom rule the GPU instance-culling path (which assumes frusta) is bypassed unless `gpu_culling` says the
    ///   rule is compatible with it.
    ///
    /// A replacement module must define every function of the shipped one (`sturdySpaceModelToWorld`,
    /// `sturdySpaceWorldToClip`, `sturdySpaceTransformDirection`, `sturdySpaceClipToWorldH`,
    /// `sturdySpaceClipToWorld`): start from a copy of `Shaders/sturdy_space.slang`. The view and projection
    /// matrices a frame carries are opaque payload to the module -- it receives them as `viewProjection` /
    /// `inverseViewProjection` and may interpret them however its geometry needs (pack a Lorentz transform, a
    /// portal chain, ...); the renderer only derives the default frustum from them, which a custom `item_visible`
    /// replaces.
    ///
    /// Deferred lighting, ReSTIR GI and the spectral integrators reconstruct world position through
    /// `sturdySpaceClipToWorld`, and lighting projects into light space through `sturdySpaceWorldToClip`, so they
    /// follow the module; shadow casters' vertex stages do too. The cascade/cone/cube *fitting* is Euclidean:
    /// `shadow_view_matrix` replaces the fitted matrices.
    /// The matrices a frame's shaders receive as `viewProjection` / `inverseViewProjection`. Opaque to the renderer:
    /// a module (`sturdy_space`) interprets them, so they can carry anything a 4x4 can hold (a Lorentz transform,
    /// a portal chain, a conformal map). Computed once per frame from the camera and shared by every pass.
    struct CameraPayload {
        glm::mat4 view_projection{1.0f};
        glm::mat4 inverse_view_projection{1.0f};
    };
    using CameraPayloadFn = std::function<CameraPayload(const CameraView &)>;

    enum class ShadowViewKind : u8 {
        DirectionalCascade,
        Spot,
        PointFace,
    };

    /// What a `SpaceModel::shadow_view_matrix` hook is told about one shadow view the renderer is about to render.
    struct ShadowViewRequest {
        ShadowViewKind kind = ShadowViewKind::DirectionalCascade;
        /// Cascade index (directional), light index in importance order (spot/point).
        u32 index = 0;
        /// Cube face 0..5 for `PointFace`, else 0.
        u32 face = 0;
        /// The Euclidean fit the renderer computed (orthographic cascade / perspective cone / cube face).
        glm::mat4 default_view_projection{1.0f};
        glm::vec3 light_position{0.0f};
        glm::vec3 light_direction{0.0f, -1.0f, 0.0f};
        f32 near_plane = 0.0f;
        f32 far_plane = 0.0f;
        CameraView camera{};
    };
    /// Returns the matrix to render and sample that shadow view with, or nothing to keep the default.
    using ShadowViewMatrixFn = std::function<std::optional<glm::mat4>(const ShadowViewRequest &)>;

    struct SpaceModel {
        std::string name = "euclidean";
        /// Replacement source for the `sturdy_space` module; empty keeps the shipped Euclidean one.
        std::optional<std::string> shader_source;
        /// Replacement visibility rule; empty keeps the frustum sphere test.
        ItemVisibilityFn item_visible;
        /// Replaces how the frame's view-projection matrix (and its inverse) is built; empty is `projection * view`
        /// and its `glm::inverse`. Use it when the payload is not invertible in the ordinary sense.
        CameraPayloadFn camera_payload;
        /// Replaces the shadow view matrices the renderer fits (orthographic cascades, spot cones, cube faces). The
        /// returned matrix is what the shadow pass renders with *and* what the lighting pass projects with (through
        /// `sturdySpaceWorldToClip`), so the two stay consistent. A replaced cascade drops the renderer's caster
        /// list (it was computed for the Euclidean fit) and falls back to `item_visible` / the matrix's frustum.
        ShadowViewMatrixFn shadow_view_matrix;
        /// Keep the GPU instance-culling path enabled. Only true for rules the frustum-based GPU culler agrees
        /// with; forced off whenever `item_visible` is set.
        bool gpu_culling = true;
    };

} // namespace SFT::Renderer
