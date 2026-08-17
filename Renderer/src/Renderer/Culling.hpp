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

    /// Six inward-facing clip planes (ax + by + cz + d >= 0 for a point inside), extracted from a
    /// view-projection matrix via the standard Gribb/Hartmann method. Used to CPU-cull camera-visible
    /// draws against each render item's world-space bounding sphere before issuing a draw call — see
    /// record_render_item's call sites in RendererLifecycle.cpp. Order: left, right, bottom, top, near, far.
    struct Frustum {
        array<glm::vec4, 6> planes{};
    };

    [[nodiscard]] Frustum frustum_from_view_projection(const glm::mat4 &view_projection) noexcept;

    /// Conservative sphere-vs-frustum test: false only when the sphere is fully outside at least one
    /// plane. Spheres straddling a plane, or fully inside, both return true — a cheap, safe over-cull-
    /// avoidance test (never rejects something that might still be visible).
    [[nodiscard]] bool frustum_intersects_sphere(const Frustum &frustum, const glm::vec3 &center,
                                                        f32 radius) noexcept;

} // namespace SFT::Renderer
