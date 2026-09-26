#pragma once

#include <Foundation/Foundation.hpp>

#include <utility>

#include <Renderer/Displacement/DisplacementTypes.hpp>

namespace SFT::Renderer::Displacement {

    /// The budget a hardware tier grants. Total function: every tier has a profile, and the profiles
    /// are monotone — a higher tier never grants less than a lower one on any axis (the test suite
    /// checks this, because a violated ordering would make "raise the quality setting" look worse).
    ///
    /// Even Low grants ParallaxOcclusion: the goal is that old hardware still gets real view-dependent
    /// material depth, just with fewer steps and a coarser traversal mip.
    [[nodiscard]] TierProfile profile_for_tier(HardwareTier tier) noexcept;

    /// Largest per-triangle subdivision level a mesh-shader device can emit given its output limits,
    /// or 0 when it cannot run the displacement mesh shader at all. Mirrors the constants the shader
    /// declares (see DisplacementMesh.hpp) so shader and planner cannot silently disagree.
    [[nodiscard]] u32 max_mesh_shader_subdivision(const DisplacementCapabilities &capabilities) noexcept;

    /// Resolves a request against a tier and a device.
    ///
    /// Rules, in order:
    ///  * Each axis is clamped to the tier profile, then to what the device can do; every clamp is
    ///    recorded in DisplacementPlan::downgrades with the reason.
    ///  * A GeometryPath other than None already carries the displacement, so the per-pixel algorithm
    ///    collapses to NormalOnly (re-displacing in the fragment stage would apply the heightfield
    ///    twice) and depth stays at the base surface (the geometry's depth is already correct).
    ///  * `silhouette_critical` promotes the geometry path to the best one the tier allows; without
    ///    it, geometry is only used if the request explicitly asked for it.
    ///  * The MeshShader path falls back to PreTessellatedVertex, never to None, when the device lacks
    ///    mesh shaders: a silhouette-critical asset should degrade in quality, not lose its silhouette.
    [[nodiscard]] DisplacementPlan plan_displacement(const TierProfile &profile,
                                                     const DisplacementCapabilities &capabilities,
                                                     const DisplacementRequest &request);

    /// Convenience overload: profile_for_tier(tier).
    [[nodiscard]] DisplacementPlan plan_displacement(HardwareTier tier, const DisplacementCapabilities &capabilities,
                                                     const DisplacementRequest &request);

    /// Per-frame, per-object refinement of a plan's per-pixel algorithm (plans/displacement.md §23):
    /// never returns anything more expensive than `plan.algorithm`, but drops to a cheaper rung when
    /// the extra fidelity would be invisible at this size/angle/amplitude.
    [[nodiscard]] HeightfieldAlgorithm select_algorithm_for_view(const DisplacementPlan &plan,
                                                                 const DisplacementViewMetrics &metrics) noexcept;

    /// Traversal step budget for one object this frame: the plan's ceiling, shrunk for small or
    /// head-on surfaces (few cells are crossed) and left whole for grazing ones.
    [[nodiscard]] u32 select_traversal_steps(const DisplacementPlan &plan,
                                             const DisplacementViewMetrics &metrics) noexcept;

    /// Mip level the traversal should start from, from the pixel's texel footprint and the plan's
    /// bias, clamped to [0, mip_count - 1]. Coarser mips are cheaper; the final hit is still solved
    /// against level 0 for hierarchical/cell-exact so silhouettes of detail do not blur.
    [[nodiscard]] u32 select_traversal_mip(const DisplacementPlan &plan, const DisplacementViewMetrics &metrics,
                                           u32 mip_count) noexcept;

    /// Hierarchy level the walk should begin at for this pixel: the footprint-derived mip
    /// (select_traversal_mip over `level_count` hierarchy levels), or the top level when the plan does not opt
    /// into start_level_from_footprint. Never changes a trace's hit (trace_hierarchical's start_level is only a
    /// step-count hint); pass the result as `start_level`, and as HeightfieldSettings::startLevel.
    [[nodiscard]] u32 select_traversal_start_level(const DisplacementPlan &plan, const DisplacementViewMetrics &metrics,
                                                   u32 level_count) noexcept;

    /// The concrete budget a self-shadow quality resolves to:
    ///   Off        no rays
    ///   Cheap      1 hard ray, start 2 levels coarser than the view ray, a quarter of the steps
    ///   Production 1 hard ray, full traversal
    ///   High       1 hard ray, full traversal (the tier above it buys its budget through max_traversal_steps)
    ///   Reference  8 stochastic rays across the light's angular size, full traversal
    /// Monotone: a higher quality never gets fewer rays, fewer steps or a coarser start.
    [[nodiscard]] SelfShadowBudget self_shadow_budget(SelfShadowQuality quality) noexcept;

    /// Step ceiling for one shadow ray given the view ray's step budget: budget.step_fraction of it, never
    /// below 4 (or the view budget itself if smaller), never above it.
    [[nodiscard]] u32 self_shadow_max_steps(const SelfShadowBudget &budget, u32 view_max_steps) noexcept;

    /// Starting hierarchy level for a shadow ray: the view ray's start level plus the budget's bias, clamped
    /// to the top level (level_count - 1).
    [[nodiscard]] u32 self_shadow_start_level(const SelfShadowBudget &budget, u32 view_start_level,
                                              u32 level_count) noexcept;

    /// Preprocessor defines (name, value) the optional traversal optimizations need in the shader variant
    /// for this plan; empty for a plan that enables none, in which case the variant is the baseline shader.
    /// Deliberately excludes SFT_HF_ALGORITHM / SFT_DISPLACEMENT_* — those are the renderer's.
    [[nodiscard]] vector<std::pair<string, string>> optimization_defines(const DisplacementPlan &plan);

} // namespace SFT::Renderer::Displacement
