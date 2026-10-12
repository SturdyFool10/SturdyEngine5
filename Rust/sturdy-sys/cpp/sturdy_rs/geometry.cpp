#include "sturdy_rs/geometry.hpp"

#include <cstring>
#include <vector>

#include <Renderer/Culling.hpp>

namespace sturdy_rs::geometry {

    std::unique_ptr<Spline3> spline3_create(rust::Slice<const float> waypoints, std::uint32_t samples_per_segment) {
        std::vector<glm::vec3> points;
        for (std::size_t i = 0; i + 2 < waypoints.size(); i += 3) {
            points.emplace_back(waypoints[i], waypoints[i + 1], waypoints[i + 2]);
        }
        return std::make_unique<Spline3>(Spline3::create(std::move(points), samples_per_segment));
    }
    bool spline3_valid(const Spline3 &s) noexcept { return s.valid(); }
    std::uint32_t spline3_segment_count(const Spline3 &s) noexcept { return s.segment_count(); }
    std::array<float, 3> spline3_position(const Spline3 &s, float t) noexcept {
        const glm::vec3 v = s.position(t);
        return {v.x, v.y, v.z};
    }
    std::array<float, 3> spline3_tangent(const Spline3 &s, float t) noexcept {
        const glm::vec3 v = s.tangent(t);
        return {v.x, v.y, v.z};
    }
    float spline3_total_length(const Spline3 &s) noexcept { return s.total_length(); }
    float spline3_t_at_arc_length(const Spline3 &s, float arc_length) noexcept { return s.t_at_arc_length(arc_length); }

    std::unique_ptr<Spline2> spline2_create(rust::Slice<const float> waypoints, std::uint32_t samples_per_segment) {
        std::vector<glm::vec2> points;
        for (std::size_t i = 0; i + 1 < waypoints.size(); i += 2) {
            points.emplace_back(waypoints[i], waypoints[i + 1]);
        }
        return std::make_unique<Spline2>(Spline2::create(std::move(points), samples_per_segment));
    }
    bool spline2_valid(const Spline2 &s) noexcept { return s.valid(); }
    std::uint32_t spline2_segment_count(const Spline2 &s) noexcept { return s.segment_count(); }
    std::array<float, 2> spline2_position(const Spline2 &s, float t) noexcept {
        const glm::vec2 v = s.position(t);
        return {v.x, v.y};
    }
    std::array<float, 2> spline2_tangent(const Spline2 &s, float t) noexcept {
        const glm::vec2 v = s.tangent(t);
        return {v.x, v.y};
    }
    float spline2_total_length(const Spline2 &s) noexcept { return s.total_length(); }
    float spline2_t_at_arc_length(const Spline2 &s, float arc_length) noexcept { return s.t_at_arc_length(arc_length); }

    std::array<float, 24> frustum_from_view_projection(const std::array<float, 16> &view_projection) noexcept {
        glm::mat4 matrix;
        std::memcpy(&matrix, view_projection.data(), sizeof(matrix));
        const SFT::Renderer::Frustum frustum = SFT::Renderer::frustum_from_view_projection(matrix);
        std::array<float, 24> out{};
        std::memcpy(out.data(), frustum.planes.data(), sizeof(out));
        return out;
    }

    bool frustum_intersects_sphere(const std::array<float, 24> &planes, std::array<float, 3> center,
                                   float radius) noexcept {
        SFT::Renderer::Frustum frustum;
        std::memcpy(frustum.planes.data(), planes.data(), sizeof(planes));
        return SFT::Renderer::frustum_intersects_sphere(frustum, glm::vec3{center[0], center[1], center[2]}, radius);
    }

} // namespace sturdy_rs::geometry
