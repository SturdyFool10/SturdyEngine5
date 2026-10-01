#pragma once

#include <Foundation/Foundation.hpp>

#include <functional>
#include <optional>
#include <span>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <RHI/RHI.hpp>
#include <Renderer/Culling.hpp>
#include <Renderer/DisplacementMeshPath.hpp>
#include <Renderer/Material.hpp>
#include <Renderer/Mesh.hpp>

namespace SFT::Renderer {

    /// One mesh drawn with one material at one transform: the unit `Renderer::record_draw_items` draws. The engine's
    /// own scene draws are lists of these; a feature can build its own from any mesh/material handles.
    struct RenderItem {
        MeshHandle mesh{};
        MaterialInstanceHandle material{};
        glm::mat4 world_transform{1.0f};
        glm::mat4 previous_world_transform{1.0f};
        u64 stable_id = 0;
        u32 sort_key = 0;
        bool casts_shadows = true;
        RHI::CullMode cull_mode = RHI::CullMode::Back;
        RHI::FrontFace front_face = RHI::FrontFace::CounterClockwise;
        glm::vec3 world_bounds_center{0.0f};
        f32 world_bounds_radius = 0.0f;


        u32 object_index = 0;

        // Per-frame data for the mesh-shader displacement path (RendererDisplacementMesh.cpp); null unless
        // the frame has draws whose material template enabled that path.
        const DisplacementMeshFrame *displacement_frame = nullptr;
    };

    /// How `Renderer::record_draw_items` draws a list of `RenderItem`s into the current render pass. This is the
    /// engine's own item-drawing path (mesh arena, material binding, pipeline selection, frustum culling, parallel
    /// render-bundle recording for large lists), so a feature that draws scene geometry uses the same code the
    /// built-in depth prepass and gizmo passes do.
    struct ItemCuller;

    /// Decides whether a `RenderItem` is drawn in a view. The default is a bounding-sphere test against the view
    /// frustum; a `SpaceModel` replaces it (portal traversal, hyperbolic distance, voxel chunk visibility, ...).
    using ItemVisibilityFn = std::function<bool(const RenderItem &, const ItemCuller &)>;

    /// One view's culling inputs plus the visibility rule to apply. Built once per view; cheap to copy.
    struct ItemCuller {
        Frustum frustum{};
        glm::mat4 view_projection{1.0f};
        glm::vec3 camera_position{0.0f};
        /// True when culling for a light's shadow view rather than the camera: `view_projection` and `frustum` are the
        /// light's and `camera_position` is unset.
        bool shadow_view = false;
        /// Null (or empty) means the default frustum test.
        const ItemVisibilityFn *custom = nullptr;

        [[nodiscard]] bool operator()(const RenderItem &item) const {
            if (custom != nullptr && *custom) {
                return (*custom)(item, *this);
            }
            return frustum_intersects_sphere(frustum, item.world_bounds_center, item.world_bounds_radius);
        }
        [[nodiscard]] static ItemCuller from_view_projection(const glm::mat4 &view_projection,
                                                             const glm::vec3 &camera_position = glm::vec3{0.0f},
                                                             const ItemVisibilityFn *custom = nullptr) {
            return ItemCuller{.frustum = frustum_from_view_projection(view_projection),
                              .view_projection = view_projection,
                              .camera_position = camera_position,
                              .custom = custom};
        }
    };

    /// Which of the frame's item lists to draw. (Arbitrary caller-supplied items are a later addition: the item
    /// type still lives inside the renderer.)
    enum class DrawItemSet : u8 {
        /// The scene's opaque draws.
        SceneDraws,
        /// Light indicators and other gizmos.
        Gizmos,
        /// The caller's own list, `DrawItemPass::items`.
        Custom,
    };

    struct DrawItemPass {
        DrawItemSet set = DrawItemSet::SceneDraws;
        /// The items to draw when `set` is `Custom`; must outlive the frame's graph execution.
        std::span<const RenderItem> items;
        /// Attachment formats the pass was declared with (empty for a depth-only pass).
        std::span<const RHI::Format> color_formats;
        RHI::Format depth_format = RHI::Format::Undefined;
        RHI::SampleCount samples = RHI::SampleCount::X1;
        u64 frame_index = 0;
        glm::mat4 view_projection{1.0f};
        /// Strength of the camera lens (vertex-warp fisheye) for a pass drawn from the camera's own view: the
        /// engine's `CameraEmulationSettings::lens_strength`. 0 for anything else, notably shadow views.
        f32 camera_lens = 0.0f;
        /// Draw only depth (materials pick their depth-only pipeline).
        bool depth_only = false;
        /// Plain LessEqual depth test instead of the prepass-matched "equal" test.
        bool standard_depth_test = false;
        /// Skip items the culler rejects.
        bool frustum_cull = true;
        /// The culler to use; null builds the default frustum culler from `view_projection`.
        const ItemCuller *culler = nullptr;
        /// Record large lists in parallel render bundles (only used if the device supports them).
        bool allow_bundles = true;
        /// Debug label for bundles.
        const char *label = "draw items";
        RHI::Viewport viewport{};
        RHI::Rect2D scissor{};
    };

} // namespace SFT::Renderer
