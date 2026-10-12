// Rust <-> SturdyEngine 5 shim for Renderer/Spline.hpp and Renderer/Culling.hpp.
#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include <rust/cxx.h>

#include <Renderer/Spline.hpp>

namespace sturdy_rs::geometry {

    using Spline3 = SFT::Renderer::Spline3D;
    using Spline2 = SFT::Renderer::Spline2D;

    std::unique_ptr<Spline3> spline3_create(rust::Slice<const float> waypoints, std::uint32_t samples_per_segment);
    bool spline3_valid(const Spline3 &spline) noexcept;
    std::uint32_t spline3_segment_count(const Spline3 &spline) noexcept;
    std::array<float, 3> spline3_position(const Spline3 &spline, float t) noexcept;
    std::array<float, 3> spline3_tangent(const Spline3 &spline, float t) noexcept;
    float spline3_total_length(const Spline3 &spline) noexcept;
    float spline3_t_at_arc_length(const Spline3 &spline, float arc_length) noexcept;

    std::unique_ptr<Spline2> spline2_create(rust::Slice<const float> waypoints, std::uint32_t samples_per_segment);
    bool spline2_valid(const Spline2 &spline) noexcept;
    std::uint32_t spline2_segment_count(const Spline2 &spline) noexcept;
    std::array<float, 2> spline2_position(const Spline2 &spline, float t) noexcept;
    std::array<float, 2> spline2_tangent(const Spline2 &spline, float t) noexcept;
    float spline2_total_length(const Spline2 &spline) noexcept;
    float spline2_t_at_arc_length(const Spline2 &spline, float arc_length) noexcept;

    std::array<float, 24> frustum_from_view_projection(const std::array<float, 16> &view_projection) noexcept;
    bool frustum_intersects_sphere(const std::array<float, 24> &planes, std::array<float, 3> center,
                                   float radius) noexcept;

} // namespace sturdy_rs::geometry
