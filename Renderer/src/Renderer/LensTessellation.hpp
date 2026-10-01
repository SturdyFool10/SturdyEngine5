#pragma once

#include <Foundation/Foundation.hpp>

#include <span>
#include <vector>

#include <Renderer/Geometry.hpp>

namespace SFT::Renderer {

    /// Default longest edge, in mesh-local units, the camera-lens variant of a mesh may keep.
    inline constexpr f32 kDefaultLensMaxEdgeLength = 0.4f;
    /// The renderer cuts a mesh's edges to its bounding radius divided by this (see try_upload_mesh).
    inline constexpr f32 kLensEdgesPerMeshRadius = 24.0f;

    struct LensTessellatedMesh {
        std::vector<GeometryVertex> vertices;
        std::vector<u32> indices;
        /// False when no edge was long enough to split (the caller keeps using the original mesh).
        bool subdivided = false;
    };

    /// Splits every triangle edge longer than `max_edge_length` at its midpoint, repeatedly, until none is (or
    /// `max_triangles` would be exceeded, in which case the last complete pass is returned).
    ///
    /// The vertex-warp camera lens (see `Shaders/sturdy_space.slang`) only moves vertices: the GPU still draws straight
    /// lines between them, so a large triangle near the screen edge bends the wrong way. Splitting long edges bounds
    /// that error. Splits are decided per *edge* (by its endpoint indices), so neighbouring triangles always agree on
    /// where an edge is cut and the result has no T-junction cracks. Attributes are interpolated linearly (normals and
    /// tangents renormalised).
    [[nodiscard]] LensTessellatedMesh lens_tessellate(std::span<const GeometryVertex> vertices, std::span<const u32> indices,
                                                      f32 max_edge_length = kDefaultLensMaxEdgeLength,
                                                      usize max_triangles = 1'000'000);

} // namespace SFT::Renderer
