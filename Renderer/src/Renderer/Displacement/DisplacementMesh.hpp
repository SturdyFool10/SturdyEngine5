#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <array>
#include <span>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#pragma endregion

#include <Renderer/Displacement/DisplacementTypes.hpp>
#include <Renderer/Geometry.hpp>

using std::array;
using std::span;
using std::vector;

namespace SFT::Renderer::Displacement {

    // Constants the mesh/task shaders (Shaders/displacement_mesh.slang) also declare. They live here
    // so the planner's capability check and the CPU reference below stay in lockstep with the
    // shader; DisplacementTest re-derives the vertex/primitive counts from the subdivision level so a
    // change on either side that breaks the relationship fails a test instead of a GPU.

    /// Base triangles one task-shader workgroup classifies (one per lane).
    inline constexpr u32 kTaskTrianglesPerGroup = 32;
    /// Highest per-triangle subdivision level the mesh shader emits: (8+1)(8+2)/2 = 45 vertices and
    /// 8*8 = 64 primitives, inside the 64/126 budget every mesh-shader implementation guarantees.
    inline constexpr u32 kMeshMaxSubdivisionLevel = 8;
    /// task->mesh payload: count + kTaskTrianglesPerGroup triangle indices + as many levels.
    inline constexpr u32 kTaskPayloadBytes = static_cast<u32>(sizeof(u32)) * (1u + 2u * kTaskTrianglesPerGroup);

    /// Vertices in one triangle subdivided N times per edge: (N+1)(N+2)/2.
    [[nodiscard]] constexpr u32 subdivided_vertex_count(u32 level) noexcept { return (level + 1u) * (level + 2u) / 2u; }
    /// Triangles in one triangle subdivided N times per edge: N*N.
    [[nodiscard]] constexpr u32 subdivided_triangle_count(u32 level) noexcept { return level * level; }

    /// One base triangle's view of the world, in the units the task shader sees.
    struct TriangleLodInput {
        glm::vec3 p0{};
        glm::vec3 p1{};
        glm::vec3 p2{};
        /// World-space displacement amplitude (height_scale); pads culling bounds.
        f32 max_displacement = 0.0f;
        /// Pixels per world unit at unit distance from the camera along the view axis
        /// (viewport_height / (2 * tan(fov/2))).
        f32 pixels_per_unit_at_one = 0.0f;
        glm::vec3 camera_position{};
        f32 target_edge_pixels = 16.0f;
        u32 max_level = kMeshMaxSubdivisionLevel;
    };

    /// CPU mirror of the task/mesh shaders' edge level: the smallest level whose edge length projects to
    /// at most `target_edge_pixels`, with the displacement amplitude added to the edge so a tall
    /// heightfield on a short edge still refines. Depends only on the two endpoints (and the camera),
    /// never on which triangle asks — and is exactly symmetric in (a, b) — so the two triangles sharing an
    /// edge always agree on it. That agreement is what keeps the surface crack-free when their interior
    /// levels differ.
    [[nodiscard]] u32 select_edge_level(const glm::vec3 &a, const glm::vec3 &b, const TriangleLodInput &input) noexcept;

    /// Triangle level: the largest of its three edge levels (a triangle must be at least as fine as its
    /// finest edge). Returns 1 for triangles that need no refinement — a triangle always emits itself.
    /// `input.p0..p2` are the corners; the result is what the task shader writes into the payload.
    [[nodiscard]] u32 select_subdivision_level(const TriangleLodInput &input) noexcept;

    /// Snaps grid step `step` (of `steps`, running from endpoint `i` to endpoint `j`) to an edge's own
    /// level, and returns the resulting parameter measured from the edge's LOWER-ID endpoint.
    ///
    /// Two triangles sharing the edge list its endpoints in opposite orders, so each must reach the
    /// same answer from a mirrored input. Done in floating point that fails at exact rounding ties (the
    /// mirror of s is 1 - s, which is not always bit-exact), so it is done in integers: the step is
    /// mirrored as (steps - step), rounded half-up to the edge level with integer arithmetic, and only the
    /// final, exactly representable ratio g / edge_level becomes a float.
    [[nodiscard]] f32 snap_edge_parameter(u32 step, u32 steps, u32 edge_level, bool a_is_origin) noexcept;

    /// Canonical walking direction of an edge: true when `a` is the edge's origin, i.e. the lexicographically
    /// smaller endpoint (x, then y, then z). Depends on positions only, so duplicated vertices that share a
    /// position (uv / normal seams, welded copies) agree on it whatever their vertex indices are.
    [[nodiscard]] bool edge_runs_forward(const glm::vec3 &a, const glm::vec3 &b) noexcept;

    /// Position of vertex (row, col) of a triangle subdivided N times, with each edge's vertices snapped to
    /// that edge's own level (`edge_levels` = levels of edges v0v1, v1v2, v2v0; direction is canonicalized by
    /// edge_runs_forward). Interior vertices are plain barycentric. Mirrors the mesh
    /// shader; DisplacementTest uses it to prove adjacent triangles at different levels share an identical
    /// polyline along their common edge.
    [[nodiscard]] glm::vec3 snapped_vertex_position(const glm::vec3 &p0, const glm::vec3 &p1, const glm::vec3 &p2,
                                                    const array<u32, 3> &edge_levels,
                                                    u32 level, u32 row, u32 col) noexcept;

    /// Result of subdividing a mesh for the legacy path.
    struct SubdividedMesh {
        vector<GeometryVertex> vertices;
        vector<u32> indices;
    };

    /// Legacy geometry path: uniformly subdivide each triangle N times per edge, interpolating
    /// position/normal/uv/color/tangent barycentrically. Vertices are *not* welded across triangles,
    /// so no cracks can open from mismatched adjacent levels — at the cost of duplicated shared
    /// vertices; a uniform level across the mesh is expected here. The vertex shader then displaces
    /// each vertex from the heightfield (gbuffer_geometry_displaced.slang's vertexMainDisplaced), and because every
    /// duplicate of a shared corner samples the same texel at the same uv, the displaced result still
    /// agrees across triangle edges.
    ///
    /// @param level Subdivisions per edge; 0 and 1 both return a copy of the input.
    [[nodiscard]] SubdividedMesh subdivide_uniform(span<const GeometryVertex> vertices, span<const u32> indices,
                                                   u32 level);

    /// Exact output sizes subdivide_uniform will produce, for reserving and for the memory estimate
    /// that lets the planner reject a legacy plan before allocating.
    struct SubdivisionCost {
        u64 vertex_count = 0;
        u64 index_count = 0;
        u64 vertex_bytes = 0;
        u64 index_bytes = 0;
    };
    [[nodiscard]] SubdivisionCost subdivision_cost(u64 base_triangle_count, u32 level) noexcept;

} // namespace SFT::Renderer::Displacement
