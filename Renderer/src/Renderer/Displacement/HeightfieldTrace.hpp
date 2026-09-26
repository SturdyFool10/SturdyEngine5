#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <optional>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#pragma endregion

#include <Renderer/Displacement/HeightfieldHierarchy.hpp>

using std::optional;

// CPU reference implementation of the heightfield projection blocks. Shaders/sturdy_heightfield.slang
// implements the *same* algorithms; this file exists so that
//   * the maths (bilinear cell solve, hierarchical skipping, conservative bounds) is unit-tested on
//     a CPU with no GPU or shader compiler in the loop, and
//   * a shader regression has a known-good answer to be compared against.
// When one changes, change the other and keep the parameter names aligned.
namespace SFT::Renderer::Displacement {

    /// Ray through heightfield space: x/y are UV in tile units (the heightfield repeats every 1.0 when
    /// it wraps), z is *normalized height* in [0, 1] — the caller folds height_scale into the ray
    /// direction. The heightfield occupies the slab 0 <= z <= 1; the surface is z = H(u, v).
    ///
    /// A view ray has direction.z < 0 (descending into the slab) and finds the first point where the
    /// ray dips to or below the surface. A shadow ray has direction.z > 0 and asks whether anything
    /// is in the way before the top of the slab.
    struct HeightfieldRay {
        glm::vec3 origin{};
        glm::vec3 direction{};
        /// Upper bound on the ray parameter t. Bounds work for near-horizontal rays that would
        /// otherwise wander across an unbounded number of repeated tiles.
        f32 max_t = 1.0e30f;
    };

    enum class HitStatus : u8 {
        /// The ray met the surface (or, for a shadow ray, was blocked).
        Hit,
        /// The ray left the volume — the slab, or the texture rectangle for non-wrapping fields —
        /// without meeting the surface.
        Miss,
        /// The step budget ran out first. `t`/`uv` are the ray's position at that point: a usable
        /// approximation, but confidence is low.
        BudgetExhausted,
    };

    struct HeightfieldHit {
        HitStatus status = HitStatus::Miss;
        f32 t = 0.0f;
        /// Hit position in tile units (not wrapped: a repeating field can report u > 1).
        glm::vec2 uv{};
        f32 height = 0.0f;
        /// dH/du, dH/dv of the surface at the hit, in normalized-height per tile unit. Feed to
        /// heightfield_normal() together with the material's physical scale.
        glm::vec2 gradient{};
        /// |ray height - surface height| at the reported point. Near zero for an exact cell solve.
        f32 residual = 0.0f;
        u32 steps = 0;
    };

    /// Result of solving one bilinear cell against a ray segment.
    struct CellSolve {
        /// Segment-local parameter in [0, segment_length] of the first ray/surface crossing.
        f32 s = 0.0f;
        f32 residual = 0.0f;
    };

    /// Solves for the first point in [0, segment_length] where a ray meets the bilinear patch
    /// H(fu, fv) = h00 + (h10-h00) fu + (h01-h00) fv + (h00-h10-h01+h11) fu fv.
    ///
    /// The ray is given by its state at the segment start (local cell coordinates fu0/fv0 in [0, 1],
    /// ray height h0) and per-unit-parameter rates (dfu, dfv, dh). Substituting the ray into the patch
    /// gives a quadratic A s^2 + B s + C = 0 in the segment parameter; roots use the cancellation-safe
    /// formulation and a near-zero A degrades to the linear solve. Returns nullopt when the ray stays
    /// above the patch for the whole segment.
    [[nodiscard]] optional<CellSolve> solve_bilinear_cell(f32 h00, f32 h10, f32 h01, f32 h11, f32 fu0, f32 fv0,
                                                          f32 h0, f32 dfu, f32 dfv, f32 dh,
                                                          f32 segment_length) noexcept;

    /// Bilinear height at (u, v) in tile units — what a filtered texture fetch returns.
    [[nodiscard]] f32 sample_height(const HeightfieldView &heightfield, f32 u, f32 v) noexcept;

    /// dH/du, dH/dv at (u, v) in tile units, from the same bilinear patch.
    [[nodiscard]] glm::vec2 sample_gradient(const HeightfieldView &heightfield, f32 u, f32 v) noexcept;

    /// Tangent-space normal of z = height_scale * H(u/size.x, v/size.y): normalize(-dz/dx, -dz/dy, 1).
    /// `tile_size` is the world size one UV tile covers.
    [[nodiscard]] glm::vec3 heightfield_normal(const glm::vec2 &gradient, f32 height_scale,
                                               const glm::vec2 &tile_size) noexcept;

    /// Builds the slab ray for a shaded point. `view_ts` points from the surface toward the eye in
    /// tangent space (z > 0). The base surface sits at normalized height `reference_height` (1 =
    /// "depth map", the mesh is the top of the volume; 0 = "height map", the mesh is the bottom), the
    /// ray is extended back to the top of the slab so the volume is traversed completely, and its
    /// parameter t = 0 lies on that top plane.
    [[nodiscard]] HeightfieldRay make_view_ray(const glm::vec2 &uv, const glm::vec3 &view_ts, f32 height_scale,
                                               const glm::vec2 &tile_size, f32 reference_height) noexcept;

    /// Pass as `start_level` to begin at the hierarchy's top level (the original behaviour).
    inline constexpr u32 kStartAtTopLevel = 0xFFFFFFFFu;

    /// Hierarchical cell-exact traversal — the "best" block. `hierarchy` must have been built from
    /// `heightfield`. Skips whole regions whose max-height bound is below the ray, descends where the
    /// bound is not decisive, and solves the exact bilinear patch at level 0.
    ///
    /// `max_steps` counts loop iterations (each skip, descent and leaf visit is one); it is the value
    /// DisplacementPlan::max_traversal_steps carries.
    ///
    /// `start_level` is the hierarchy level the walk begins at (clamped to the top level). It is purely a
    /// step-count hint — the walk still ascends after a skip and descends on doubt from wherever it
    /// starts — so it never changes which cell is hit, only how many iterations are needed to find it
    /// (see select_traversal_start_level in DisplacementPlanner.hpp for how it is chosen). Rays that
    /// cross only a few cells are cheaper starting low; long rays should start high.
    [[nodiscard]] HeightfieldHit trace_hierarchical(const HeightfieldView &heightfield,
                                                    const HeightfieldHierarchy &hierarchy,
                                                    const HeightfieldRay &ray, u32 max_steps,
                                                    u32 start_level = kStartAtTopLevel) noexcept;

    /// Cell-exact traversal without a hierarchy: a plain 2D DDA over level-0 cells with the same exact
    /// solve. Needs no precomputed data, so it is the fallback whenever a hierarchy is unavailable.
    [[nodiscard]] HeightfieldHit trace_cell_exact(const HeightfieldView &heightfield, const HeightfieldRay &ray,
                                                  u32 max_steps) noexcept;

    /// Classic POM: `steps` uniform samples along the slab crossing, then a few bisection refinements.
    /// Can miss features thinner than a step; that is exactly the failure the exact blocks remove.
    [[nodiscard]] HeightfieldHit trace_parallax_occlusion(const HeightfieldView &heightfield,
                                                          const HeightfieldRay &ray, u32 steps,
                                                          u32 refinements = 5) noexcept;

    /// Deliberately slow, deliberately independent ground truth (very fine uniform stepping + long
    /// bisection). Only for tests and offline comparison; shares no code with the blocks above.
    [[nodiscard]] HeightfieldHit trace_brute_force(const HeightfieldView &heightfield, const HeightfieldRay &ray,
                                                   u32 samples = 65536) noexcept;

    /// Self-shadow query: true if the surface blocks the ray from a point on it toward the light.
    /// `surface_uv`/`surface_height` is the shaded point, `light_ts` the tangent-space direction to
    /// the light, `bias` lifts the start off the surface so it does not shadow itself.
    [[nodiscard]] bool trace_shadow(const HeightfieldView &heightfield, const HeightfieldHierarchy &hierarchy,
                                    const glm::vec2 &surface_uv, f32 surface_height, const glm::vec3 &light_ts,
                                    f32 height_scale, const glm::vec2 &tile_size, u32 max_steps,
                                    f32 bias = 2.0e-3f, u32 start_level = kStartAtTopLevel) noexcept;

} // namespace SFT::Renderer::Displacement
