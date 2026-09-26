#include <Renderer/Displacement/DisplacementMesh.hpp>
#include <Renderer/Displacement/DisplacementPlanner.hpp>
#include <Renderer/Displacement/HeightfieldHierarchy.hpp>
#include <Renderer/Displacement/HeightfieldTrace.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numeric>
#include <random>
#include <tuple>
#include <vector>

#include <glm/geometric.hpp>

// CPU-only tests for the displacement system: tier/capability planning, the min/max hierarchy, the
// exact bilinear-cell solver, and the reference traversal blocks. No GPU, no shader compiler.
//
// The traversal tests compare against trace_brute_force, which shares no code with the blocks under
// test. That independence is the point: a bug in the cell solve or the hierarchy skip would have to be
// reproduced identically by a fine uniform march to slip through.

namespace {

    using namespace SFT::Renderer;
    using namespace SFT::Renderer::Displacement;

    int g_failures = 0;

    bool check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
        return condition;
    }

    bool near(f32 a, f32 b, f32 tolerance) { return std::abs(a - b) <= tolerance; }

    // ---- Heightfield fixtures --------------------------------------------------------------------

    struct Field {
        std::vector<f32> heights;
        u32 width = 0;
        u32 height = 0;
        bool wrap = true;
        [[nodiscard]] HeightfieldView view() const { return HeightfieldView{heights, width, height, wrap}; }
    };

    /// Smooth, tileable rolling field: a few sines. Seam-free under wrap because every frequency is an
    /// integer number of periods across the texture.
    Field make_rolling_field(u32 w, u32 h, f32 amplitude) {
        Field f{{}, w, h, true};
        f.heights.resize(static_cast<usize>(w) * h);
        const f32 two_pi = 6.28318530718f;
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const f32 u = static_cast<f32>(x) / static_cast<f32>(w);
                const f32 v = static_cast<f32>(y) / static_cast<f32>(h);
                f32 s = 0.5f + amplitude * (0.5f * std::sin(two_pi * 2.0f * u) * std::cos(two_pi * 3.0f * v) +
                                            0.3f * std::sin(two_pi * 5.0f * (u + v)) + 0.2f * std::cos(two_pi * 7.0f * u));
                f.heights[static_cast<usize>(y) * w + x] = std::clamp(s, 0.0f, 1.0f);
            }
        }
        return f;
    }

    /// Hard, noisy field — random per texel, so plenty of narrow spikes for POM to miss.
    Field make_noise_field(u32 w, u32 h, u32 seed) {
        Field f{{}, w, h, true};
        f.heights.resize(static_cast<usize>(w) * h);
        std::mt19937 rng(seed);
        std::uniform_real_distribution<f32> dist(0.0f, 1.0f);
        for (f32 &v : f.heights) {
            v = dist(rng);
        }
        return f;
    }

    /// Mostly flat low plateau with a few tall pillars: the case hierarchical skipping exists for.
    Field make_sparse_field(u32 w, u32 h) {
        Field f{{}, w, h, true};
        f.heights.assign(static_cast<usize>(w) * h, 0.1f);
        for (u32 i = 0; i < 6; ++i) {
            const u32 x = (i * 37 + 11) % w;
            const u32 y = (i * 53 + 5) % h;
            f.heights[static_cast<usize>(y) * w + x] = 0.95f;
        }
        return f;
    }

    // ---- Planner ---------------------------------------------------------------------------------

    bool tier_profiles_are_monotone() {
        bool ok = true;
        for (usize i = 1; i < kAllHardwareTiers.size(); ++i) {
            const TierProfile lo = profile_for_tier(kAllHardwareTiers[i - 1]);
            const TierProfile hi = profile_for_tier(kAllHardwareTiers[i]);
            ok &= check(hi.max_algorithm >= lo.max_algorithm, "tier algorithm ceiling must not drop as the tier rises");
            ok &= check(hi.max_geometry >= lo.max_geometry, "tier geometry ceiling must not drop as the tier rises");
            ok &= check(hi.max_depth >= lo.max_depth, "tier depth policy must not drop as the tier rises");
            ok &= check(hi.max_self_shadow >= lo.max_self_shadow, "tier self-shadow must not drop as the tier rises");
            ok &= check(hi.max_traversal_steps >= lo.max_traversal_steps, "traversal budget must not drop as the tier rises");
            ok &= check(hi.max_subdivision_level >= lo.max_subdivision_level, "subdivision cap must not drop");
            ok &= check(hi.target_edge_pixels <= lo.target_edge_pixels, "a higher tier must target smaller edges");
            ok &= check(hi.traversal_mip_bias <= lo.traversal_mip_bias, "a higher tier must not read coarser mips");
        }
        // The point of the ladder: even the bottom tier gets real view-dependent depth.
        ok &= check(profile_for_tier(HardwareTier::Low).max_algorithm >= HeightfieldAlgorithm::ParallaxOcclusion,
                    "Low tier must still get parallax occlusion");
        return ok;
    }

    DisplacementCapabilities mesh_capable() {
        DisplacementCapabilities c;
        c.mesh_shader = true;
        c.task_shader = true;
        c.max_mesh_output_vertices = 256;
        c.max_mesh_output_primitives = 256;
        c.max_mesh_payload_bytes = 16384;
        return c;
    }

    bool mesh_limits_bound_subdivision() {
        bool ok = true;
        ok &= check(max_mesh_shader_subdivision(DisplacementCapabilities::baseline()) == 0, "no mesh shaders -> level 0");
        ok &= check(max_mesh_shader_subdivision(mesh_capable()) == kMeshMaxSubdivisionLevel, "roomy limits -> shader max");

        DisplacementCapabilities tight = mesh_capable();
        tight.max_mesh_output_vertices = 32;
        tight.max_mesh_output_primitives = 32;
        // Level 5: 21 verts / 25 prims fits; level 6: 28 verts / 36 prims exceeds 32 primitives.
        ok &= check(max_mesh_shader_subdivision(tight) == 5, "primitive limit must bound the subdivision level");

        DisplacementCapabilities no_task = mesh_capable();
        no_task.task_shader = false;
        ok &= check(max_mesh_shader_subdivision(no_task) == 0, "the path needs the task stage too");

        DisplacementCapabilities small_payload = mesh_capable();
        small_payload.max_mesh_payload_bytes = kTaskPayloadBytes - 1;
        ok &= check(max_mesh_shader_subdivision(small_payload) == 0, "the path needs room for its payload");
        return ok;
    }

    bool planner_degrades_without_losing_the_silhouette() {
        bool ok = true;
        DisplacementRequest hero;
        hero.silhouette_critical = true;
        hero.desired_depth = DepthPolicy::DisplacedDepth;
        hero.desired_self_shadow = SelfShadowQuality::Reference;

        const auto modern = plan_displacement(HardwareTier::Ultra, mesh_capable(), hero);
        ok &= check(modern.geometry == GeometryPath::MeshShader, "silhouette-critical asset on Ultra+mesh uses mesh shaders");
        ok &= check(modern.algorithm == HeightfieldAlgorithm::NormalOnly,
                    "geometry path must not also project per pixel (double displacement)");
        ok &= check(modern.depth == DepthPolicy::BaseSurface, "real geometry needs no depth write");

        const auto old_hw = plan_displacement(HardwareTier::Ultra, DisplacementCapabilities::baseline(), hero);
        ok &= check(old_hw.geometry == GeometryPath::PreTessellatedVertex,
                    "without mesh shaders the silhouette falls back to the legacy vertex path, not to nothing");
        ok &= check(!old_hw.downgrades.empty(), "the fallback must be explained");

        const auto low = plan_displacement(HardwareTier::Low, DisplacementCapabilities::baseline(), hero);
        ok &= check(low.geometry == GeometryPath::None, "Low tier grants no geometry path");
        ok &= check(low.algorithm >= HeightfieldAlgorithm::ParallaxOcclusion, "...but still projects per pixel");
        ok &= check(low.self_shadow == SelfShadowQuality::Off, "Low tier has no self-shadow");
        ok &= check(low.depth == DepthPolicy::BaseSurface, "Low tier keeps base depth");

        // Baseline hardware at every tier must produce a plan that needs nothing beyond texture sampling.
        for (const HardwareTier tier : kAllHardwareTiers) {
            const auto plan = plan_displacement(tier, DisplacementCapabilities::baseline(), hero);
            ok &= check(plan.geometry != GeometryPath::MeshShader, "baseline device must never be planned mesh shaders");
        }
        return ok;
    }

    bool planner_projection_and_depth_rules() {
        bool ok = true;
        DisplacementRequest brick;
        brick.desired_algorithm = HeightfieldAlgorithm::HierarchicalCellExact;
        brick.desired_depth = DepthPolicy::DisplacedDepth;
        brick.desired_self_shadow = SelfShadowQuality::Production;

        const auto ultra = plan_displacement(HardwareTier::Ultra, DisplacementCapabilities::baseline(), brick);
        ok &= check(ultra.algorithm == HeightfieldAlgorithm::HierarchicalCellExact, "Ultra runs the hierarchical block");
        ok &= check(ultra.needs_hierarchy(), "...which needs a hierarchy resource");
        ok &= check(ultra.depth == DepthPolicy::DisplacedDepth, "Ultra writes displaced depth");
        ok &= check(ultra.geometry == GeometryPath::None, "no silhouette request, no geometry");

        const auto medium = plan_displacement(HardwareTier::Medium, DisplacementCapabilities::baseline(), brick);
        ok &= check(medium.algorithm == HeightfieldAlgorithm::CellExact, "Medium caps at cell-exact");
        ok &= check(!medium.needs_hierarchy(), "...which needs no hierarchy");
        ok &= check(medium.depth == DepthPolicy::BaseSurface, "Medium keeps early-Z");
        ok &= check(medium.self_shadow == SelfShadowQuality::Cheap, "Medium self-shadow is capped");

        DisplacementCapabilities no_depth = DisplacementCapabilities::baseline();
        no_depth.fragment_depth_write = false;
        ok &= check(plan_displacement(HardwareTier::Cinematic, no_depth, brick).depth == DepthPolicy::BaseSurface,
                    "a backend that cannot write depth vetoes DisplacedDepth");

        DisplacementRequest dynamic = brick;
        dynamic.dynamic_heightfield = true;
        ok &= check(plan_displacement(HardwareTier::High, DisplacementCapabilities::baseline(), dynamic).hierarchy_build ==
                        HierarchyBuild::Cpu,
                    "no storage-format support -> CPU hierarchy build");
        DisplacementCapabilities compute = DisplacementCapabilities::baseline();
        compute.compute_hierarchy_build = true;
        ok &= check(plan_displacement(HardwareTier::High, compute, dynamic).hierarchy_build == HierarchyBuild::Compute,
                    "dynamic heightfield + storage support -> GPU hierarchy build");
        return ok;
    }

    bool per_view_selection_never_exceeds_the_plan() {
        bool ok = true;
        DisplacementRequest brick;
        for (const HardwareTier tier : kAllHardwareTiers) {
            const auto plan = plan_displacement(tier, DisplacementCapabilities::baseline(), brick);
            for (const f32 size : {4.0f, 50.0f, 400.0f}) {
                for (const f32 cos_v : {0.05f, 0.5f, 1.0f}) {
                    for (const f32 amp : {0.1f, 1.0f, 8.0f}) {
                        for (const f32 tpp : {0.5f, 8.0f}) {
                            const DisplacementViewMetrics m{size, cos_v, amp, tpp};
                            const auto chosen = select_algorithm_for_view(plan, m);
                            ok &= check(chosen <= plan.algorithm, "per-view choice must not exceed the plan");
                            const u32 steps = select_traversal_steps(plan, m);
                            ok &= check(steps <= plan.max_traversal_steps && steps >= 1,
                                        "step budget within [1, plan ceiling]");
                            ok &= check(select_traversal_mip(plan, m, 6) < 6, "mip within the chain");
                        }
                    }
                }
            }
        }
        const auto ultra = plan_displacement(HardwareTier::Ultra, DisplacementCapabilities::baseline(), brick);
        ok &= check(select_algorithm_for_view(ultra, {400.0f, 1.0f, 0.2f, 1.0f}) == HeightfieldAlgorithm::NormalOnly,
                    "sub-pixel displacement is just a normal map");
        ok &= check(select_algorithm_for_view(ultra, {400.0f, 0.1f, 8.0f, 1.0f}) == HeightfieldAlgorithm::HierarchicalCellExact,
                    "large grazing surface with real amplitude uses the hierarchy");
        ok &= check(select_algorithm_for_view(ultra, {60.0f, 1.0f, 3.0f, 1.0f}) == HeightfieldAlgorithm::Parallax,
                    "a small surface gets the cheap block");
        // Grazing rays cross more cells and so keep more of the budget than head-on ones.
        ok &= check(select_traversal_steps(ultra, {400.0f, 0.05f, 4.0f, 1.0f}) >
                        select_traversal_steps(ultra, {400.0f, 1.0f, 4.0f, 1.0f}),
                    "grazing views keep more of the traversal budget");
        return ok;
    }

    // ---- Geometry path -----------------------------------------------------------------------------

    bool mesh_shader_constants_fit_every_implementation() {
        bool ok = true;
        // 64 vertices / 126 primitives is the portable mesh-shader output budget.
        ok &= check(subdivided_vertex_count(kMeshMaxSubdivisionLevel) <= 64, "max level must fit 64 vertices");
        ok &= check(subdivided_triangle_count(kMeshMaxSubdivisionLevel) <= 126, "max level must fit 126 primitives");
        ok &= check(subdivided_vertex_count(1) == 3 && subdivided_triangle_count(1) == 1, "level 1 is the triangle itself");
        ok &= check(subdivided_vertex_count(8) == 45 && subdivided_triangle_count(8) == 64, "documented level-8 counts");
        ok &= check(kTaskPayloadBytes == 260, "payload layout is count + 32 indices + 32 levels");
        return ok;
    }

    f32 triangle_area(const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c) {
        return 0.5f * glm::length(glm::cross(b - a, c - a));
    }

    bool subdivision_preserves_the_surface() {
        bool ok = true;
        std::vector<GeometryVertex> verts(3);
        verts[0].position = {0, 0, 0};
        verts[1].position = {2, 0, 0};
        verts[2].position = {0, 3, 0};
        verts[0].uv = {0, 0};
        verts[1].uv = {1, 0};
        verts[2].uv = {0, 1};
        for (auto &v : verts) {
            v.normal = {0, 0, 1};
        }
        const std::vector<u32> indices{0, 1, 2};

        for (const u32 level : {1u, 2u, 3u, 7u, 8u}) {
            const auto out = subdivide_uniform(verts, indices, level);
            const auto cost = subdivision_cost(1, level);
            ok &= check(out.vertices.size() == cost.vertex_count, "vertex count matches subdivision_cost");
            ok &= check(out.indices.size() == cost.index_count, "index count matches subdivision_cost");
            ok &= check(out.vertices.size() == subdivided_vertex_count(level), "vertex count matches the shader formula");

            f32 area = 0.0f;
            bool winding_ok = true;
            for (usize i = 0; i + 2 < out.indices.size(); i += 3) {
                const auto &a = out.vertices[out.indices[i]].position;
                const auto &b = out.vertices[out.indices[i + 1]].position;
                const auto &c = out.vertices[out.indices[i + 2]].position;
                area += triangle_area(a, b, c);
                winding_ok &= glm::cross(b - a, c - a).z > 0.0f; // source winding is counter-clockwise (+z)
            }
            ok &= check(near(area, 3.0f, 1.0e-4f), "subdivided triangles must tile the original area exactly");
            ok &= check(winding_ok, "subdivision must preserve winding");
            bool uv_ok = true;
            for (const auto &v : out.vertices) {
                uv_ok &= near(v.uv.x * 2.0f, v.position.x, 1.0e-4f) && near(v.uv.y * 3.0f, v.position.y, 1.0e-4f);
            }
            ok &= check(uv_ok, "uv must interpolate with position");
        }
        return ok;
    }

    bool subdivision_level_tracks_distance() {
        bool ok = true;
        TriangleLodInput in;
        in.p0 = {0, 0, 0};
        in.p1 = {1, 0, 0};
        in.p2 = {0, 1, 0};
        in.max_displacement = 0.1f;
        in.pixels_per_unit_at_one = 1000.0f;
        in.target_edge_pixels = 16.0f;

        u32 previous = kMeshMaxSubdivisionLevel + 1;
        for (const f32 z : {2.0f, 5.0f, 20.0f, 100.0f, 1000.0f}) {
            in.camera_position = {0.3f, 0.3f, z};
            const u32 level = select_subdivision_level(in);
            ok &= check(level >= 1 && level <= kMeshMaxSubdivisionLevel, "level within [1, max]");
            ok &= check(level <= previous, "farther triangles never get more subdivision");
            previous = level;
        }
        in.camera_position = {0.3f, 0.3f, 2.0f};
        ok &= check(select_subdivision_level(in) == kMeshMaxSubdivisionLevel, "a close large triangle refines fully");
        in.camera_position = {0.3f, 0.3f, 1.0e5f};
        ok &= check(select_subdivision_level(in) == 1, "a distant triangle is emitted as-is");
        in.camera_position = {0.3f, 0.3f, 0.0f}; // camera in the triangle's plane: distance term must not blow up
        ok &= check(select_subdivision_level(in) <= kMeshMaxSubdivisionLevel, "degenerate camera position stays finite");
        return ok;
    }


    bool edge_levels_are_symmetric_and_edges_watertight() {
        bool ok = true;
        TriangleLodInput params;
        params.max_displacement = 0.2f;
        params.pixels_per_unit_at_one = 900.0f;
        params.target_edge_pixels = 10.0f;
        params.camera_position = {0.4f, -0.3f, 3.0f};

        std::mt19937 rng(11);
        std::uniform_real_distribution<f32> coord(-2.0f, 2.0f);
        for (int trial = 0; trial < 300; ++trial) {
            const glm::vec3 a(coord(rng), coord(rng), coord(rng));
            const glm::vec3 b(coord(rng), coord(rng), coord(rng));
            ok &= check(select_edge_level(a, b, params) == select_edge_level(b, a, params),
                        "edge level must not depend on endpoint order (adjacent triangles must agree)");
        }

        // Triangle A owns edge (P, Q) as its v0->v1; triangle B lists the same edge reversed, as its v0->v1 =
        // (Q, P), with a different vertex order and a different interior level. Both must generate the same set
        // of points along it — bit for bit — or the surface cracks.
        for (int trial = 0; trial < 200; ++trial) {
            const glm::vec3 P(coord(rng), coord(rng), coord(rng));
            const glm::vec3 Q(coord(rng), coord(rng), coord(rng));
            const glm::vec3 RA(coord(rng), coord(rng), coord(rng));
            const glm::vec3 RB(coord(rng), coord(rng), coord(rng));
            const u32 shared_level = 1 + static_cast<u32>(rng() % 8);
            const u32 level_a = std::max(shared_level, 1 + static_cast<u32>(rng() % 8));
            const u32 level_b = std::max(shared_level, 1 + static_cast<u32>(rng() % 8));

            std::vector<glm::vec3> from_a;
            std::vector<glm::vec3> from_b;
            for (u32 col = 0; col <= level_a; ++col) {
                from_a.push_back(snapped_vertex_position(P, Q, RA, {shared_level, 8, 8}, level_a, 0, col));
            }
            for (u32 col = 0; col <= level_b; ++col) {
                from_b.push_back(snapped_vertex_position(Q, P, RB, {shared_level, 8, 8}, level_b, 0, col));
            }
            auto key = [](const glm::vec3 &v) { return std::tuple(v.x, v.y, v.z); };
            auto unique_sorted = [&](std::vector<glm::vec3> v) {
                std::sort(v.begin(), v.end(), [&](const auto &l, const auto &r) { return key(l) < key(r); });
                v.erase(std::unique(v.begin(), v.end()), v.end());
                return v;
            };
            const auto ua = unique_sorted(from_a);
            const auto ub = unique_sorted(from_b);
            ok &= check(ua == ub, "the two triangles sharing an edge must emit bit-identical points along it");
            ok &= check(ua.size() == shared_level + 1, "the shared edge is subdivided exactly at its own level");
        }

        // Corners are never moved by snapping.
        const glm::vec3 c0(0, 0, 0);
        const glm::vec3 c1(1, 0, 0);
        const glm::vec3 c2(0, 1, 0);
        ok &= check(snapped_vertex_position(c0, c1, c2, {2, 3, 4}, 8, 0, 0) == c0, "v0 stays put");
        ok &= check(snapped_vertex_position(c0, c1, c2, {2, 3, 4}, 8, 0, 8) == c1, "v1 stays put");
        ok &= check(snapped_vertex_position(c0, c1, c2, {2, 3, 4}, 8, 8, 0) == c2, "v2 stays put");
        return ok;
    }

    // ---- Hierarchy ---------------------------------------------------------------------------------

    bool hierarchy_bounds_are_conservative() {
        bool ok = true;
        for (const auto &[w, h] : {std::pair<u32, u32>{16, 16}, {13, 7}, {32, 8}, {5, 5}, {1, 4}}) {
            const Field f = make_noise_field(w, h, 1234u + w * 31u + h);
            const auto view = f.view();
            const auto hier = HeightfieldHierarchy::build(view);
            ok &= check(hier.valid(), "hierarchy builds for a valid field");
            ok &= check(hier.level_width(hier.level_count() - 1) == 1 && hier.level_height(hier.level_count() - 1) == 1,
                        "top level is a single node");

            std::mt19937 rng(99);
            std::uniform_real_distribution<f32> dist(0.0f, 1.0f);
            bool bounded = true;
            for (int i = 0; i < 4000; ++i) {
                const f32 u = dist(rng);
                const f32 v = dist(rng);
                const f32 height = sample_height(view, u, v);
                // Which level-0 cell owns this point, then check its ancestors at every level.
                const f32 x = u * static_cast<f32>(w) - 0.5f;
                const f32 y = v * static_cast<f32>(h) - 0.5f;
                auto wrap = [](i32 a, i32 n) { return ((a % n) + n) % n; };
                const i32 cx = wrap(static_cast<i32>(std::floor(x)), static_cast<i32>(w));
                const i32 cy = wrap(static_cast<i32>(std::floor(y)), static_cast<i32>(h));
                for (u32 level = 0; level < hier.level_count(); ++level) {
                    const auto &n = hier.node(level, static_cast<u32>(cx) >> level, static_cast<u32>(cy) >> level);
                    bounded &= height >= n.min_height - 1.0e-6f && height <= n.max_height + 1.0e-6f;
                }
            }
            ok &= check(bounded, "every level's node must bound the heights inside its region");
        }
        return ok;
    }

    bool hierarchy_incremental_update_matches_rebuild() {
        bool ok = true;
        for (const bool wrap : {true, false}) {
            for (const auto &[w, h] : {std::pair<u32, u32>{16, 16}, {13, 9}}) {
                Field f = make_noise_field(w, h, 7);
                f.wrap = wrap;
                auto hier = HeightfieldHierarchy::build(f.view());

                std::mt19937 rng(42);
                for (int edit = 0; edit < 40; ++edit) {
                    const u32 x0 = static_cast<u32>(rng() % w);
                    const u32 y0 = static_cast<u32>(rng() % h);
                    const u32 x1 = std::min(w, x0 + 1 + static_cast<u32>(rng() % 4));
                    const u32 y1 = std::min(h, y0 + 1 + static_cast<u32>(rng() % 4));
                    // Include seam-touching edits explicitly: the wrap border is where the -1 alias lives.
                    const u32 ex0 = edit % 5 == 0 ? 0 : x0;
                    const u32 ey0 = edit % 7 == 0 ? 0 : y0;
                    for (u32 y = ey0; y < y1; ++y) {
                        for (u32 x = ex0; x < x1; ++x) {
                            f.heights[static_cast<usize>(y) * w + x] = static_cast<f32>(rng() % 1000) / 1000.0f;
                        }
                    }
                    hier.update_region(f.view(), ex0, ey0, x1, y1);
                    const auto rebuilt = HeightfieldHierarchy::build(f.view());

                    bool same = hier.level_count() == rebuilt.level_count();
                    for (u32 lv = 0; same && lv < rebuilt.level_count(); ++lv) {
                        for (u32 y = 0; y < rebuilt.level_height(lv); ++y) {
                            for (u32 x = 0; x < rebuilt.level_width(lv); ++x) {
                                const auto &a = hier.node(lv, x, y);
                                const auto &b = rebuilt.node(lv, x, y);
                                same &= a.min_height == b.min_height && a.max_height == b.max_height;
                            }
                        }
                    }
                    if (!check(same, "update_region must equal a full rebuild")) {
                        return false;
                    }
                }
            }
        }
        return ok;
    }

    bool hierarchy_packing_is_conservative_and_sized() {
        bool ok = true;
        for (const auto &[w, h] : {std::pair<u32, u32>{16, 16}, {13, 7}, {64, 32}}) {
            const Field f = make_noise_field(w, h, 5);
            const auto hier = HeightfieldHierarchy::build(f.view());
            for (const auto precision : {HierarchyPrecision::Unorm16, HierarchyPrecision::Float32}) {
                for (const auto channels : {HierarchyChannels::MaxOnly, HierarchyChannels::MinMax}) {
                    const auto packed = hier.pack(precision, channels);
                    ok &= check(packed.data.size() == hierarchy_memory_bytes(w, h, precision, channels),
                                "hierarchy_memory_bytes must equal the packed size");
                    ok &= check(packed.mip_offsets.size() == packed.mip_count, "one offset per mip");
                    ok &= check(packed.mip_count + 1 >= hier.level_count(), "the packed chain covers levels 1..top");

                    bool conservative = true;
                    for (u32 lv = 1; lv < hier.level_count(); ++lv) {
                        const u32 mip = lv - 1; // level 0 is not packed
                        const u32 mip_w = std::max(1u, packed.width >> mip);
                        for (u32 y = 0; y < hier.level_height(lv); ++y) {
                            for (u32 x = 0; x < hier.level_width(lv); ++x) {
                                const auto &n = hier.node(lv, x, y);
                                const u8 *texel = packed.data.data() + packed.mip_offsets[mip] +
                                                  (static_cast<usize>(y) * mip_w + x) * packed.channels * packed.bytes_per_channel;
                                auto read = [&](u32 channel) {
                                    if (precision == HierarchyPrecision::Float32) {
                                        f32 v;
                                        std::memcpy(&v, texel + channel * 4, 4);
                                        return v;
                                    }
                                    u16 q;
                                    std::memcpy(&q, texel + channel * 2, 2);
                                    return static_cast<f32>(q) / 65535.0f;
                                };
                                if (channels == HierarchyChannels::MinMax) {
                                    conservative &= read(0) <= n.min_height + 1.0e-7f;
                                    conservative &= read(1) >= n.max_height - 1.0e-7f;
                                } else {
                                    conservative &= read(0) >= n.max_height - 1.0e-7f;
                                }
                            }
                        }
                    }
                    ok &= check(conservative, "quantization must round min down and max up");
                }
            }
        }
        // A 4K hierarchy fits the budgets the plan doc quotes.
        ok &= check(hierarchy_memory_bytes(4096, 4096, HierarchyPrecision::Unorm16, HierarchyChannels::MaxOnly) <
                        12ull * 1024 * 1024,
                    "4K R16 max-only hierarchy (levels 1..) should be ~10.7 MiB");
        return ok;
    }

    // ---- Exact solver and traversal ---------------------------------------------------------------

    bool cell_solver_matches_analytic_cases() {
        bool ok = true;
        // Flat patch at 0.5, ray descending from 1.0 with slope -1 per unit: crosses at s = 0.5.
        const auto flat = solve_bilinear_cell(0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f, -1.0f, 2.0f);
        ok &= check(flat && near(flat->s, 0.5f, 1.0e-5f), "flat patch: linear solve");
        ok &= check(!solve_bilinear_cell(0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f, -1.0f, 0.4f),
                    "flat patch: no hit when the segment ends first");
        ok &= check(!solve_bilinear_cell(0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f, +1.0f, 5.0f),
                    "flat patch: an ascending ray never hits");
        const auto inside = solve_bilinear_cell(0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.25f, 0.0f, 0.0f, -1.0f, 1.0f);
        ok &= check(inside && inside->s == 0.0f, "starting under the surface hits immediately");

        // Saddle: H = fu*fv (h11 = 1, others 0). Ray along fu = fv = s, height 0.9 - 0.1 s. Surface s^2
        // meets ray where s^2 + 0.1 s - 0.9 = 0 -> s = (-0.1 + sqrt(0.01 + 3.6)) / 2.
        const f32 expected = (-0.1f + std::sqrt(0.01f + 3.6f)) * 0.5f;
        const auto saddle = solve_bilinear_cell(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.9f, 1.0f, 1.0f, -0.1f, 1.0f);
        ok &= check(saddle && near(saddle->s, expected, 1.0e-4f), "saddle: quadratic root");
        ok &= check(saddle && saddle->residual < 1.0e-4f, "saddle: residual near zero");

        // A ray that enters above the surface, dips below and comes back out inside one segment still
        // reports the *first* crossing: H = fu (h10 = h11 = 1); ray starts at h=0.9, fu goes 0->1.
        const auto first = solve_bilinear_cell(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.5f, 0.9f, 1.0f, 0.0f, -0.6f, 1.0f);
        ok &= check(first && near(first->s, 0.9f / 1.6f, 1.0e-4f), "linear-in-fu case");
        return ok;
    }

    struct TraceStats {
        u32 rays = 0;
        u32 agree = 0;
        u32 exact_earlier = 0;   // exact hit before brute force's — legitimately finds thin features it steps over
        u32 disagree = 0;
        u64 steps_cell = 0;
        u64 steps_hier = 0;
        u32 hier_vs_cell_mismatch = 0;
    };

    TraceStats compare_traces(const Field &f, u32 ray_count, u32 seed, f32 height_scale, const glm::vec2 &tile_size) {
        const auto view = f.view();
        const auto hier = HeightfieldHierarchy::build(view);
        TraceStats stats;
        std::mt19937 rng(seed);
        std::uniform_real_distribution<f32> unit(0.0f, 1.0f);
        for (u32 i = 0; i < ray_count; ++i) {
            const glm::vec2 uv(unit(rng), unit(rng));
            // Random view direction over the hemisphere, biased toward grazing to stress traversal.
            const f32 phi = unit(rng) * 6.2831853f;
            const f32 cos_theta = 0.12f + 0.88f * unit(rng);
            const f32 sin_theta = std::sqrt(1.0f - cos_theta * cos_theta);
            const glm::vec3 view_ts(sin_theta * std::cos(phi), sin_theta * std::sin(phi), cos_theta);
            const HeightfieldRay ray = make_view_ray(uv, view_ts, height_scale, tile_size, 1.0f);

            const auto exact = trace_cell_exact(view, ray, 4096);
            const auto fast = trace_hierarchical(view, hier, ray, 4096);
            const auto truth = trace_brute_force(view, ray, 32768);
            ++stats.rays;
            stats.steps_cell += exact.steps;
            stats.steps_hier += fast.steps;

            if (exact.status != fast.status || (exact.status == HitStatus::Hit && !near(exact.t, fast.t, 1.0e-3f))) {
                ++stats.hier_vs_cell_mismatch;
            }
            if (exact.status != HitStatus::Hit) {
                ++stats.disagree;
                continue;
            }
            if (truth.status != HitStatus::Hit) {
                // A grazing ray can dip below the surface for less than one brute-force step. The exact
                // solve's own residual says whether its hit is real.
                if (exact.residual < 1.0e-4f) {
                    ++stats.exact_earlier;
                } else {
                    ++stats.disagree;
                }
                continue;
            }
            const f32 tolerance = 2.0e-3f * (1.0f + truth.t);
            if (near(exact.t, truth.t, tolerance)) {
                ++stats.agree;
            } else if (exact.t < truth.t) {
                ++stats.exact_earlier;
            } else {
                ++stats.disagree; // exact reporting a *later* hit than brute force would mean it stepped over the surface
            }
        }
        return stats;
    }

    bool exact_traversal_matches_ground_truth() {
        bool ok = true;
        const glm::vec2 tile(1.0f, 1.0f);

        const Field smooth = make_rolling_field(64, 64, 0.9f);
        const TraceStats s = compare_traces(smooth, 600, 1, 0.25f, tile);
        ok &= check(s.disagree == 0, "smooth field: exact solve never contradicts the brute-force hit");
        ok &= check(s.agree >= s.rays * 95 / 100, "smooth field: exact solve agrees with ground truth almost everywhere");
        ok &= check(s.hier_vs_cell_mismatch == 0, "smooth field: hierarchy must not change the result");

        const Field noisy = make_noise_field(32, 32, 77);
        const TraceStats n = compare_traces(noisy, 600, 2, 0.35f, tile);
        ok &= check(n.disagree == 0, "noisy field: exact solve never contradicts the brute-force hit");
        ok &= check(n.hier_vs_cell_mismatch == 0, "noisy field: hierarchy must not change the result");

        // Non-square, non-power-of-two, non-unit tile: exercises the clipped last node and the aspect maths.
        const Field odd = make_noise_field(21, 10, 5);
        const TraceStats o = compare_traces(odd, 600, 3, 0.2f, glm::vec2(2.0f, 0.5f));
        ok &= check(o.disagree == 0, "odd-sized field: exact solve never contradicts the brute-force hit");
        ok &= check(o.hier_vs_cell_mismatch == 0, "odd-sized field: hierarchy must not change the result");

        const Field sparse = make_sparse_field(256, 256);
        const TraceStats sp = compare_traces(sparse, 400, 4, 0.3f, tile);
        ok &= check(sp.hier_vs_cell_mismatch == 0, "sparse field: hierarchy must not change the result");
        ok &= check(sp.steps_hier * 2 < sp.steps_cell,
                    "sparse field: hierarchical skipping must at least halve traversal steps versus the plain DDA");
        return ok;
    }

    bool pom_can_miss_what_exact_finds() {
        bool ok = true;
        // A single-texel spike in an otherwise low field, seen at a grazing angle. Coarse POM steps over it.
        Field f{{}, 64, 64, true};
        f.heights.assign(64 * 64, 0.05f);
        f.heights[32 * 64 + 32] = 1.0f;
        const auto view = f.view();
        const auto hier = HeightfieldHierarchy::build(view);

        u32 pom_miss = 0;
        u32 exact_found = 0;
        for (int i = 0; i < 64; ++i) {
            // Rays descend across the spike (texel 32, whose centre is u = v = 32.5/64) at heights
            // around 0.55-0.65, inside the tent that the neighbouring low texels leave.
            const glm::vec2 uv(0.15f + 0.0007f * static_cast<f32>(i), 32.5f / 64.0f);
            const HeightfieldRay ray = make_view_ray(uv, glm::normalize(glm::vec3(-1.0f, 0.0f, 0.35f)), 0.3f,
                                                     glm::vec2(1.0f), 1.0f);
            const auto exact = trace_hierarchical(view, hier, ray, 4096);
            const auto truth = trace_brute_force(view, ray, 65536);
            const auto pom = trace_parallax_occlusion(view, ray, 16);
            if (truth.status == HitStatus::Hit && truth.uv.x > 0.45f && truth.uv.x < 0.55f && exact.status == HitStatus::Hit &&
                near(exact.t, truth.t, 5.0e-3f)) {
                ++exact_found;
                if (pom.status != HitStatus::Hit || std::abs(pom.t - truth.t) > 5.0e-3f) {
                    ++pom_miss;
                }
            }
        }
        ok &= check(exact_found > 0, "fixture must actually put rays on the spike");
        ok &= check(pom_miss > 0, "16-step POM should miss a one-texel spike that the exact solve finds");
        return ok;
    }

    bool wrap_and_clamp_addressing() {
        bool ok = true;
        // Wrapping field: a ray that runs off the right edge keeps going into the repeated tile.
        Field wrap = make_rolling_field(32, 32, 0.8f);
        const auto view = wrap.view();
        const auto hier = HeightfieldHierarchy::build(view);
        const HeightfieldRay grazing = make_view_ray({0.9f, 0.5f}, glm::normalize(glm::vec3(-0.95f, 0.0f, 0.25f)), 0.2f,
                                                     glm::vec2(1.0f), 1.0f);
        const auto hit = trace_hierarchical(view, hier, grazing, 4096);
        const auto truth = trace_brute_force(view, grazing, 65536);
        ok &= check(hit.status == HitStatus::Hit && truth.status == HitStatus::Hit, "wrapping ray finds a surface");
        ok &= check(hit.status == HitStatus::Hit && near(hit.t, truth.t, 3.0e-3f * (1.0f + truth.t)),
                    "wrapping traversal agrees with brute force across the seam");

        // Non-wrapping field: leaving the texel-centre rectangle is a miss, not a wrapped hit.
        Field clamp = make_rolling_field(32, 32, 0.8f);
        clamp.wrap = false;
        const auto cview = clamp.view();
        const HeightfieldRay outward = make_view_ray({0.98f, 0.5f}, glm::normalize(glm::vec3(-0.999f, 0.0f, 0.04f)), 0.05f,
                                                     glm::vec2(1.0f), 1.0f);
        ok &= check(trace_cell_exact(cview, outward, 4096).status != HitStatus::BudgetExhausted,
                    "clamped traversal terminates");
        const HeightfieldRay away({2.0f, 0.5f, 1.0f}, {1.0f, 0.0f, -0.1f});
        ok &= check(trace_cell_exact(cview, away, 4096).status == HitStatus::Miss, "a ray starting outside a clamped field misses");
        return ok;
    }

    bool budget_exhaustion_is_reported() {
        bool ok = true;
        const Field f = make_noise_field(64, 64, 3);
        const auto view = f.view();
        const HeightfieldRay ray = make_view_ray({0.3f, 0.3f}, glm::normalize(glm::vec3(0.9f, 0.1f, 0.2f)), 0.1f,
                                                 glm::vec2(1.0f), 1.0f);
        const auto starved = trace_cell_exact(view, ray, 2);
        ok &= check(starved.status == HitStatus::BudgetExhausted, "a 2-step budget on a grazing ray runs out");
        ok &= check(starved.steps == 2, "and reports the steps it spent");
        ok &= check(std::isfinite(starved.uv.x) && std::isfinite(starved.residual), "with a finite best-effort position");
        return ok;
    }

    bool normals_and_self_shadow() {
        bool ok = true;
        // Height ramp along u: H = u. dH/du = 1, so with scale 0.5 over a unit tile the slope is 0.5.
        Field ramp{{}, 64, 4, false};
        ramp.heights.resize(64 * 4);
        for (u32 y = 0; y < 4; ++y) {
            for (u32 x = 0; x < 64; ++x) {
                ramp.heights[y * 64 + x] = static_cast<f32>(x) / 63.0f;
            }
        }
        const auto view = ramp.view();
        const glm::vec2 grad = sample_gradient(view, 0.5f, 0.5f);
        ok &= check(near(grad.x, 64.0f / 63.0f, 1.0e-3f) && near(grad.y, 0.0f, 1.0e-4f), "gradient of a linear ramp");
        const glm::vec3 n = heightfield_normal(grad, 0.5f, glm::vec2(1.0f));
        ok &= check(n.x < 0.0f && near(n.y, 0.0f, 1.0e-4f) && near(glm::length(n), 1.0f, 1.0e-4f),
                    "normal tilts away from the rising side and is unit length");
        ok &= check(near(n.x / n.z, -0.5f * 64.0f / 63.0f, 1.0e-3f), "normal slope equals scale * dH/du");

        // Self shadow: a wall at u ~ 0.5 in a wrapping field.
        Field wall{{}, 64, 8, true};
        wall.heights.assign(64 * 8, 0.1f);
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 30; x < 34; ++x) {
                wall.heights[y * 64 + x] = 0.9f;
            }
        }
        const auto wview = wall.view();
        const auto whier = HeightfieldHierarchy::build(wview);
        const glm::vec2 tile(1.0f);
        const glm::vec3 low_light = glm::normalize(glm::vec3(1.0f, 0.0f, 0.4f)); // toward +u, low sun
        // Point west of the wall, sun to the east: blocked. Point east of the wall, same sun: free.
        ok &= check(trace_shadow(wview, whier, {0.3f, 0.5f}, 0.1f, low_light, 0.3f, tile, 512),
                    "a point behind a wall (toward the light) is shadowed");
        ok &= check(!trace_shadow(wview, whier, {0.7f, 0.5f}, 0.1f, low_light, 0.3f, tile, 512),
                    "a point past the wall (light side) is lit");
        ok &= check(!trace_shadow(wview, whier, {0.3f, 0.5f}, 0.1f, glm::vec3(0.0f, 0.0f, 1.0f), 0.3f, tile, 512),
                    "overhead light is never blocked by a wall beside the point");
        ok &= check(trace_shadow(wview, whier, {0.3f, 0.5f}, 0.1f, glm::vec3(0.0f, 0.0f, -1.0f), 0.3f, tile, 512),
                    "a light below the base surface is always blocked");
        return ok;
    }

    // ---- Ray-tracing slot planning -----------------------------------------------------------------

    DisplacementCapabilities ray_tracing_capable() {
        DisplacementCapabilities c = mesh_capable();
        c.acceleration_structures = true;
        c.ray_query = true;
        c.procedural_primitives = true;
        return c;
    }

    bool ray_tracing_ladder_and_downgrades() {
        bool ok = true;
        // Monotone across tiers.
        for (usize i = 1; i < kAllHardwareTiers.size(); ++i) {
            ok &= check(profile_for_tier(kAllHardwareTiers[i]).max_ray_tracing >=
                            profile_for_tier(kAllHardwareTiers[i - 1]).max_ray_tracing,
                        "tier ray-tracing ceiling must not drop as the tier rises");
        }
        DisplacementRequest ask;
        ask.desired_ray_tracing = RayTracingPath::ProjectivePrism;

        // Opt-in: an ordinary request never pays for ray tracing.
        ok &= check(plan_displacement(HardwareTier::Cinematic, ray_tracing_capable(), DisplacementRequest{}).ray_tracing ==
                        RayTracingPath::None,
                    "ray tracing is opt-in");

        const DisplacementPlan full = plan_displacement(HardwareTier::Ultra, ray_tracing_capable(), ask);
        ok &= check(full.ray_tracing == RayTracingPath::ProjectivePrism, "capable device + Ultra gets the prism BVH");
        ok &= check(full.needs_prism_acceleration_structure(), "prism plan needs the prism acceleration structure");
        ok &= check(full.needs_hierarchy(), "a ray-tracing plan needs the hierarchy for the trace inside each prism");

        // Same request, no hardware RT (WebGPU/older GPU): software rung, recorded, never None.
        const DisplacementPlan soft = plan_displacement(HardwareTier::Ultra, mesh_capable(), ask);
        ok &= check(soft.ray_tracing == RayTracingPath::SoftwareHeightfield, "no RT hardware -> software heightfield trace");
        ok &= check(!soft.needs_prism_acceleration_structure(), "software rung builds no acceleration structure");
        bool noted = false;
        for (const PlanDowngrade &d : soft.downgrades) {
            noted |= d.what.find("ProjectivePrism -> SoftwareHeightfield") != string::npos;
        }
        ok &= check(noted, "the prism -> software downgrade is recorded");

        // Each missing capability alone forces the fallback.
        for (int missing = 0; missing < 3; ++missing) {
            DisplacementCapabilities c = ray_tracing_capable();
            (missing == 0 ? c.acceleration_structures : missing == 1 ? c.ray_query : c.procedural_primitives) = false;
            ok &= check(plan_displacement(HardwareTier::Cinematic, c, ask).ray_tracing == RayTracingPath::SoftwareHeightfield,
                        "each of AS / ray query / procedural primitives is required for the prism path");
        }

        // No compute at all: base triangle.
        DisplacementCapabilities none = DisplacementCapabilities::baseline();
        none.compute_shader = false;
        ok &= check(plan_displacement(HardwareTier::Ultra, none, ask).ray_tracing == RayTracingPath::None,
                    "no compute -> secondary rays see the base triangle");
        ok &= check(static_cast<bool>(DisplacementCapabilities::baseline().compute_shader),
                    "baseline still has compute (universal post-2019)");

        // Tier caps.
        ok &= check(plan_displacement(HardwareTier::Low, ray_tracing_capable(), ask).ray_tracing == RayTracingPath::None,
                    "Low tier: base triangle");
        ok &= check(plan_displacement(HardwareTier::Medium, ray_tracing_capable(), ask).ray_tracing ==
                        RayTracingPath::SoftwareHeightfield,
                    "Medium tier caps at the software rung even on RT hardware");

        // Independent of the geometry path.
        DisplacementRequest both = ask;
        both.silhouette_critical = true;
        const DisplacementPlan mesh_and_rt = plan_displacement(HardwareTier::Ultra, ray_tracing_capable(), both);
        ok &= check(mesh_and_rt.geometry == GeometryPath::MeshShader && mesh_and_rt.ray_tracing == RayTracingPath::ProjectivePrism,
                    "ray tracing is planned independently of the geometry path");
        return ok;
    }

} // namespace

int main() {
    struct Case {
        const char *name;
        bool (*run)();
    };
    const Case cases[] = {
        {"tier profiles monotone", tier_profiles_are_monotone},
        {"mesh limits bound subdivision", mesh_limits_bound_subdivision},
        {"planner degrades without losing silhouette", planner_degrades_without_losing_the_silhouette},
        {"planner projection/depth rules", planner_projection_and_depth_rules},
        {"per-view selection", per_view_selection_never_exceeds_the_plan},
        {"mesh shader constants", mesh_shader_constants_fit_every_implementation},
        {"subdivision preserves surface", subdivision_preserves_the_surface},
        {"subdivision level tracks distance", subdivision_level_tracks_distance},
        {"edge levels symmetric, edges watertight", edge_levels_are_symmetric_and_edges_watertight},
        {"hierarchy bounds conservative", hierarchy_bounds_are_conservative},
        {"hierarchy incremental update", hierarchy_incremental_update_matches_rebuild},
        {"hierarchy packing", hierarchy_packing_is_conservative_and_sized},
        {"cell solver analytic", cell_solver_matches_analytic_cases},
        {"exact traversal vs ground truth", exact_traversal_matches_ground_truth},
        {"POM misses thin spike", pom_can_miss_what_exact_finds},
        {"wrap and clamp addressing", wrap_and_clamp_addressing},
        {"budget exhaustion", budget_exhaustion_is_reported},
        {"normals and self shadow", normals_and_self_shadow},
        {"ray tracing planning", ray_tracing_ladder_and_downgrades},
    };
    for (const Case &c : cases) {
        const int before = g_failures;
        const bool ok = c.run();
        std::cout << (ok && g_failures == before ? "ok   " : "FAIL ") << c.name << '\n';
    }
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
