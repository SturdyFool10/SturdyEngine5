#include <Renderer/UI/UiWorld.hpp>

#include <cmath>
#include <glm/geometric.hpp>

namespace SFT::UI {

    std::optional<glm::vec2> ui_uv_at_ray(const UiWorldPlane &plane, glm::vec3 ray_origin, glm::vec3 ray_direction,
                                          bool allow_outside) noexcept {
        const glm::vec3 normal = glm::cross(plane.u_axis, plane.v_axis);
        const f32 normal_length_squared = glm::dot(normal, normal);
        if (normal_length_squared <= 0.0f) {
            return std::nullopt;
        }
        const f32 denominator = glm::dot(normal, ray_direction);
        if (std::abs(denominator) < 1.0e-8f) {
            return std::nullopt;
        }
        const f32 t = glm::dot(normal, plane.origin - ray_origin) / denominator;
        if (t < 0.0f) {
            return std::nullopt;
        }
        const glm::vec3 offset = ray_origin + ray_direction * t - plane.origin;
        // Solve offset = u * u_axis + v * v_axis in the plane's (possibly skewed) basis.
        const f32 uu = glm::dot(plane.u_axis, plane.u_axis);
        const f32 uv_dot = glm::dot(plane.u_axis, plane.v_axis);
        const f32 vv = glm::dot(plane.v_axis, plane.v_axis);
        const f32 ou = glm::dot(offset, plane.u_axis);
        const f32 ov = glm::dot(offset, plane.v_axis);
        const f32 determinant = uu * vv - uv_dot * uv_dot;
        if (std::abs(determinant) < 1.0e-12f) {
            return std::nullopt;
        }
        const glm::vec2 result{(ou * vv - ov * uv_dot) / determinant, (ov * uu - ou * uv_dot) / determinant};
        if (!allow_outside && (result.x < 0.0f || result.x > 1.0f || result.y < 0.0f || result.y > 1.0f)) {
            return std::nullopt;
        }
        return result;
    }

    glm::vec2 ui_pixel_from_uv(glm::vec2 uv, glm::vec2 ui_extent) noexcept { return uv * ui_extent; }

} // namespace SFT::UI
