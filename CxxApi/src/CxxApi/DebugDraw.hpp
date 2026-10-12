#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <Engine/DebugDraw.hpp>
#include <Engine/Engine.hpp>

/// Debug drawing (`Engine/DebugDraw.hpp`): lines, boxes, spheres, arrows and friends drawn over the frame, from game logic.
/// Colours are rgba in 0..1. `seconds` is how long a shape stays: 0 shows it until the next update (call it every update for a
/// steady image), a positive value keeps it that long. Matrices are column-major.
namespace SFT::CxxApi {

    [[nodiscard]] Engine::DebugDraw &engine_debug_draw(Engine::Engine &engine) noexcept;

    void debug_set_enabled(Engine::DebugDraw &draw, bool enabled);
    [[nodiscard]] bool debug_enabled(const Engine::DebugDraw &draw);
    /// Line width in pixels for the shapes drawn after this call.
    void debug_set_width(Engine::DebugDraw &draw, float width_px);
    /// `depth_test` hides (dims) lines behind scene geometry; `occluded_alpha` is how visible the hidden stretches stay (0..1).
    void debug_set_occlusion(Engine::DebugDraw &draw, bool depth_test, float occluded_alpha);
    void debug_clear(Engine::DebugDraw &draw);
    [[nodiscard]] std::uint64_t debug_line_count(const Engine::DebugDraw &draw);

    void debug_line(Engine::DebugDraw &draw, std::array<float, 3> a, std::array<float, 3> b, std::array<float, 4> color, float seconds);
    /// `points_xyz` is x, y, z triples; a trailing incomplete triple is ignored.
    void debug_polyline(Engine::DebugDraw &draw, const std::vector<float> &points_xyz, std::array<float, 4> color, bool closed,
                        float seconds);
    void debug_cross(Engine::DebugDraw &draw, std::array<float, 3> point, float size, std::array<float, 4> color, float seconds);
    void debug_box(Engine::DebugDraw &draw, std::array<float, 3> min, std::array<float, 3> max, std::array<float, 4> color, float seconds);
    void debug_oriented_box(Engine::DebugDraw &draw, std::array<float, 16> transform, std::array<float, 3> half_extents,
                            std::array<float, 4> color, float seconds);
    void debug_sphere(Engine::DebugDraw &draw, std::array<float, 3> center, float radius, std::array<float, 4> color, float seconds);
    void debug_circle(Engine::DebugDraw &draw, std::array<float, 3> center, std::array<float, 3> normal, float radius,
                      std::array<float, 4> color, float seconds);
    void debug_capsule(Engine::DebugDraw &draw, std::array<float, 3> a, std::array<float, 3> b, float radius, std::array<float, 4> color,
                       float seconds);
    void debug_cone(Engine::DebugDraw &draw, std::array<float, 3> apex, std::array<float, 3> direction, float length,
                    float half_angle_radians, std::array<float, 4> color, float seconds);
    /// `head_size` 0 = a fifth of the arrow.
    void debug_arrow(Engine::DebugDraw &draw, std::array<float, 3> from, std::array<float, 3> to, std::array<float, 4> color,
                     float head_size, float seconds);
    void debug_axes(Engine::DebugDraw &draw, std::array<float, 16> transform, float size, float seconds);
    void debug_frustum(Engine::DebugDraw &draw, std::array<float, 16> inverse_view_projection, std::array<float, 4> color, float seconds);
    void debug_grid(Engine::DebugDraw &draw, std::array<float, 3> center, std::array<float, 3> axis_u, std::array<float, 3> axis_v,
                    std::uint32_t half_cells, float cell_size, std::array<float, 4> color, float seconds);

} // namespace SFT::CxxApi
