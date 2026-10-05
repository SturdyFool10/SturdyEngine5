#pragma once

#include <Foundation/Foundation.hpp>

#include <span>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <RenderSettings/RenderSettings.hpp>

/// Clustered light binning: the view frustum is cut into screen tiles x exponential depth slices ("clusters") and every
/// local light is listed in each cluster its bounding sphere touches, so a pixel (or a fog froxel) only evaluates the lights
/// that can reach it. Done on the CPU each frame: deterministic, no atomics (so it runs the same on every backend), and cheap
/// for the thousand-or-so lights a frame uploads (~0.1-0.3 ms).
namespace SFT::Renderer {

    /// What the binner needs to know about one light.
    struct ClusterLight {
        glm::vec3 position{0.0f};
        f32 range = 1.0f;
        /// Spot cone axis and the cosine of its half-angle; `cone_cos` <= -1 marks a point light (a full sphere).
        glm::vec3 direction{0.0f, -1.0f, 0.0f};
        f32 cone_cos = -2.0f;
    };

    struct LightClusterGrid {
        u32 tiles_x = 0;
        u32 tiles_y = 0;
        u32 slices = 0;
        u32 tile_px = 64;
        f32 near_plane = 0.1f;
        f32 far_plane = 300.0f;
        /// slice = floor(log(view_depth) * slice_scale + slice_bias)
        f32 slice_scale = 0.0f;
        f32 slice_bias = 0.0f;
        [[nodiscard]] u32 cluster_count() const noexcept { return tiles_x * tiles_y * slices; }
        [[nodiscard]] u32 cluster_index(u32 x, u32 y, u32 slice) const noexcept { return (slice * tiles_y + y) * tiles_x + x; }
        /// The slice a view-space distance falls in (clamped to the grid).
        [[nodiscard]] u32 slice_of(f32 view_depth) const noexcept;
        /// View-space distance where `slice` begins.
        [[nodiscard]] f32 slice_near(u32 slice) const noexcept;
    };

    struct LightClusters {
        LightClusterGrid grid{};
        /// Per cluster: first index into `indices` and how many lights it lists.
        std::vector<glm::uvec2> ranges;
        std::vector<u32> indices;
        /// Light-list entries dropped because a cluster was already full (`max_lights_per_cluster`).
        u32 dropped = 0;
    };

    /// Bins `lights` (indices into that span) for a perspective camera. `view` is world -> view (camera looks down -Z),
    /// `projection` view -> clip; `render_width`/`render_height` the lit image's size in pixels. An orthographic or
    /// degenerate projection yields an empty grid (callers fall back to the unclustered path).
    [[nodiscard]] LightClusters build_light_clusters(std::span<const ClusterLight> lights, const glm::mat4 &view, const glm::mat4 &projection,
                                                     u32 render_width, u32 render_height, f32 near_plane,
                                                     const RenderSettings::LightingSettings &settings);

    /// The sphere that bounds a light's lit volume (a spot's cone, a point's sphere): centre xyz, radius w.
    [[nodiscard]] glm::vec4 light_bounding_sphere(const ClusterLight &light) noexcept;

} // namespace SFT::Renderer
