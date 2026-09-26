#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <array>
#include <string>
#include <string_view>
#include <vector>
#pragma endregion

#include <RHI/RHI.hpp>

using std::array;
using std::string;
using std::string_view;
using std::vector;

// Vocabulary shared by every displacement block. See plans/displacement-system.md for how the blocks
// fit together; the short version is that displacement is three independent decisions —
//
//   1. how a *pixel* finds the displaced surface        (HeightfieldAlgorithm)
//   2. how the *mesh* is refined into displaced geometry (GeometryPath)
//   3. what the deferred pipeline is told about the result (DepthPolicy / SelfShadowQuality)
//
// — each of which has a ladder of implementations from "works on anything that can sample a texture"
// to "needs modern hardware". Nothing in this header touches the GPU; it is all plain data so the
// selection logic in DisplacementPlanner can be unit-tested without a device.
namespace SFT::Renderer::Displacement {

    /// Coarse hardware/quality class a user or preset picks. It bounds *how much* the planner may
    /// spend; DisplacementCapabilities bounds what the device can do at all. The planner honours both.
    enum class HardwareTier : u8 {
        Low,
        Medium,
        High,
        Ultra,
        Cinematic,
    };

    inline constexpr array<HardwareTier, 5> kAllHardwareTiers{
        HardwareTier::Low, HardwareTier::Medium, HardwareTier::High, HardwareTier::Ultra, HardwareTier::Cinematic};

    /// Per-pixel block: how a fragment finds the displaced surface. Ordered by cost/fidelity, so
    /// `a < b` means "cheaper and less accurate", which the planner's degrade loop relies on.
    /// Every entry runs on any device that can sample a texture with an explicit LOD — including
    /// WebGPU and pre-2019 GPUs — because the ladder's whole purpose is that the bottom rungs never
    /// become unavailable.
    enum class HeightfieldAlgorithm : u8 {
        /// Shading-only: displaced normal from the height gradient, no change to visibility.
        NormalOnly,
        /// Single-offset parallax with limiting. Cheapest view-dependent option.
        Parallax,
        /// Fixed-step linear search + binary refinement (classic POM / relief mapping).
        ParallaxOcclusion,
        /// DDA over level-0 cells, exact bilinear ray/surface solve per cell. No hierarchy needed.
        CellExact,
        /// Min/max-hierarchy space skipping over CellExact leaves. Needs a HeightfieldHierarchy.
        HierarchicalCellExact,
    };

    inline constexpr array<HeightfieldAlgorithm, 5> kAllHeightfieldAlgorithms{
        HeightfieldAlgorithm::NormalOnly, HeightfieldAlgorithm::Parallax, HeightfieldAlgorithm::ParallaxOcclusion,
        HeightfieldAlgorithm::CellExact, HeightfieldAlgorithm::HierarchicalCellExact};

    /// Per-mesh block: how (whether) the base mesh is refined into real displaced triangles.
    /// `None` keeps the base silhouette; the others change it.
    enum class GeometryPath : u8 {
        None,
        /// Legacy path. A CPU-subdivided mesh (subdivide_uniform) is drawn by an ordinary vertex
        /// shader that fetches the height per vertex. Runs on everything with vertex texture fetch.
        PreTessellatedVertex,
        /// Modern path. A task shader picks a per-triangle subdivision level and culls; a mesh shader
        /// emits the displaced sub-triangles directly, so no refined mesh is ever stored. Needs
        /// RHI::Feature::MeshShader (+ TaskShader for the amplification stage).
        MeshShader,
    };

    inline constexpr array<GeometryPath, 3> kAllGeometryPaths{GeometryPath::None, GeometryPath::PreTessellatedVertex,
                                                             GeometryPath::MeshShader};

    /// Ray-tracing slot: what a *secondary ray* (reflection, refraction, GI, shadow) sees where a
    /// displaced material is. Ordered by capability requirement, so `a < b` = "needs less hardware".
    /// Same universal-fallback rule as every other ladder: the top rung uses hardware ray tracing only
    /// because the middle rung exists for any device that can run a compute shader (WebGPU included).
    /// See plans/displacement-system.md section 9 and Renderer/Displacement/RayTracingReference.hpp.
    enum class RayTracingPath : u8 {
        /// Rays see the base triangle: the surface is flat where the raster path shows relief.
        None,
        /// Software: a compute (or ray-query-free) kernel intersects rays against the displaced triangle
        /// list — prism entry/exit, then the hf_trace_grid blocks — with no acceleration structure at
        /// all. Any compute-capable device runs it; speed is the only thing it gives up.
        SoftwareHeightfield,
        /// Hardware: each base triangle's prism is a procedural (AABB) primitive in the BVH and the
        /// displaced-triangle test runs in the ray query's candidate loop (inline ray query, not the
        /// RT pipeline — WebGPU-style targets without pipelines still qualify once they expose queries).
        /// Needs Feature::AccelerationStructures + Feature::RayQuery.
        ProjectivePrism,
    };

    inline constexpr array<RayTracingPath, 3> kAllRayTracingPaths{RayTracingPath::None,
                                                                 RayTracingPath::SoftwareHeightfield,
                                                                 RayTracingPath::ProjectivePrism};

    /// What the G-buffer pass records for a displaced material.
    enum class DepthPolicy : u8 {
        /// Raster depth stays at the base surface. Cheapest; early-Z intact; screen-space effects
        /// see the base mesh ("shading displacement, not scene geometry").
        BaseSurface,
        /// The fragment writes the displaced depth (SV_Depth). Correct for SSAO/GTAO/contact shadows,
        /// but disables early-Z for that draw and must run after any depth prepass that would
        /// otherwise reject it.
        DisplacedDepth,
    };

    enum class SelfShadowQuality : u8 {
        Off,
        Cheap,
        Production,
        High,
        Reference,
    };

    /// Who builds the min/max hierarchy that HierarchicalCellExact reads.
    enum class HierarchyBuild : u8 {
        /// build_hierarchy() on the CPU at import/edit time. Works everywhere, including targets with
        /// no storage-image write for the hierarchy format.
        Cpu,
        /// Compute pass that reduces the height texture level by level. Preferred when the height
        /// data is dynamic (sculpting/animation) — see HeightfieldHierarchy::update_region for the
        /// matching CPU incremental path.
        Compute,
    };

    /// Stored precision of a hierarchy node. The builder rounds min down and max up when quantizing,
    /// so a 16-bit hierarchy stays *conservative* (never skips a real hit).
    enum class HierarchyPrecision : u8 {
        Unorm16,
        Float32,
    };

    /// Which bounds a node stores. MaxOnly halves the memory and is enough for the skip test; MinMax
    /// additionally lets a node whose minimum is above the ray report an early definite hit.
    enum class HierarchyChannels : u8 {
        MaxOnly,
        MinMax,
    };

    /// Concrete self-shadow traversal budget one SelfShadowQuality resolves to (see self_shadow_budget in
    /// DisplacementPlanner.hpp). Pure data: the shader consumes these through HeightfieldSettings, the
    /// planner owns the mapping.
    struct SelfShadowBudget {
        /// Shadow rays cast per shaded pixel; 0 = shadowing off.
        u32 ray_count = 0;
        /// True when several rays are jittered across the light's angular size (soft shadows) instead of
        /// one hard ray. Only meaningful for ray_count > 1.
        b8 stochastic = false;
        /// Levels added to the shadow ray's starting hierarchy level (coarser = cheaper start).
        u32 start_level_bias = 0;
        /// Fraction of the view ray's step budget a shadow ray may spend, in (0, 1].
        f32 step_fraction = 1.0f;
    };

    /// What the device can do, reduced to the questions displacement actually asks. Kept separate
    /// from RHI::FeatureSet so tests (and tools that pick a preset for a machine they are not
    /// running on) can construct one by hand.
    struct DisplacementCapabilities {
        b8 mesh_shader = false;
        b8 task_shader = false;
        u32 max_mesh_output_vertices = 0;
        u32 max_mesh_output_primitives = 0;
        u32 max_mesh_payload_bytes = 0;
        /// Storage-image write for the two-channel hierarchy format. RHI has no per-format storage
        /// capability query yet (see the XeGTAO note about forced R32Float), so this defaults to
        /// false and the planner falls back to the CPU build.
        b8 compute_hierarchy_build = false;
        /// SV_Depth writes are portable everywhere, but callers on a backend that cannot honour them
        /// can veto DepthPolicy::DisplacedDepth here.
        b8 fragment_depth_write = true;
        /// Compute shaders. Universal on any post-2019 device (WebGPU included), so it defaults true and
        /// baseline() keeps it: the SoftwareHeightfield ray-tracing rung rests on it.
        b8 compute_shader = true;
        /// Bottom/top-level acceleration structures (RHI Feature::AccelerationStructures).
        b8 acceleration_structures = false;
        /// Inline ray queries from compute/fragment shaders (RHI Feature::RayQuery).
        b8 ray_query = false;
        /// Procedural (AABB) primitives inside an acceleration structure. The RHI exposes them as
        /// AccelerationStructureGeometryType::Aabbs, part of the AccelerationStructures feature, so
        /// from_rhi() mirrors that bit; it is separate so a backend that cannot build them can veto it.
        b8 procedural_primitives = false;
        /// Four-texel gather (GatherRed) of the height texture. Needs a sampler bound with the texture, which the
        /// WebGPU/WGSL path deliberately does not have (HfTexture is a plain Texture2D there), so a caller on that
        /// backend sets this false and the planner drops TierProfile::corner_gather with a recorded downgrade.
        b8 height_gather = true;

        /// Reads the mesh-shader bits out of the RHI. Storage-format and depth-write bits keep their
        /// conservative defaults until the RHI can answer them.
        [[nodiscard]] static DisplacementCapabilities from_rhi(const RHI::FeatureSet &features,
                                                               const RHI::FeatureProperties &properties) noexcept;

        /// The lowest common denominator: a device that can only sample textures and run vertex and
        /// fragment shaders. Everything the planner ever emits for this must still work.
        [[nodiscard]] static constexpr DisplacementCapabilities baseline() noexcept { return {}; }
    };

    /// Budget knobs one HardwareTier grants. The planner clamps the request to this and then to
    /// DisplacementCapabilities.
    struct TierProfile {
        HardwareTier tier = HardwareTier::Low;
        HeightfieldAlgorithm max_algorithm = HeightfieldAlgorithm::NormalOnly;
        GeometryPath max_geometry = GeometryPath::None;
        DepthPolicy max_depth = DepthPolicy::BaseSurface;
        SelfShadowQuality max_self_shadow = SelfShadowQuality::Off;
        /// Highest ray-tracing rung this tier may spend on secondary rays (see RayTracingPath).
        RayTracingPath max_ray_tracing = RayTracingPath::None;
        /// Hard ceiling on traversal iterations per pixel (POM steps, or DDA/hierarchy visits).
        u32 max_traversal_steps = 0;
        /// Target on-screen edge length, in pixels, the geometry path refines toward.
        f32 target_edge_pixels = 16.0f;
        /// Cap on per-triangle subdivision level (mesh shader) / pre-tessellation level (legacy).
        u32 max_subdivision_level = 0;
        /// Bias added to the mip chosen for traversal. Positive = coarser = cheaper.
        f32 traversal_mip_bias = 0.0f;
        HierarchyPrecision hierarchy_precision = HierarchyPrecision::Unorm16;
        HierarchyChannels hierarchy_channels = HierarchyChannels::MaxOnly;
        /// Optional traversal optimizations (plans/displacement-system.md section 7). Both default OFF so a
        /// profile reproduces the verified baseline shader; a profile opts in by setting them.
        /// One four-texel GatherRed per leaf cell instead of four Load()s (SFT_HF_USE_GATHER).
        b8 corner_gather = false;
        /// Start the hierarchy walk at a level derived from the pixel's texel footprint
        /// (SFT_HF_START_LEVEL_FROM_FOOTPRINT).
        b8 start_level_from_footprint = false;
    };

    /// What a material/asset *asks for*, before tier and capability limits. Materials request a
    /// displacement operation, never a specific algorithm, so the same asset scales across tiers.
    struct DisplacementRequest {
        /// Highest fidelity the asset is authored to benefit from.
        HeightfieldAlgorithm desired_algorithm = HeightfieldAlgorithm::HierarchicalCellExact;
        GeometryPath desired_geometry = GeometryPath::None;
        DepthPolicy desired_depth = DepthPolicy::BaseSurface;
        SelfShadowQuality desired_self_shadow = SelfShadowQuality::Off;
        /// What secondary rays should see. Opt-in (None): secondary-ray displacement costs a BVH of
        /// prisms or a software trace per ray, which only assets whose reflections/GI matter should pay.
        RayTracingPath desired_ray_tracing = RayTracingPath::None;
        /// The asset's silhouette genuinely depends on the displacement. Material-space projection
        /// cannot deliver that, so this is what makes the planner reach for a GeometryPath at all.
        b8 silhouette_critical = false;
        /// Height data changes at runtime, so a precomputed CPU hierarchy would go stale.
        b8 dynamic_heightfield = false;
    };

    /// One step the planner took away from the request, recorded so a debug overlay (or a bug
    /// report) can say why a material is not rendering at its requested fidelity.
    struct PlanDowngrade {
        string what;
        string why;
    };

    /// The resolved decision for one displaced material at one tier on one device.
    struct DisplacementPlan {
        HeightfieldAlgorithm algorithm = HeightfieldAlgorithm::NormalOnly;
        GeometryPath geometry = GeometryPath::None;
        DepthPolicy depth = DepthPolicy::BaseSurface;
        SelfShadowQuality self_shadow = SelfShadowQuality::Off;
        /// How secondary rays see the displaced surface. Independent of `geometry`: even a mesh-shader
        /// displaced mesh rasterizes displaced but is a flat triangle to the BVH unless this is set.
        RayTracingPath ray_tracing = RayTracingPath::None;
        HierarchyBuild hierarchy_build = HierarchyBuild::Cpu;
        HierarchyPrecision hierarchy_precision = HierarchyPrecision::Unorm16;
        HierarchyChannels hierarchy_channels = HierarchyChannels::MaxOnly;
        u32 max_traversal_steps = 0;
        u32 max_subdivision_level = 0;
        f32 target_edge_pixels = 16.0f;
        f32 traversal_mip_bias = 0.0f;
        /// Copied from the TierProfile; see there. Drive shader variant defines via optimization_defines().
        b8 corner_gather = false;
        b8 start_level_from_footprint = false;
        vector<PlanDowngrade> downgrades;

        /// True when the plan needs a min/max hierarchy resource attached to the heightfield.
        [[nodiscard]] bool needs_hierarchy() const noexcept {
            return algorithm == HeightfieldAlgorithm::HierarchicalCellExact || ray_tracing != RayTracingPath::None;
        }

        /// True when the plan needs a *prism* acceleration structure (procedural AABB BLAS over the base
        /// triangles). Depends only on the base mesh and (height_scale, reference height) unless tight
        /// height bounds are used, so a dynamic heightfield never forces a rebuild by itself.
        [[nodiscard]] bool needs_prism_acceleration_structure() const noexcept {
            return ray_tracing == RayTracingPath::ProjectivePrism;
        }
    };

    /// View-dependent inputs for the per-object algorithm choice (plans/displacement.md §23). These
    /// change every frame; DisplacementPlan changes only with settings/device.
    struct DisplacementViewMetrics {
        /// Approximate on-screen size of the surface, in pixels along its longer axis.
        f32 projected_size_pixels = 0.0f;
        /// |dot(view, base normal)|, 1 = head-on, ~0 = grazing.
        f32 view_cos = 1.0f;
        /// Displacement amplitude projected to pixels (height_scale * pixels-per-world-unit).
        f32 amplitude_pixels = 0.0f;
        /// Height texels covered by one screen pixel along the view ray's UV footprint.
        f32 texels_per_pixel = 1.0f;
    };

    [[nodiscard]] string_view to_string(HardwareTier tier) noexcept;
    [[nodiscard]] string_view to_string(HeightfieldAlgorithm algorithm) noexcept;
    [[nodiscard]] string_view to_string(GeometryPath path) noexcept;
    [[nodiscard]] string_view to_string(DepthPolicy policy) noexcept;
    [[nodiscard]] string_view to_string(SelfShadowQuality quality) noexcept;
    [[nodiscard]] string_view to_string(RayTracingPath path) noexcept;

} // namespace SFT::Renderer::Displacement
