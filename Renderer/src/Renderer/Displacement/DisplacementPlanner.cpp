#include <Renderer/Displacement/DisplacementPlanner.hpp>

#include <algorithm>
#include <cmath>

#include <Renderer/Displacement/DisplacementMesh.hpp>

namespace SFT::Renderer::Displacement {

    namespace {

        template <typename Enum>
        [[nodiscard]] constexpr Enum min_rung(Enum a, Enum b) noexcept {
            return static_cast<u8>(a) < static_cast<u8>(b) ? a : b;
        }

        void note(DisplacementPlan &plan, string what, string why) {
            plan.downgrades.push_back(PlanDowngrade{std::move(what), std::move(why)});
        }

        /// Clamps `wanted` to `ceiling`, recording a downgrade when that changes it.
        template <typename Enum>
        Enum clamp_to_tier(DisplacementPlan &plan, Enum wanted, Enum ceiling, string_view axis, HardwareTier tier) {
            if (static_cast<u8>(wanted) <= static_cast<u8>(ceiling)) {
                return wanted;
            }
            note(plan, string(axis) + ": " + string(to_string(wanted)) + " -> " + string(to_string(ceiling)),
                 "capped by the " + string(to_string(tier)) + " tier");
            return ceiling;
        }

    } // namespace

    string_view to_string(HardwareTier tier) noexcept {
        switch (tier) {
            case HardwareTier::Low: return "Low";
            case HardwareTier::Medium: return "Medium";
            case HardwareTier::High: return "High";
            case HardwareTier::Ultra: return "Ultra";
            case HardwareTier::Cinematic: return "Cinematic";
        }
        return "?";
    }

    string_view to_string(HeightfieldAlgorithm algorithm) noexcept {
        switch (algorithm) {
            case HeightfieldAlgorithm::NormalOnly: return "NormalOnly";
            case HeightfieldAlgorithm::Parallax: return "Parallax";
            case HeightfieldAlgorithm::ParallaxOcclusion: return "ParallaxOcclusion";
            case HeightfieldAlgorithm::CellExact: return "CellExact";
            case HeightfieldAlgorithm::HierarchicalCellExact: return "HierarchicalCellExact";
        }
        return "?";
    }

    string_view to_string(GeometryPath path) noexcept {
        switch (path) {
            case GeometryPath::None: return "None";
            case GeometryPath::PreTessellatedVertex: return "PreTessellatedVertex";
            case GeometryPath::MeshShader: return "MeshShader";
        }
        return "?";
    }

    string_view to_string(DepthPolicy policy) noexcept {
        switch (policy) {
            case DepthPolicy::BaseSurface: return "BaseSurface";
            case DepthPolicy::DisplacedDepth: return "DisplacedDepth";
        }
        return "?";
    }

    string_view to_string(RayTracingPath path) noexcept {
        switch (path) {
            case RayTracingPath::None: return "None";
            case RayTracingPath::SoftwareHeightfield: return "SoftwareHeightfield";
            case RayTracingPath::ProjectivePrism: return "ProjectivePrism";
        }
        return "?";
    }

    string_view to_string(SelfShadowQuality quality) noexcept {
        switch (quality) {
            case SelfShadowQuality::Off: return "Off";
            case SelfShadowQuality::Cheap: return "Cheap";
            case SelfShadowQuality::Production: return "Production";
            case SelfShadowQuality::High: return "High";
            case SelfShadowQuality::Reference: return "Reference";
        }
        return "?";
    }

    DisplacementCapabilities DisplacementCapabilities::from_rhi(const RHI::FeatureSet &features,
                                                                const RHI::FeatureProperties &properties) noexcept {
        DisplacementCapabilities caps;
        caps.mesh_shader = features.has(RHI::Feature::MeshShader);
        caps.task_shader = features.has(RHI::Feature::TaskShader);
        caps.max_mesh_output_vertices = properties.mesh_shader.max_mesh_output_vertices;
        caps.max_mesh_output_primitives = properties.mesh_shader.max_mesh_output_primitives;
        caps.max_mesh_payload_bytes = properties.mesh_shader.max_mesh_payload_size;
        caps.acceleration_structures = features.has(RHI::Feature::AccelerationStructures);
        caps.ray_query = features.has(RHI::Feature::RayQuery);
        // AABB geometry is part of the AccelerationStructures feature in every RHI backend that has it.
        caps.procedural_primitives = caps.acceleration_structures;
        return caps;
    }

    TierProfile profile_for_tier(HardwareTier tier) noexcept {
        TierProfile p;
        p.tier = tier;
        switch (tier) {
            case HardwareTier::Low:
                // Old/weak hardware still gets view-dependent depth — just with a short march and a
                // coarser traversal mip. No geometry, no depth writes: both cost more than the
                // cheapest tier can spare, and the base silhouette is the honest fallback.
                p.max_algorithm = HeightfieldAlgorithm::ParallaxOcclusion;
                p.max_geometry = GeometryPath::None;
                p.max_depth = DepthPolicy::BaseSurface;
                p.max_self_shadow = SelfShadowQuality::Off;
                p.max_ray_tracing = RayTracingPath::None;
                p.max_traversal_steps = 16;
                p.target_edge_pixels = 32.0f;
                p.max_subdivision_level = 0;
                p.traversal_mip_bias = 1.0f;
                p.hierarchy_precision = HierarchyPrecision::Unorm16;
                p.hierarchy_channels = HierarchyChannels::MaxOnly;
                break;
            case HardwareTier::Medium:
                p.max_algorithm = HeightfieldAlgorithm::CellExact;
                p.max_geometry = GeometryPath::PreTessellatedVertex;
                p.max_depth = DepthPolicy::BaseSurface;
                p.max_self_shadow = SelfShadowQuality::Cheap;
                p.max_ray_tracing = RayTracingPath::SoftwareHeightfield;
                p.max_traversal_steps = 32;
                p.target_edge_pixels = 20.0f;
                p.max_subdivision_level = 4;
                p.traversal_mip_bias = 0.5f;
                p.hierarchy_precision = HierarchyPrecision::Unorm16;
                p.hierarchy_channels = HierarchyChannels::MaxOnly;
                break;
            case HardwareTier::High:
                p.max_algorithm = HeightfieldAlgorithm::HierarchicalCellExact;
                p.max_geometry = GeometryPath::MeshShader;
                p.max_depth = DepthPolicy::DisplacedDepth;
                p.max_self_shadow = SelfShadowQuality::Production;
                p.max_ray_tracing = RayTracingPath::ProjectivePrism;
                p.max_traversal_steps = 64;
                p.target_edge_pixels = 12.0f;
                p.max_subdivision_level = 6;
                p.traversal_mip_bias = 0.0f;
                p.hierarchy_precision = HierarchyPrecision::Unorm16;
                p.hierarchy_channels = HierarchyChannels::MaxOnly;
                break;
            case HardwareTier::Ultra:
                p.max_algorithm = HeightfieldAlgorithm::HierarchicalCellExact;
                p.max_geometry = GeometryPath::MeshShader;
                p.max_depth = DepthPolicy::DisplacedDepth;
                p.max_self_shadow = SelfShadowQuality::High;
                p.max_ray_tracing = RayTracingPath::ProjectivePrism;
                p.max_traversal_steps = 128;
                p.target_edge_pixels = 8.0f;
                p.max_subdivision_level = 8;
                p.traversal_mip_bias = 0.0f;
                p.hierarchy_precision = HierarchyPrecision::Unorm16;
                p.hierarchy_channels = HierarchyChannels::MaxOnly;
                break;
            case HardwareTier::Cinematic:
                // Offline-quality: budget is effectively "until it converges", full-precision bounds,
                // and a negative bias so traversal never reads a coarser mip than the pixel needs.
                p.max_algorithm = HeightfieldAlgorithm::HierarchicalCellExact;
                p.max_geometry = GeometryPath::MeshShader;
                p.max_depth = DepthPolicy::DisplacedDepth;
                p.max_self_shadow = SelfShadowQuality::Reference;
                p.max_ray_tracing = RayTracingPath::ProjectivePrism;
                p.max_traversal_steps = 512;
                p.target_edge_pixels = 2.0f;
                p.max_subdivision_level = kMeshMaxSubdivisionLevel;
                p.traversal_mip_bias = -1.0f;
                p.hierarchy_precision = HierarchyPrecision::Float32;
                p.hierarchy_channels = HierarchyChannels::MaxOnly;
                break;
        }
        return p;
    }

    u32 max_mesh_shader_subdivision(const DisplacementCapabilities &capabilities) noexcept {
        if (!capabilities.mesh_shader || !capabilities.task_shader) {
            return 0;
        }
        if (capabilities.max_mesh_payload_bytes < kTaskPayloadBytes) {
            return 0;
        }
        u32 best = 0;
        for (u32 level = 1; level <= kMeshMaxSubdivisionLevel; ++level) {
            if (subdivided_vertex_count(level) <= capabilities.max_mesh_output_vertices &&
                subdivided_triangle_count(level) <= capabilities.max_mesh_output_primitives) {
                best = level;
            }
        }
        return best;
    }

    DisplacementPlan plan_displacement(const TierProfile &profile, const DisplacementCapabilities &capabilities,
                                       const DisplacementRequest &request) {
        DisplacementPlan plan;
        plan.hierarchy_precision = profile.hierarchy_precision;
        plan.hierarchy_channels = profile.hierarchy_channels;
        plan.max_traversal_steps = profile.max_traversal_steps;
        plan.target_edge_pixels = profile.target_edge_pixels;
        plan.traversal_mip_bias = profile.traversal_mip_bias;
        plan.corner_gather = profile.corner_gather;
        if (plan.corner_gather && !capabilities.height_gather) {
            plan.corner_gather = false;
            note(plan, "corner gather", "the backend binds the height texture without a sampler (WebGPU/WGSL); using four Load()s");
        }
        plan.start_level_from_footprint = profile.start_level_from_footprint;

        // --- Geometry path -------------------------------------------------------------------------
        GeometryPath wanted_geometry = request.desired_geometry;
        if (request.silhouette_critical && wanted_geometry == GeometryPath::None) {
            wanted_geometry = GeometryPath::MeshShader;
        }
        wanted_geometry = clamp_to_tier(plan, wanted_geometry, profile.max_geometry, "geometry", profile.tier);

        u32 subdivision = profile.max_subdivision_level;
        if (wanted_geometry == GeometryPath::MeshShader) {
            const u32 mesh_limit = max_mesh_shader_subdivision(capabilities);
            if (mesh_limit == 0) {
                note(plan, "geometry: MeshShader -> PreTessellatedVertex",
                     "device has no usable mesh/task shader stage (or its output limits are below the shader's)");
                wanted_geometry = GeometryPath::PreTessellatedVertex;
            } else if (mesh_limit < subdivision) {
                note(plan, "subdivision: " + std::to_string(subdivision) + " -> " + std::to_string(mesh_limit),
                     "device mesh-shader output limits");
                subdivision = mesh_limit;
            }
        }
        if (wanted_geometry != GeometryPath::None && subdivision == 0) {
            // A tier that grants geometry but a zero subdivision cap would emit unrefined triangles
            // through a more expensive path; that is strictly worse than no geometry path.
            note(plan, "geometry: " + string(to_string(wanted_geometry)) + " -> None",
                 "tier grants no subdivision levels");
            wanted_geometry = GeometryPath::None;
        }
        plan.geometry = wanted_geometry;
        plan.max_subdivision_level = wanted_geometry == GeometryPath::None ? 0 : subdivision;

        // --- Per-pixel algorithm ---------------------------------------------------------------------
        HeightfieldAlgorithm algorithm =
            clamp_to_tier(plan, request.desired_algorithm, profile.max_algorithm, "algorithm", profile.tier);
        if (plan.geometry != GeometryPath::None && algorithm != HeightfieldAlgorithm::NormalOnly) {
            note(plan, "algorithm: " + string(to_string(algorithm)) + " -> NormalOnly",
                 "geometry path already displaces the surface; projecting again would apply the heightfield twice");
            algorithm = HeightfieldAlgorithm::NormalOnly;
        }
        plan.algorithm = algorithm;

        // --- Depth policy ----------------------------------------------------------------------------
        DepthPolicy depth = clamp_to_tier(plan, request.desired_depth, profile.max_depth, "depth", profile.tier);
        if (depth == DepthPolicy::DisplacedDepth) {
            if (plan.geometry != GeometryPath::None) {
                // Real geometry already rasterizes at the displaced depth; nothing to write.
                depth = DepthPolicy::BaseSurface;
            } else if (plan.algorithm <= HeightfieldAlgorithm::Parallax) {
                // NormalOnly/Parallax never leave the base surface enough to be worth losing early-Z.
                note(plan, "depth: DisplacedDepth -> BaseSurface",
                     "the chosen algorithm does not move the surface far enough to justify disabling early-Z");
                depth = DepthPolicy::BaseSurface;
            } else if (!capabilities.fragment_depth_write) {
                note(plan, "depth: DisplacedDepth -> BaseSurface", "backend cannot write fragment depth");
                depth = DepthPolicy::BaseSurface;
            }
        }
        plan.depth = depth;

        plan.self_shadow = clamp_to_tier(plan, request.desired_self_shadow, profile.max_self_shadow, "self-shadow",
                                         profile.tier);

        // --- Hierarchy -------------------------------------------------------------------------------
        // A dynamic heightfield rebuilds its bounds on the GPU when the device can; otherwise the CPU
        // incremental update (HeightfieldHierarchy::update_region) plus a re-upload is still correct.
        // --- Ray tracing -----------------------------------------------------------------------------
        // Independent of the geometry path: the BVH never sees rasterized displaced geometry.
        {
            RayTracingPath rt = clamp_to_tier(plan, request.desired_ray_tracing, profile.max_ray_tracing,
                                              "ray tracing", profile.tier);
            if (rt == RayTracingPath::ProjectivePrism &&
                !(capabilities.acceleration_structures && capabilities.ray_query && capabilities.procedural_primitives)) {
                // Degrade quality-of-speed, never lose the displaced surface: the software rung needs only
                // compute, so it is the floor for every device that can run one.
                note(plan, "ray tracing: ProjectivePrism -> SoftwareHeightfield",
                     "device lacks acceleration structures, inline ray query or procedural primitives");
                rt = RayTracingPath::SoftwareHeightfield;
            }
            if (rt == RayTracingPath::SoftwareHeightfield && !capabilities.compute_shader) {
                note(plan, "ray tracing: SoftwareHeightfield -> None",
                     "device cannot run compute shaders; secondary rays see the base triangle");
                rt = RayTracingPath::None;
            }
            plan.ray_tracing = rt;
        }

        plan.hierarchy_build = (request.dynamic_heightfield && capabilities.compute_hierarchy_build)
                                   ? HierarchyBuild::Compute
                                   : HierarchyBuild::Cpu;
        return plan;
    }

    DisplacementPlan plan_displacement(HardwareTier tier, const DisplacementCapabilities &capabilities,
                                       const DisplacementRequest &request) {
        return plan_displacement(profile_for_tier(tier), capabilities, request);
    }

    HeightfieldAlgorithm select_algorithm_for_view(const DisplacementPlan &plan,
                                                   const DisplacementViewMetrics &metrics) noexcept {
        if (plan.algorithm == HeightfieldAlgorithm::NormalOnly) {
            return HeightfieldAlgorithm::NormalOnly;
        }

        // Displacement smaller than a pixel, or a surface smaller than a handful of pixels, cannot
        // show occlusion; a normal map is visually identical and far cheaper.
        if (metrics.amplitude_pixels < 0.75f || metrics.projected_size_pixels < 24.0f) {
            return HeightfieldAlgorithm::NormalOnly;
        }

        HeightfieldAlgorithm wanted;
        const bool grazing = metrics.view_cos < 0.3f;
        const bool dense = metrics.texels_per_pixel > 4.0f;
        if (metrics.projected_size_pixels < 96.0f || metrics.amplitude_pixels < 1.5f) {
            wanted = HeightfieldAlgorithm::Parallax;
        } else if (grazing || dense) {
            // Long rays through texture space are where fixed-step search misses features and where
            // space skipping pays for its hierarchy fetches.
            wanted = HeightfieldAlgorithm::HierarchicalCellExact;
        } else {
            wanted = HeightfieldAlgorithm::ParallaxOcclusion;
            if (plan.algorithm >= HeightfieldAlgorithm::CellExact) {
                wanted = HeightfieldAlgorithm::CellExact;
            }
        }
        return min_rung(wanted, plan.algorithm);
    }

    u32 select_traversal_steps(const DisplacementPlan &plan, const DisplacementViewMetrics &metrics) noexcept {
        // A head-on ray crosses few cells; a grazing one crosses ~1/cos times as many. Scale the
        // ceiling by that, keeping at least a quarter so a mis-estimated footprint cannot starve it.
        const f32 cos_clamped = std::clamp(metrics.view_cos, 0.05f, 1.0f);
        const f32 scale = 0.25f + 0.75f * (1.0f - cos_clamped);
        const auto steps = static_cast<u32>(std::ceil(static_cast<f32>(plan.max_traversal_steps) * scale));
        return std::clamp(steps, std::min(4u, plan.max_traversal_steps), plan.max_traversal_steps);
    }

    u32 select_traversal_mip(const DisplacementPlan &plan, const DisplacementViewMetrics &metrics,
                             u32 mip_count) noexcept {
        if (mip_count == 0) {
            return 0;
        }
        const f32 footprint = std::max(metrics.texels_per_pixel, 1.0f);
        const f32 mip = std::log2(footprint) + plan.traversal_mip_bias;
        return static_cast<u32>(std::clamp(mip, 0.0f, static_cast<f32>(mip_count - 1)));
    }

    u32 select_traversal_start_level(const DisplacementPlan &plan, const DisplacementViewMetrics &metrics,
                                     u32 level_count) noexcept {
        if (level_count == 0) {
            return 0;
        }
        if (!plan.start_level_from_footprint) {
            return level_count - 1;
        }
        return select_traversal_mip(plan, metrics, level_count);
    }

    SelfShadowBudget self_shadow_budget(SelfShadowQuality quality) noexcept {
        SelfShadowBudget b;
        switch (quality) {
            case SelfShadowQuality::Off:
                b.ray_count = 0;
                b.step_fraction = 0.0f;
                break;
            case SelfShadowQuality::Cheap:
                b.ray_count = 1;
                b.start_level_bias = 2;
                b.step_fraction = 0.25f;
                break;
            case SelfShadowQuality::Production:
            case SelfShadowQuality::High:
                b.ray_count = 1;
                break;
            case SelfShadowQuality::Reference:
                b.ray_count = 8;
                b.stochastic = true;
                break;
        }
        return b;
    }

    u32 self_shadow_max_steps(const SelfShadowBudget &budget, u32 view_max_steps) noexcept {
        if (budget.ray_count == 0) {
            return 0;
        }
        const f32 fraction = std::clamp(budget.step_fraction, 0.0f, 1.0f);
        const auto steps = static_cast<u32>(std::ceil(static_cast<f32>(view_max_steps) * fraction));
        return std::clamp(steps, std::min(4u, view_max_steps), view_max_steps);
    }

    u32 self_shadow_start_level(const SelfShadowBudget &budget, u32 view_start_level, u32 level_count) noexcept {
        if (level_count == 0) {
            return 0;
        }
        return std::min(view_start_level + budget.start_level_bias, level_count - 1);
    }

    vector<std::pair<string, string>> optimization_defines(const DisplacementPlan &plan) {
        vector<std::pair<string, string>> defines;
        if (plan.corner_gather) {
            defines.emplace_back("SFT_HF_USE_GATHER", "1");
        }
        if (plan.start_level_from_footprint) {
            defines.emplace_back("SFT_HF_START_LEVEL_FROM_FOOTPRINT", "1");
        }
        return defines;
    }

} // namespace SFT::Renderer::Displacement
