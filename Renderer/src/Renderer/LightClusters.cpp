#include <Renderer/LightClusters.hpp>

#include <algorithm>
#include <cmath>

namespace SFT::Renderer {

    u32 LightClusterGrid::slice_of(f32 view_depth) const noexcept {
        if (slices == 0) return 0;
        const f32 slice = std::floor(std::log(std::max(view_depth, near_plane)) * slice_scale + slice_bias);
        return static_cast<u32>(std::clamp(slice, 0.0f, static_cast<f32>(slices - 1)));
    }

    f32 LightClusterGrid::slice_near(u32 slice) const noexcept {
        if (slices == 0) return near_plane;
        return near_plane * std::pow(far_plane / near_plane, static_cast<f32>(slice) / static_cast<f32>(slices));
    }

    glm::vec4 light_bounding_sphere(const ClusterLight &light) noexcept {
        const f32 range = std::max(light.range, 1e-4f);
        if (light.cone_cos <= -1.0f) {
            return glm::vec4{light.position, range};
        }
        const f32 len = glm::length(light.direction);
        const glm::vec3 axis = len > 1e-6f ? light.direction / len : glm::vec3{0.0f, -1.0f, 0.0f};
        const f32 cos_half = std::clamp(light.cone_cos, -1.0f, 1.0f);
        const f32 sin_half = std::sqrt(std::max(0.0f, 1.0f - cos_half * cos_half));
        // Tightest sphere around a cone (apex + spherical cap): narrow cones are bounded through their far rim, wide ones
        // by the circle where the cap meets the side.
        if (cos_half >= std::sqrt(0.5f)) {
            const f32 radius = range / (2.0f * cos_half);
            return glm::vec4{light.position + axis * radius, radius};
        }
        if (cos_half <= 0.0f) {
            return glm::vec4{light.position, range}; // wider than a hemisphere: the whole sphere
        }
        return glm::vec4{light.position + axis * (range * cos_half), range * sin_half};
    }

    LightClusters build_light_clusters(std::span<const ClusterLight> lights, const glm::mat4 &view, const glm::mat4 &projection,
                                       u32 render_width, u32 render_height, f32 near_plane, const RenderSettings::LightingSettings &settings) {
        LightClusters out;
        const bool perspective = std::abs(projection[3][3]) < 0.5f && std::abs(projection[2][3]) > 0.5f;
        if (!perspective || render_width == 0 || render_height == 0) {
            return out;
        }
        LightClusterGrid &grid = out.grid;
        grid.tile_px = std::clamp(settings.cluster_tile_px, 8u, 512u);
        grid.tiles_x = (render_width + grid.tile_px - 1) / grid.tile_px;
        grid.tiles_y = (render_height + grid.tile_px - 1) / grid.tile_px;
        grid.slices = std::clamp(settings.cluster_depth_slices, 1u, 128u);
        grid.near_plane = std::max(near_plane, 1e-3f);
        grid.far_plane = std::max(std::isfinite(settings.cluster_max_distance) ? settings.cluster_max_distance : 300.0f, grid.near_plane * 2.0f);
        const f32 log_ratio = std::log(grid.far_plane / grid.near_plane);
        grid.slice_scale = static_cast<f32>(grid.slices) / log_ratio;
        grid.slice_bias = -static_cast<f32>(grid.slices) * std::log(grid.near_plane) / log_ratio;

        const u32 cluster_count = grid.cluster_count();
        const u32 max_per_cluster = std::clamp(settings.max_lights_per_cluster, 1u, 1024u);
        std::vector<u32> counts(cluster_count, 0);
        struct Footprint {
            u32 x0, x1, y0, y1, s0, s1;
        };
        std::vector<Footprint> footprints(lights.size(), Footprint{1, 0, 1, 0, 1, 0});

        const auto project = [&](const glm::vec3 &p) {
            const glm::vec4 clip = projection * glm::vec4{p, 1.0f};
            return glm::vec2{clip.x, clip.y} / std::max(clip.w, 1e-6f);
        };
        for (usize i = 0; i < lights.size(); ++i) {
            const glm::vec4 sphere = light_bounding_sphere(lights[i]);
            const glm::vec3 center = glm::vec3{view * glm::vec4{glm::vec3{sphere}, 1.0f}};
            const f32 radius = sphere.w;
            const f32 depth = -center.z;
            const f32 depth_min = depth - radius, depth_max = depth + radius;
            if (depth_max < grid.near_plane || depth_min > grid.far_plane) {
                continue;
            }
            Footprint fp{};
            fp.s0 = grid.slice_of(std::max(depth_min, grid.near_plane));
            fp.s1 = grid.slice_of(std::min(depth_max, grid.far_plane));
            if (depth_min <= grid.near_plane) {
                // The sphere reaches the camera: its screen extent is unbounded.
                fp.x0 = 0;
                fp.y0 = 0;
                fp.x1 = grid.tiles_x - 1;
                fp.y1 = grid.tiles_y - 1;
            } else {
                // Conservative: the box around the sphere projected at its nearest and farthest depth.
                glm::vec2 lo{1e30f}, hi{-1e30f};
                for (const f32 z : {depth_min, depth_max}) {
                    for (const f32 dx : {-radius, radius}) {
                        for (const f32 dy : {-radius, radius}) {
                            const glm::vec2 ndc = project(glm::vec3{center.x + dx, center.y + dy, -z});
                            lo = glm::min(lo, ndc);
                            hi = glm::max(hi, ndc);
                        }
                    }
                }
                if (hi.x < -1.0f || lo.x > 1.0f || hi.y < -1.0f || lo.y > 1.0f) {
                    continue;
                }
                // NDC y points up; pixel rows run down.
                const auto to_tile_x = [&](f32 ndc) {
                    const f32 px = (ndc * 0.5f + 0.5f) * static_cast<f32>(render_width);
                    return static_cast<u32>(std::clamp(std::floor(px / static_cast<f32>(grid.tile_px)), 0.0f, static_cast<f32>(grid.tiles_x - 1)));
                };
                const auto to_tile_y = [&](f32 ndc) {
                    const f32 py = (0.5f - ndc * 0.5f) * static_cast<f32>(render_height);
                    return static_cast<u32>(std::clamp(std::floor(py / static_cast<f32>(grid.tile_px)), 0.0f, static_cast<f32>(grid.tiles_y - 1)));
                };
                fp.x0 = to_tile_x(lo.x);
                fp.x1 = to_tile_x(hi.x);
                fp.y0 = to_tile_y(hi.y);
                fp.y1 = to_tile_y(lo.y);
            }
            footprints[i] = fp;
            for (u32 s = fp.s0; s <= fp.s1; ++s) {
                for (u32 y = fp.y0; y <= fp.y1; ++y) {
                    for (u32 x = fp.x0; x <= fp.x1; ++x) {
                        u32 &count = counts[grid.cluster_index(x, y, s)];
                        if (count < max_per_cluster) {
                            ++count;
                        } else {
                            ++out.dropped;
                        }
                    }
                }
            }
        }
        out.ranges.resize(cluster_count);
        u32 offset = 0;
        for (u32 c = 0; c < cluster_count; ++c) {
            out.ranges[c] = glm::uvec2{offset, 0u};
            offset += counts[c];
        }
        out.indices.assign(offset, 0u);
        // Second pass in the same order as the first, so a full cluster keeps the earlier (more important) lights.
        for (usize i = 0; i < lights.size(); ++i) {
            const Footprint &fp = footprints[i];
            if (fp.x0 > fp.x1) continue;
            for (u32 s = fp.s0; s <= fp.s1; ++s) {
                for (u32 y = fp.y0; y <= fp.y1; ++y) {
                    for (u32 x = fp.x0; x <= fp.x1; ++x) {
                        glm::uvec2 &range = out.ranges[grid.cluster_index(x, y, s)];
                        if (range.y < counts[grid.cluster_index(x, y, s)]) {
                            out.indices[range.x + range.y] = static_cast<u32>(i);
                            ++range.y;
                        }
                    }
                }
            }
        }
        return out;
    }

} // namespace SFT::Renderer
