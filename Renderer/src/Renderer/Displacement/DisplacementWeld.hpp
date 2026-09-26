#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <vector>

#include <glm/vec3.hpp>
#pragma endregion

#include <Renderer/Geometry.hpp>

using std::span;
using std::vector;

// Welded displacement-normal channel (plans/displacement-system.md section 7, item 7).
//
// Displacement moves each vertex along its normal. A hard-edged mesh (a cube, a faceted rock) stores a
// shared corner as several vertices with different *split* normals; each copy is pushed in a different
// direction and the surface tears open along the edge. The cure used everywhere is a separate normal
// just for displacement, identical for every vertex at the same position, while the shading normal stays
// split so the edge still looks hard.
//
// This computes that channel on the CPU. It is deliberately independent of the mesh's own normals and
// tangents: the shading data is left untouched. How the channel reaches the shader is the renderer's
// call (an extra vertex stream / a spare attribute); the vertex stage then uses it in place of
// VertexInput.normal for the displacement direction only (and, for the history path, the previous-frame
// push direction). Note the fragment-side projection (per-pixel blocks) works in the *shading* frame and
// is unaffected: it never moves geometry, so it cannot open cracks.
namespace SFT::Renderer::Displacement {

    /// One smoothed direction per input vertex; vertices whose positions coincide (within `weld_epsilon`,
    /// in mesh units) get bit-identical directions. Each face contributes its unit normal to the
    /// vertices at its corners, weighted by the corner's interior angle (so a fan of thin triangles at
    /// a vertex does not outvote one wide one), and the per-position sum is normalized. Degenerate
    /// triangles contribute nothing. A position whose contributions cancel exactly falls back to the
    /// average of the vertices' own normals. Out-of-range indices are ignored.
    [[nodiscard]] vector<glm::vec3> compute_welded_displacement_normals(span<const GeometryVertex> vertices,
                                                                        span<const u32> indices,
                                                                        f32 weld_epsilon = 1.0e-5f);

    /// Vertices' welded-group id (the lowest vertex index in the group): two vertices share a position iff
    /// they share an id. Exposed for tests and for tools that want to weld other attributes the same way.
    [[nodiscard]] vector<u32> position_weld_groups(span<const GeometryVertex> vertices, f32 weld_epsilon);

} // namespace SFT::Renderer::Displacement
