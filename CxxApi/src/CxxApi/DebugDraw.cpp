#include <CxxApi/DebugDraw.hpp>

#include <algorithm>

#include <CxxApi/Conversions.hpp>

namespace SFT::CxxApi {

    Engine::DebugDraw &engine_debug_draw(Engine::Engine &engine) noexcept { return engine.debug_draw(); }

    void debug_set_enabled(Engine::DebugDraw &draw, bool enabled) { draw.set_enabled(enabled); }

    bool debug_enabled(const Engine::DebugDraw &draw) { return draw.enabled(); }

    void debug_set_width(Engine::DebugDraw &draw, float width_px) { draw.set_width(width_px); }

    void debug_set_occlusion(Engine::DebugDraw &draw, bool depth_test, float occluded_alpha) {
        Renderer::DebugDrawSettings settings = draw.settings();
        settings.depth_test = depth_test;
        settings.occluded_alpha = std::clamp(occluded_alpha, 0.0f, 1.0f);
        draw.set_settings(settings);
    }

    void debug_clear(Engine::DebugDraw &draw) { draw.clear(); }

    std::uint64_t debug_line_count(const Engine::DebugDraw &draw) { return draw.line_count(); }

    void debug_line(Engine::DebugDraw &draw, std::array<float, 3> a, std::array<float, 3> b, std::array<float, 4> color, float seconds) {
        draw.line(to_vec3(a), to_vec3(b), to_vec4(color), seconds);
    }

    void debug_polyline(Engine::DebugDraw &draw, const std::vector<float> &points_xyz, std::array<float, 4> color, bool closed,
                        float seconds) {
        std::vector<glm::vec3> points;
        points.reserve(points_xyz.size() / 3);
        for (std::size_t i = 0; i + 2 < points_xyz.size(); i += 3) {
            points.emplace_back(points_xyz[i], points_xyz[i + 1], points_xyz[i + 2]);
        }
        draw.polyline(points, to_vec4(color), closed, seconds);
    }

    void debug_cross(Engine::DebugDraw &draw, std::array<float, 3> point, float size, std::array<float, 4> color, float seconds) {
        draw.cross(to_vec3(point), size, to_vec4(color), seconds);
    }

    void debug_box(Engine::DebugDraw &draw, std::array<float, 3> min, std::array<float, 3> max, std::array<float, 4> color, float seconds) {
        draw.box(to_vec3(min), to_vec3(max), to_vec4(color), seconds);
    }

    void debug_oriented_box(Engine::DebugDraw &draw, std::array<float, 16> transform, std::array<float, 3> half_extents,
                            std::array<float, 4> color, float seconds) {
        draw.oriented_box(to_mat4(transform), to_vec3(half_extents), to_vec4(color), seconds);
    }

    void debug_sphere(Engine::DebugDraw &draw, std::array<float, 3> center, float radius, std::array<float, 4> color, float seconds) {
        draw.sphere(to_vec3(center), radius, to_vec4(color), seconds);
    }

    void debug_circle(Engine::DebugDraw &draw, std::array<float, 3> center, std::array<float, 3> normal, float radius,
                      std::array<float, 4> color, float seconds) {
        draw.circle(to_vec3(center), to_vec3(normal), radius, to_vec4(color), seconds);
    }

    void debug_capsule(Engine::DebugDraw &draw, std::array<float, 3> a, std::array<float, 3> b, float radius, std::array<float, 4> color,
                       float seconds) {
        draw.capsule(to_vec3(a), to_vec3(b), radius, to_vec4(color), seconds);
    }

    void debug_cone(Engine::DebugDraw &draw, std::array<float, 3> apex, std::array<float, 3> direction, float length,
                    float half_angle_radians, std::array<float, 4> color, float seconds) {
        draw.cone(to_vec3(apex), to_vec3(direction), length, half_angle_radians, to_vec4(color), seconds);
    }

    void debug_arrow(Engine::DebugDraw &draw, std::array<float, 3> from, std::array<float, 3> to, std::array<float, 4> color,
                     float head_size, float seconds) {
        draw.arrow(to_vec3(from), to_vec3(to), to_vec4(color), head_size, seconds);
    }

    void debug_axes(Engine::DebugDraw &draw, std::array<float, 16> transform, float size, float seconds) {
        draw.axes(to_mat4(transform), size, seconds);
    }

    void debug_frustum(Engine::DebugDraw &draw, std::array<float, 16> inverse_view_projection, std::array<float, 4> color, float seconds) {
        draw.frustum(to_mat4(inverse_view_projection), to_vec4(color), seconds);
    }

    void debug_grid(Engine::DebugDraw &draw, std::array<float, 3> center, std::array<float, 3> axis_u, std::array<float, 3> axis_v,
                    std::uint32_t half_cells, float cell_size, std::array<float, 4> color, float seconds) {
        draw.grid(to_vec3(center), to_vec3(axis_u), to_vec3(axis_v), half_cells, cell_size, to_vec4(color), seconds);
    }

} // namespace SFT::CxxApi
