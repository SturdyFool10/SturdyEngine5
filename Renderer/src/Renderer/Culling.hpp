#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <array>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#pragma endregion

using std::array;

namespace SFT::Renderer {

    // Six inward-facing clip planes (ax + by + cz + d >= 0 for a point inside), extracted from a
    // view-projection matrix via the standard Gribb/Hartmann method. Used to CPU-cull camera-visible
    // draws against each render item's world-space bounding sphere before issuing a draw call — see
    // record_render_item's call sites in RendererLifecycle.cpp. Order: left, right, bottom, top, near, far.
    struct Frustum {
        array<glm::vec4, 6> planes{};
    };

    [[nodiscard]] inline Frustum frustum_from_view_projection(const glm::mat4 &view_projection) noexcept {
        const glm::mat4 &m = view_projection;
        Frustum frustum{};
        // Row i of m, accessed via m[col][row] since glm matrices are column-major.
        auto row = [&m](int r) { return glm::vec4{m[0][r], m[1][r], m[2][r], m[3][r]}; };
        const glm::vec4 r0 = row(0);
        const glm::vec4 r1 = row(1);
        const glm::vec4 r2 = row(2);
        const glm::vec4 r3 = row(3);
        frustum.planes[0] = r3 + r0; // left
        frustum.planes[1] = r3 - r0; // right
        frustum.planes[2] = r3 + r1; // bottom
        frustum.planes[3] = r3 - r1; // top
        frustum.planes[4] = r3 + r2; // near (0..1 depth convention, matches glm::*RH_ZO projections)
        frustum.planes[5] = r3 - r2; // far
        for (glm::vec4 &plane : frustum.planes) {
            const f32 length = glm::length(glm::vec3{plane});
            if (length > 1e-8f) {
                plane /= length;
            }
        }
        return frustum;
    }

    // Conservative sphere-vs-frustum test: false only when the sphere is fully outside at least one
    // plane. Spheres straddling a plane, or fully inside, both return true — a cheap, safe over-cull-
    // avoidance test (never rejects something that might still be visible).
    [[nodiscard]] inline bool frustum_intersects_sphere(const Frustum &frustum, const glm::vec3 &center,
                                                        f32 radius) noexcept {
        for (const glm::vec4 &plane : frustum.planes) {
            if (glm::dot(glm::vec3{plane}, center) + plane.w < -radius) {
                return false;
            }
        }
        return true;
    }

} // namespace SFT::Renderer
