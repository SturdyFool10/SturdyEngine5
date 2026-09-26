#include <Renderer/Displacement/DisplacementMesh.hpp>

#include <algorithm>
#include <cmath>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace SFT::Renderer::Displacement {

    u32 select_edge_level(const glm::vec3 &a, const glm::vec3 &b, const TriangleLodInput &input) noexcept {
        const f32 length = glm::length(b - a);
        const glm::vec3 midpoint = (a + b) * 0.5f;
        // Distance to the *nearest* point the displaced edge could reach is what governs its apparent size;
        // midpoint distance minus half the edge is a cheap, stable stand-in that also stops an edge
        // straddling the camera from dividing by zero. Every term is symmetric in (a, b).
        const f32 distance = std::max(glm::length(midpoint - input.camera_position) - 0.5f * length, 1.0e-3f);

        const f32 edge_pixels = (length + input.max_displacement) * input.pixels_per_unit_at_one / distance;
        const f32 wanted = std::ceil(edge_pixels / std::max(input.target_edge_pixels, 1.0e-3f));

        const u32 ceiling = std::max(input.max_level, 1u);
        if (!(wanted >= 1.0f)) { // also catches NaN
            return 1;
        }
        return static_cast<u32>(std::min(wanted, static_cast<f32>(ceiling)));
    }

    u32 select_subdivision_level(const TriangleLodInput &input) noexcept {
        return std::max({select_edge_level(input.p0, input.p1, input), select_edge_level(input.p1, input.p2, input),
                         select_edge_level(input.p2, input.p0, input)});
    }

    bool edge_runs_forward(const glm::vec3 &a, const glm::vec3 &b) noexcept {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    }

    f32 snap_edge_parameter(u32 step, u32 steps, u32 edge_level, bool a_is_origin) noexcept {
        const u32 n = std::max(steps, 1u);
        const u32 levels = std::max(edge_level, 1u);
        const u32 canonical_step = a_is_origin ? step : n - step;
        // round-half-up(canonical_step * levels / n), exactly, in integers.
        const u32 g = (2u * canonical_step * levels + n) / (2u * n);
        return static_cast<f32>(g) / static_cast<f32>(levels);
    }

    glm::vec3 snapped_vertex_position(const glm::vec3 &p0, const glm::vec3 &p1, const glm::vec3 &p2,
                                      const array<u32, 3> &edge_levels, u32 level, u32 row, u32 col) noexcept {
        const u32 n = std::max(level, 1u);
        // A point on an edge is lerp(canonical origin, other endpoint, t) from the SAME two operands in the
        // SAME order on both sides of the edge, so it is bit-identical for both triangles; the general
        // barycentric sum would round differently depending on which corner comes first.
        auto on_edge = [&](const glm::vec3 &pa, const glm::vec3 &pb, u32 step, u32 edge_level) {
            const bool forward = edge_runs_forward(pa, pb);
            const f32 t = snap_edge_parameter(step, n, edge_level, forward);
            // Exact at both ends (the shader's exactLerp): a + t * (b - a) is not bit-exact at t = 1.
            const auto exact_mix = [](const glm::vec3 &lo, const glm::vec3 &hi, f32 w) {
                return w <= 0.0f ? lo : (w >= 1.0f ? hi : glm::mix(lo, hi, w));
            };
            return forward ? exact_mix(pa, pb, t) : exact_mix(pb, pa, t);
        };

        if (row == 0) {
            return on_edge(p0, p1, col, edge_levels[0]); // v0 -> v1
        }
        if (col + row == level) {
            return on_edge(p1, p2, row, edge_levels[1]); // v1 -> v2
        }
        if (col == 0) {
            return on_edge(p0, p2, row, edge_levels[2]); // v0 -> v2
        }
        const f32 wb = static_cast<f32>(col) / static_cast<f32>(n);
        const f32 wc = static_cast<f32>(row) / static_cast<f32>(n);
        return p0 * (1.0f - wb - wc) + p1 * wb + p2 * wc;
    }

    SubdivisionCost subdivision_cost(u64 base_triangle_count, u32 level) noexcept {
        const u64 n = std::max(level, 1u);
        SubdivisionCost cost;
        cost.vertex_count = base_triangle_count * subdivided_vertex_count(static_cast<u32>(n));
        cost.index_count = base_triangle_count * subdivided_triangle_count(static_cast<u32>(n)) * 3u;
        cost.vertex_bytes = cost.vertex_count * sizeof(GeometryVertex);
        cost.index_bytes = cost.index_count * sizeof(u32);
        return cost;
    }

    SubdividedMesh subdivide_uniform(span<const GeometryVertex> vertices, span<const u32> indices, u32 level) {
        SubdividedMesh out;
        if (level <= 1) {
            out.vertices.assign(vertices.begin(), vertices.end());
            out.indices.assign(indices.begin(), indices.end());
            return out;
        }

        const usize triangle_count = indices.size() / 3;
        const SubdivisionCost cost = subdivision_cost(triangle_count, level);
        out.vertices.reserve(static_cast<usize>(cost.vertex_count));
        out.indices.reserve(static_cast<usize>(cost.index_count));

        const f32 inv_n = 1.0f / static_cast<f32>(level);
        for (usize t = 0; t < triangle_count; ++t) {
            const u32 i0 = indices[t * 3 + 0];
            const u32 i1 = indices[t * 3 + 1];
            const u32 i2 = indices[t * 3 + 2];
            if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
                continue; // malformed triangle: drop rather than read out of bounds
            }
            const GeometryVertex &a = vertices[i0];
            const GeometryVertex &b = vertices[i1];
            const GeometryVertex &c = vertices[i2];

            const auto base = static_cast<u32>(out.vertices.size());
            // Row r holds level+1-r vertices; row_start(r) is the running total of the rows above.
            auto row_start = [level](u32 r) { return r * (level + 1u) - r * (r - 1u) / 2u; };

            for (u32 r = 0; r <= level; ++r) {
                for (u32 col = 0; col <= level - r; ++col) {
                    const f32 wb = static_cast<f32>(col) * inv_n;
                    const f32 wc = static_cast<f32>(r) * inv_n;
                    const f32 wa = 1.0f - wb - wc;

                    GeometryVertex v;
                    v.position = a.position * wa + b.position * wb + c.position * wc;
                    v.normal = glm::normalize(a.normal * wa + b.normal * wb + c.normal * wc);
                    v.uv = a.uv * wa + b.uv * wb + c.uv * wc;
                    v.color = a.color * wa + b.color * wb + c.color * wc;
                    const glm::vec3 tangent = glm::vec3(a.tangent) * wa + glm::vec3(b.tangent) * wb +
                                              glm::vec3(c.tangent) * wc;
                    // Handedness is a per-triangle constant in glTF, so it is copied, never blended.
                    v.tangent = glm::vec4(glm::normalize(tangent), a.tangent.w);
                    out.vertices.push_back(v);
                }
            }

            for (u32 r = 0; r < level; ++r) {
                for (u32 col = 0; col < level - r; ++col) {
                    const u32 p = base + row_start(r) + col;
                    const u32 q = base + row_start(r + 1) + col;
                    // "Up" triangle keeps the source winding; the "down" one fills the rhombus.
                    out.indices.insert(out.indices.end(), {p, p + 1, q});
                    if (col + 1 < level - r) {
                        out.indices.insert(out.indices.end(), {p + 1, q + 1, q});
                    }
                }
            }
        }
        return out;
    }

} // namespace SFT::Renderer::Displacement
