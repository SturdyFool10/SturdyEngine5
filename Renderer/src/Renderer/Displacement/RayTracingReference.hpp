#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#pragma endregion

#include <Renderer/Displacement/HeightfieldHierarchy.hpp>
#include <Renderer/Displacement/HeightfieldTrace.hpp>

// CPU reference for the ray-tracing displacement slot (plans/displacement-system.md section 9;
// plans/displacement.md sections 18-20). Shaders/heightfield_rt_prism.slang implements the *same*
// algorithms with the same names; when one changes, change the other.
//
// The problem. A hardware ray tracer intersects the *base* triangles, so reflections / refractions /
// GI / shadows see a flat surface where the raster path shows real relief. The fix has three parts:
//
//   1. PRISM. Every base triangle is extruded along its per-vertex normals into a prism that
//      encloses the displaced surface for that triangle. The prism is the acceleration-structure
//      primitive: the BVH is built over prism bounds (procedural/AABB primitives) instead of
//      triangles, so hardware traversal finds "which triangle's prism does this ray enter".
//   2. CHORD. Inside the prism a point is (barycentric b, normalized height h) and maps to world
//      space as  P(b, h) = sum_i b_i * (p_i + s * n_i),  s = (h - reference_height) * height_scale.
//      The ray's entry and exit crossings of the prism boundary are computed exactly (caps are
//      triangles, sides are bilinear patches). The ray is then *approximated* by the straight line
//      between those two crossings in (b, h) space — exact whenever the three vertex normals agree,
//      and the standard projective-displacement approximation otherwise.
//   3. TRACE. b is linear in uv (uv = sum_i b_i * uv_i), so that straight (b, h) segment is a
//      straight ray through heightfield space (u, v, h), which is precisely the ray the existing
//      hf_trace_grid / trace_hierarchical blocks solve. The heightfield blocks are reused untouched.
//
// Independence for testing: tests compare intersect_displaced_triangle against a brute-force march
// in world space (no prism, no chord, no cell solve), see DisplacementTest.cpp.
namespace SFT::Renderer::Displacement {

    /// One base triangle in whatever space the ray is in (object or world; the caller must keep the
    /// ray and the triangle in the same space, and non-uniform scale is the caller's problem). The
    /// normals are the *interpolated-normal* inputs — they need not be unit length, but their lengths
    /// scale the displacement, so the raster path's normalized normals should be passed.
    struct DisplacedTriangle {
        glm::vec3 position[3]{};
        glm::vec3 normal[3]{};
        /// Heightfield coordinates in tile units (the same UVs the raster path samples with).
        glm::vec2 uv[3]{};
    };

    /// The displaced-material half of the RT contract on the CPU. Field-for-field the same data
    /// HfRtDisplacementRecord carries on the GPU.
    struct DisplacedMaterialParams {
        /// World-space amplitude; displacement of normalized height h is (h - reference_height) * height_scale.
        f32 height_scale = 0.0f;
        /// Normalized height at which the base mesh sits (1 = depth map, 0 = height map, 0.5 = centred).
        f32 reference_height = 1.0f;
        /// World size one UV tile covers. Only needed for shading (the normal is computed from the exact
        /// surface derivative, which folds tile_size in through the uv basis), kept for contract parity.
        glm::vec2 tile_size{1.0f};
        /// Bounds of the heights actually present, in [0, 1]. The whole range [0, 1] is always valid;
        /// a hierarchy's root node (HeightfieldHierarchy::node at the top level) gives tight bounds,
        /// which shrink every prism. Tight bounds make the acceleration structure depend on the height
        /// data (rebuild after an edit); [0, 1] makes it depend only on the base mesh.
        f32 height_min = 0.0f;
        f32 height_max = 1.0f;
        /// Iteration budget for the heightfield traversal inside one prism.
        u32 max_steps = 256;
    };

    /// Sentinel for "this material is not displaced" in SpectralMaterial's displacement lane
    /// (textureIndices1.y) and for "no hierarchy texture" in HfRtDisplacementRecordGpu::textures.y.
    inline constexpr u32 kNoRtDisplacementRecord = 0xffffffffu;
    inline constexpr u32 kRtDisplacementFlagWrap = 1u;

    /// GPU twin of HfRtDisplacementRecord (Shaders/heightfield_rt_prism.slang): one 80-byte record per
    /// displaced material in a StructuredBuffer, referenced from the ray-query scene ABI through the
    /// material's reserved lane SpectralMaterial::textureIndices1.y. Texture indices address the same
    /// bindless material-texture heap the material's other textures live in.
    struct alignas(16) HfRtDisplacementRecordGpu {
        u32 textures[4]{}; ///< height texture, hierarchy texture (kNoRtDisplacementRecord = none), flags, max steps
        i32 sizes[4]{};    ///< height width, height height, hierarchy level count (1 = none), reserved
        f32 scale[4]{};    ///< height_scale, reference_height, tile_size.xy
        f32 height_range[4]{}; ///< height_min, height_max, uv scale.xy
        f32 uv_transform[4]{}; ///< uv offset.xy, reserved
    };
    static_assert(sizeof(HfRtDisplacementRecordGpu) == 80, "layout must match HfRtDisplacementRecord in the shader");
    static_assert(offsetof(HfRtDisplacementRecordGpu, height_range) == 48);

    /// Fills a record from the CPU-side material description. `uv_scale`/`uv_offset` map the mesh UV to
    /// heightfield tile units (identity when the mesh UV already is).
    [[nodiscard]] inline HfRtDisplacementRecordGpu make_rt_displacement_record(
        const DisplacedMaterialParams &params, u32 height_texture, u32 hierarchy_texture, u32 width, u32 height,
        u32 hierarchy_levels, bool wrap, const glm::vec2 &uv_scale = glm::vec2(1.0f),
        const glm::vec2 &uv_offset = glm::vec2(0.0f)) noexcept {
        HfRtDisplacementRecordGpu r;
        r.textures[0] = height_texture;
        r.textures[1] = hierarchy_texture;
        r.textures[2] = wrap ? kRtDisplacementFlagWrap : 0u;
        r.textures[3] = params.max_steps;
        r.sizes[0] = static_cast<i32>(width);
        r.sizes[1] = static_cast<i32>(height);
        r.sizes[2] = static_cast<i32>(hierarchy_levels);
        r.scale[0] = params.height_scale;
        r.scale[1] = params.reference_height;
        r.scale[2] = params.tile_size.x;
        r.scale[3] = params.tile_size.y;
        r.height_range[0] = params.height_min;
        r.height_range[1] = params.height_max;
        r.height_range[2] = uv_scale.x;
        r.height_range[3] = uv_scale.y;
        r.uv_transform[0] = uv_offset.x;
        r.uv_transform[1] = uv_offset.y;
        return r;
    }

    /// Axis-aligned bounds of a prism: the primitive bounds a BVH is built over.
    struct PrismBounds {
        glm::vec3 min{};
        glm::vec3 max{};
    };

    /// Corner positions of the prism: index [0..2] is the bottom cap (height_min), [3..5] the top cap.
    struct Prism {
        glm::vec3 corner[6]{};
        f32 s_min = 0.0f; ///< displacement at height_min
        f32 s_max = 0.0f; ///< displacement at height_max
    };

    [[nodiscard]] Prism make_prism(const DisplacedTriangle &triangle, const DisplacedMaterialParams &params) noexcept;

    /// Tight box around the prism's corners. The prism's side faces are bilinear, which lie inside the
    /// convex hull of their corners, so the corner box is conservative.
    [[nodiscard]] PrismBounds prism_bounds(const Prism &prism) noexcept;

    /// Where a ray crosses the prism boundary, as the interval [t_enter, t_exit] plus the (barycentric,
    /// height) state at each end. When the origin is inside the prism, t_enter is negative (the ray
    /// "entered" behind the origin); RayPrismRange::clipped_* is the interval clipped to the caller's
    /// [t_min, t_max], which is what the trace uses.
    struct RayPrismRange {
        bool hit = false;
        f32 t_enter = 0.0f;
        f32 t_exit = 0.0f;
        glm::vec3 bary_enter{};
        glm::vec3 bary_exit{};
        f32 height_enter = 0.0f;
        f32 height_exit = 0.0f;
    };

    /// Exact prism crossings: two cap triangles + three bilinear side patches (ray-bilinear-patch
    /// quadratic). The interval is the hull of all crossings, correct because the domain in (b, h) space
    /// is convex. Returns hit = false when the ray misses the prism or the hull lies outside [t_min, t_max].
    [[nodiscard]] RayPrismRange intersect_prism(const Prism &prism, const glm::vec3 &origin, const glm::vec3 &direction,
                                                f32 t_min, f32 t_max) noexcept;

    struct DisplacedHit {
        HitStatus status = HitStatus::Miss;
        /// Ray parameter of the hit (projected onto the ray when the chord approximation moves the
        /// point slightly off it).
        f32 t = 0.0f;
        /// Point on the displaced surface: P(b, h_surface).
        glm::vec3 position{};
        /// Unit normal of the displaced surface, on the side the interpolated vertex normal points to.
        glm::vec3 normal{};
        glm::vec2 uv{};
        glm::vec3 barycentrics{};
        /// Normalized height of the surface at the hit.
        f32 height = 0.0f;
        /// True when the ray met the surface from behind (direction . normal > 0).
        bool back_face = false;
        u32 steps = 0;
    };

    /// Ray vs one displaced triangle. `hierarchy` may be null (plain cell-exact DDA inside the prism).
    /// The ray must be normalized or at least consistent: t is in units of `direction`.
    ///
    /// Semantics inherited from the heightfield blocks: the volume *below* the surface is solid, a ray
    /// that starts inside it hits at its origin, and a ray reaching the prism's bottom cap has hit.
    [[nodiscard]] DisplacedHit intersect_displaced_triangle(const DisplacedTriangle &triangle,
                                                            const DisplacedMaterialParams &params,
                                                            const HeightfieldView &heightfield,
                                                            const HeightfieldHierarchy *hierarchy,
                                                            const glm::vec3 &origin, const glm::vec3 &direction,
                                                            f32 t_min, f32 t_max) noexcept;

    /// Exact (b, h) -> world map of the prism, exposed for tests and for shading.
    [[nodiscard]] glm::vec3 prism_position(const DisplacedTriangle &triangle, const DisplacedMaterialParams &params,
                                           const glm::vec3 &barycentrics, f32 height) noexcept;

    /// Deliberately independent ground truth: fine march along the ray in *world space*, testing at
    /// every sample whether the point is inside the prism's (b, h) domain and below the surface. It
    /// inverts P(b, h) by Newton iteration (exact inversion for a right prism) and shares no code with
    /// the prism/chord/cell-solve path. Tests and offline comparison only.
    [[nodiscard]] DisplacedHit brute_force_displaced_triangle(const DisplacedTriangle &triangle,
                                                              const DisplacedMaterialParams &params,
                                                              const HeightfieldView &heightfield,
                                                              const glm::vec3 &origin, const glm::vec3 &direction,
                                                              f32 t_min, f32 t_max, u32 samples = 20000) noexcept;

} // namespace SFT::Renderer::Displacement
