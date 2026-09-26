#include <Renderer/Displacement/DisplacementBinning.hpp>
#include <Renderer/Displacement/DisplacementPlanner.hpp>
#include <Renderer/Displacement/DisplacementWeld.hpp>
#include <Renderer/Displacement/HeightfieldGather.hpp>
#include <Renderer/Displacement/HeightfieldHierarchy.hpp>
#include <Renderer/Displacement/HeightfieldTrace.hpp>
#include <Renderer/Displacement/HierarchyGpuBuild.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

#include <glm/geometric.hpp>

// CPU tests for the displacement optimizations (plans/displacement-system.md section 7): corner-gather
// model, traversal start level, draw binning, self-shadow budgets, the GPU hierarchy build's pass planner
// (via its line-for-line CPU model), and the welded displacement-normal channel. Separate from
// DisplacementTest so the two can evolve independently.

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

    struct Field {
        std::vector<f32> heights;
        u32 width = 0;
        u32 height = 0;
        bool wrap = true;
        [[nodiscard]] HeightfieldView view() const { return HeightfieldView{heights, width, height, wrap}; }
    };

    Field make_noise(u32 w, u32 h, u32 seed, bool wrap = true) {
        Field f{{}, w, h, wrap};
        f.heights.resize(static_cast<usize>(w) * h);
        std::mt19937 rng(seed);
        std::uniform_real_distribution<f32> dist(0.0f, 1.0f);
        for (f32 &v : f.heights) {
            v = dist(rng);
        }
        return f;
    }

    Field make_sparse(u32 w, u32 h, bool wrap = true) {
        Field f{{}, w, h, wrap};
        f.heights.assign(static_cast<usize>(w) * h, 0.1f);
        for (u32 i = 0; i < 6; ++i) {
            f.heights[static_cast<usize>((i * 53 + 5) % h) * w + (i * 37 + 11) % w] = 0.95f;
        }
        return f;
    }

    // ---- Corner gather ---------------------------------------------------------------------------

    bool gather_component_order_and_parity() {
        bool ok = true;
        Field f{{}, 4, 3, true};
        for (u32 i = 0; i < 12; ++i) {
            f.heights.push_back(static_cast<f32>(i) / 16.0f);
        }
        // Gather at the corner shared by texels (1,1), (2,1), (1,2), (2,2): cell (1,1).
        const Gather4 g = gather_red(f.view(), 2.0f / 4.0f, 2.0f / 3.0f, SamplerAddress::Repeat);
        ok &= check(g.w == f.heights[1 * 4 + 1] && g.z == f.heights[1 * 4 + 2], "gather w=(i0,j0), z=(i1,j0)");
        ok &= check(g.x == f.heights[2 * 4 + 1] && g.y == f.heights[2 * 4 + 2], "gather x=(i0,j1), y=(i1,j1)");
        const CellTexels ct = load_cell_via_gather(f.view(), 1, 1, SamplerAddress::Repeat);
        const CellTexels direct = load_cell_direct(f.view(), 1, 1);
        ok &= check(ct.h00 == direct.h00 && ct.h10 == direct.h10 && ct.h01 == direct.h01 && ct.h11 == direct.h11,
                    "gather unpacks to h00/h10/h01/h11 in the trace code's naming");

        bool naive_diverges = false;
        for (const bool wrap : {true, false}) {
            for (const auto &dims : {std::pair<u32, u32>{16, 16}, {13, 7}, {2, 2}, {1, 5}}) {
                Field n = make_noise(dims.first, dims.second, 7u + dims.first, wrap);
                for (const SamplerAddress sampler : {SamplerAddress::Repeat, SamplerAddress::ClampToEdge}) {
                    const bool sampler_matches_view = (sampler == SamplerAddress::Repeat) == wrap;
                    for (i32 cy = -1; cy <= static_cast<i32>(n.height); ++cy) {
                        for (i32 cx = -1; cx <= static_cast<i32>(n.width); ++cx) {
                            const CellTexels a = load_cell_via_gather(n.view(), cx, cy, sampler);
                            const CellTexels b = load_cell_direct(n.view(), cx, cy);
                            if (!(a.h00 == b.h00 && a.h10 == b.h10 && a.h01 == b.h01 && a.h11 == b.h11)) {
                                ok &= check(false, "interior-gather rule must equal four Loads for any sampler mode");
                            }
                            const CellTexels c = load_cell_naive_gather(n.view(), cx, cy, sampler);
                            const bool same = c.h00 == b.h00 && c.h10 == b.h10 && c.h01 == b.h01 && c.h11 == b.h11;
                            if (!sampler_matches_view && !same) {
                                naive_diverges = true;
                            }
                            if (sampler_matches_view && cx >= 0 && cy >= 0 && cx < static_cast<i32>(n.width) &&
                                cy < static_cast<i32>(n.height) && !same) {
                                ok &= check(false, "naive gather is right when the sampler matches the view");
                            }
                        }
                    }
                }
            }
        }
        ok &= check(naive_diverges, "gathering everywhere and trusting the sampler must be shown to diverge");
        return ok;
    }

    // ---- Start level -----------------------------------------------------------------------------

    bool start_level_never_changes_the_hit() {
        bool ok = true;
        std::mt19937 rng(99);
        std::uniform_real_distribution<f32> uni(0.0f, 1.0f);
        u64 steps_top = 0;
        u64 steps_low = 0;
        u32 differing_step_counts = 0;
        u32 compared = 0;

        std::vector<Field> fields;
        fields.push_back(make_noise(32, 32, 1));
        fields.push_back(make_sparse(64, 48));
        fields.push_back(make_noise(37, 23, 2));         // non power of two
        fields.push_back(make_sparse(64, 64, false));    // clamped
        fields.push_back(make_noise(29, 41, 3, false));

        for (const Field &f : fields) {
            const HeightfieldView view = f.view();
            const HeightfieldHierarchy hier = HeightfieldHierarchy::build(view);
            const u32 top = hier.level_count() - 1;
            for (u32 i = 0; i < 300; ++i) {
                const glm::vec2 uv(0.1f + 0.8f * uni(rng), 0.1f + 0.8f * uni(rng));
                glm::vec3 view_ts(uni(rng) * 2.0f - 1.0f, uni(rng) * 2.0f - 1.0f, 0.15f + 0.85f * uni(rng));
                view_ts = glm::normalize(view_ts);
                const HeightfieldRay ray = make_view_ray(uv, view_ts, 0.3f, glm::vec2(1.0f), 1.0f);
                const HeightfieldHit ref = trace_hierarchical(view, hier, ray, 8192);
                if (ref.status == HitStatus::BudgetExhausted) {
                    continue;
                }
                for (u32 start = 0; start <= top; ++start) {
                    const HeightfieldHit h = trace_hierarchical(view, hier, ray, 8192, start);
                    ++compared;
                    const bool same_status = h.status == ref.status;
                    ok &= check(same_status, "start level changed hit/miss");
                    if (same_status && ref.status == HitStatus::Hit) {
                        ok &= check(std::abs(h.t - ref.t) <= 2.0e-3f * std::max(1.0f, std::abs(ref.t)) &&
                                        glm::distance(h.uv, ref.uv) <= 2.0e-3f,
                                    "start level moved the hit point");
                    }
                    if (start == 0) {
                        steps_low += h.steps;
                        steps_top += ref.steps;
                        differing_step_counts += h.steps != ref.steps ? 1u : 0u;
                    }
                }
                // Out-of-range start clamps to the top level.
                ok &= check(trace_hierarchical(view, hier, ray, 8192, 1000).steps == ref.steps,
                            "start level past the top clamps to the top");
                ok &= check(trace_hierarchical(view, hier, ray, 8192, kStartAtTopLevel).steps == ref.steps,
                            "kStartAtTopLevel is the top level");
            }
        }
        ok &= check(compared > 1000, "start-level comparison must cover many rays");
        ok &= check(differing_step_counts > 0, "start level must actually affect the step count somewhere");
        std::cout << "     start-level: " << compared << " comparisons, steps top=" << steps_top
                  << " level0=" << steps_low << '\n';
        return ok;
    }

    bool shadow_start_level_never_changes_the_answer() {
        bool ok = true;
        Field f = make_sparse(64, 64);
        const HeightfieldView view = f.view();
        const HeightfieldHierarchy hier = HeightfieldHierarchy::build(view);
        std::mt19937 rng(5);
        std::uniform_real_distribution<f32> uni(0.0f, 1.0f);
        for (u32 i = 0; i < 400; ++i) {
            const glm::vec2 uv(uni(rng), uni(rng));
            const glm::vec3 light = glm::normalize(glm::vec3(uni(rng) - 0.5f, uni(rng) - 0.5f, 0.2f + uni(rng)));
            const bool ref = trace_shadow(view, hier, uv, 0.1f, light, 0.3f, glm::vec2(1.0f), 4096);
            for (u32 start = 0; start < hier.level_count(); ++start) {
                ok &= check(trace_shadow(view, hier, uv, 0.1f, light, 0.3f, glm::vec2(1.0f), 4096, 2.0e-3f, start) == ref,
                            "shadow answer depends on start level");
            }
        }
        return ok;
    }

    bool planner_start_level_and_budgets() {
        bool ok = true;
        DisplacementViewMetrics m;
        m.texels_per_pixel = 8.0f;
        DisplacementPlan plan;
        plan.traversal_mip_bias = 0.0f;
        ok &= check(select_traversal_start_level(plan, m, 7) == 6, "default plan starts at the top level (baseline)");
        plan.start_level_from_footprint = true;
        ok &= check(select_traversal_start_level(plan, m, 7) == select_traversal_mip(plan, m, 7),
                    "opted-in plan feeds select_traversal_mip into the start level");
        ok &= check(select_traversal_start_level(plan, m, 7) == 3, "8 texels/pixel starts at level 3");
        m.texels_per_pixel = 0.1f;
        ok &= check(select_traversal_start_level(plan, m, 7) == 0, "sub-texel footprint starts at level 0");
        m.texels_per_pixel = 1.0e6f;
        ok &= check(select_traversal_start_level(plan, m, 7) == 6, "start level clamps to the top");
        ok &= check(select_traversal_start_level(plan, m, 0) == 0, "no levels -> 0");

        // Baseline: no tier enables the optional optimizations, and a default plan asks for no defines.
        for (const HardwareTier tier : kAllHardwareTiers) {
            const DisplacementPlan p = plan_displacement(tier, DisplacementCapabilities::baseline(), DisplacementRequest{});
            ok &= check(!p.corner_gather && !p.start_level_from_footprint && optimization_defines(p).empty(),
                        "tiers keep the verified baseline shader unless a profile opts in");
        }
        TierProfile profile = profile_for_tier(HardwareTier::High);
        profile.corner_gather = true;
        profile.start_level_from_footprint = true;
        const DisplacementPlan opted = plan_displacement(profile, DisplacementCapabilities::baseline(), {});
        const auto defines = optimization_defines(opted);
        ok &= check(defines.size() == 2 && defines[0].first == "SFT_HF_USE_GATHER" &&
                        defines[1].first == "SFT_HF_START_LEVEL_FROM_FOOTPRINT",
                    "opt-in flows through the plan into shader defines");

        // Self-shadow budgets: monotone in quality.
        const SelfShadowQuality qs[] = {SelfShadowQuality::Off, SelfShadowQuality::Cheap, SelfShadowQuality::Production,
                                        SelfShadowQuality::High, SelfShadowQuality::Reference};
        ok &= check(self_shadow_budget(SelfShadowQuality::Off).ray_count == 0, "Off casts no rays");
        ok &= check(self_shadow_max_steps(self_shadow_budget(SelfShadowQuality::Off), 64) == 0, "Off spends no steps");
        for (usize i = 2; i < 5; ++i) {
            const SelfShadowBudget lo = self_shadow_budget(qs[i - 1]);
            const SelfShadowBudget hi = self_shadow_budget(qs[i]);
            ok &= check(hi.ray_count >= lo.ray_count && hi.step_fraction >= lo.step_fraction &&
                            hi.start_level_bias <= lo.start_level_bias,
                        "self-shadow budget must be monotone in quality");
        }
        const SelfShadowBudget cheap = self_shadow_budget(SelfShadowQuality::Cheap);
        ok &= check(cheap.start_level_bias > 0 && cheap.step_fraction == 0.25f && cheap.ray_count == 1 && !cheap.stochastic,
                    "Cheap = coarse start + a quarter of the steps, one hard ray");
        ok &= check(self_shadow_max_steps(cheap, 64) == 16, "Cheap at 64 steps = 16");
        ok &= check(self_shadow_max_steps(cheap, 8) == 4, "step floor of 4");
        ok &= check(self_shadow_max_steps(cheap, 2) == 2, "floor never exceeds the view budget");
        ok &= check(self_shadow_max_steps(self_shadow_budget(SelfShadowQuality::Production), 64) == 64,
                    "Production = full traversal");
        const SelfShadowBudget ref = self_shadow_budget(SelfShadowQuality::Reference);
        ok &= check(ref.ray_count > 1 && ref.stochastic && ref.step_fraction == 1.0f, "Reference = stochastic multi-ray");
        ok &= check(self_shadow_start_level(cheap, 2, 7) == 4, "shadow start = view start + bias");
        ok &= check(self_shadow_start_level(cheap, 6, 7) == 6, "shadow start clamps to the top level");
        return ok;
    }

    // ---- Binning ---------------------------------------------------------------------------------

    bool binning_groups_by_variant_stably() {
        bool ok = true;
        DisplacementPlan plan;
        plan.algorithm = HeightfieldAlgorithm::HierarchicalCellExact;
        plan.self_shadow = SelfShadowQuality::Production;
        plan.corner_gather = true;
        plan.start_level_from_footprint = true;

        const HeightfieldAlgorithm per_draw[] = {
            HeightfieldAlgorithm::HierarchicalCellExact, HeightfieldAlgorithm::NormalOnly,
            HeightfieldAlgorithm::CellExact,             HeightfieldAlgorithm::HierarchicalCellExact,
            HeightfieldAlgorithm::NormalOnly,            HeightfieldAlgorithm::CellExact,
            HeightfieldAlgorithm::HierarchicalCellExact,
        };
        std::vector<u32> keys;
        for (const auto a : per_draw) {
            keys.push_back(variant_key(variant_for(plan, a)));
        }
        const BinnedDraws b = bin_by_variant(keys);
        ok &= check(b.order.size() == keys.size(), "every draw appears once");
        ok &= check(b.bins.size() == 3, "three variants -> three bins");
        ok &= check(b.bins[0].count == 2 && b.bins[1].count == 2 && b.bins[2].count == 3, "bin sizes");
        // Cheap first (NormalOnly), expensive last.
        ok &= check(b.order[0] == 1 && b.order[1] == 4, "NormalOnly draws first, in original order");
        ok &= check(b.order[2] == 2 && b.order[3] == 5, "CellExact next, stable");
        ok &= check(b.order[4] == 0 && b.order[5] == 3 && b.order[6] == 6, "Hierarchical last, stable");
        std::vector<u32> sorted = b.order;
        std::sort(sorted.begin(), sorted.end());
        for (u32 i = 0; i < sorted.size(); ++i) {
            ok &= check(sorted[i] == i, "order is a permutation");
        }
        for (usize i = 1; i < b.bins.size(); ++i) {
            ok &= check(b.bins[i].key > b.bins[i - 1].key && b.bins[i].first == b.bins[i - 1].first + b.bins[i - 1].count,
                        "bins ascend by key and tile the order");
        }
        // Optimization flags only split bins where the algorithm can read them.
        ok &= check(!variant_for(plan, HeightfieldAlgorithm::NormalOnly).corner_gather, "NormalOnly has no gather");
        ok &= check(variant_for(plan, HeightfieldAlgorithm::CellExact).corner_gather &&
                        !variant_for(plan, HeightfieldAlgorithm::CellExact).start_level_from_footprint,
                    "CellExact has gather but no hierarchy start level");
        // Distinct variants get distinct keys.
        DisplacementVariant a;
        DisplacementVariant c = a;
        c.depth = DepthPolicy::DisplacedDepth;
        ok &= check(variant_key(a) != variant_key(c) && variant_key(a) == variant_key(a), "key injective on depth");
        ok &= check(bin_by_variant({}).bins.empty(), "empty input");
        return ok;
    }

    // ---- GPU hierarchy build (via the shader's CPU model) ---------------------------------------------

    std::vector<u8> build_via_model(const HierarchyBufferLayout &layout, const HeightfieldView &view,
                                    HierarchyPrecision precision, HierarchyChannels channels,
                                    std::vector<u32> &words, std::optional<HierarchyDirtyTexels> dirty = {}) {
        const auto passes =
            plan_hierarchy_build(layout, view.width, view.height, view.wrap, precision, channels, dirty);
        if (words.empty()) {
            words.assign(static_cast<usize>(layout.total_bytes / 4), 0xDEADBEEFu); // poison: pad must be overwritten
        }
        run_hierarchy_build_cpu_model(passes, view, words);
        std::vector<u8> bytes(words.size() * 4);
        std::memcpy(bytes.data(), words.data(), bytes.size());
        return repack_tight(layout, bytes);
    }

    bool gpu_build_matches_pack() {
        bool ok = true;
        struct Format {
            HierarchyPrecision p;
            HierarchyChannels c;
        };
        const Format formats[] = {{HierarchyPrecision::Unorm16, HierarchyChannels::MaxOnly},
                                  {HierarchyPrecision::Unorm16, HierarchyChannels::MinMax},
                                  {HierarchyPrecision::Float32, HierarchyChannels::MaxOnly},
                                  {HierarchyPrecision::Float32, HierarchyChannels::MinMax}};
        const std::pair<u32, u32> dims[] = {{64, 64}, {37, 23}, {1, 1}, {2, 1}, {1, 9}, {100, 3}, {256, 128}, {5, 5}};
        for (const auto &[w, h] : dims) {
            for (const bool wrap : {true, false}) {
                Field f = make_noise(w, h, 11u + w * 7u + h, wrap);
                // Values beyond the unorm range and exactly-on-grid values exercise the clamp/rounding.
                if (f.heights.size() > 3) {
                    f.heights[0] = 0.0f;
                    f.heights[1] = 1.0f;
                    f.heights[2] = 1.0f / 65535.0f;
                    f.heights[3] = 32768.0f / 65535.0f;
                }
                const HeightfieldHierarchy hier = HeightfieldHierarchy::build(f.view());
                ok &= check(hierarchy_level_count(w, h) == hier.level_count(), "level count helper agrees with the builder");
                for (const Format &fmt : formats) {
                    for (const u32 row_align : {4u, 256u}) {
                        const HierarchyBufferLayout layout = hierarchy_buffer_layout(
                            w, h, fmt.p, fmt.c, row_align, row_align == 256 ? 512u : 4u);
                        std::vector<u32> words;
                        const std::vector<u8> got = build_via_model(layout, f.view(), fmt.p, fmt.c, words);
                        const PackedHierarchy packed = hier.pack(fmt.p, fmt.c);
                        ok &= check(got.size() == packed.data.size(), "repacked size equals pack() size");
                        if (got != packed.data) {
                            std::cerr << "  mismatch dims " << w << "x" << h << " wrap=" << wrap
                                      << " p=" << static_cast<int>(fmt.p) << " c=" << static_cast<int>(fmt.c)
                                      << " align=" << row_align << '\n';
                        }
                        ok &= check(got == packed.data, "GPU-build model must equal HeightfieldHierarchy::pack byte for byte");
                    }
                }
            }
        }
        return ok;
    }

    bool gpu_dirty_build_matches_full_rebuild() {
        bool ok = true;
        std::mt19937 rng(21);
        const std::pair<u32, u32> dims[] = {{64, 64}, {37, 23}, {100, 33}};
        for (const auto &[w, h] : dims) {
            for (const bool wrap : {true, false}) {
                for (const auto prec : {HierarchyPrecision::Unorm16, HierarchyPrecision::Float32}) {
                    for (const auto ch : {HierarchyChannels::MaxOnly, HierarchyChannels::MinMax}) {
                        Field f = make_noise(w, h, 3u + w + h, wrap);
                        const HierarchyBufferLayout layout = hierarchy_buffer_layout(w, h, prec, ch);
                        std::vector<u32> words;
                        (void)build_via_model(layout, f.view(), prec, ch, words);

                        u64 dirty_threads = 0;
                        u64 full_threads = 0;
                        for (const auto &p : plan_hierarchy_build(layout, w, h, wrap, prec, ch)) {
                            full_threads += static_cast<u64>(p.group_count_x) * p.group_count_y;
                        }
                        for (u32 round = 0; round < 12; ++round) {
                            // Rects including ones touching the seam/edges and a whole-field edit.
                            u32 x0 = static_cast<u32>(rng() % w);
                            u32 y0 = static_cast<u32>(rng() % h);
                            u32 x1 = std::min(w, x0 + 1 + static_cast<u32>(rng() % 6));
                            u32 y1 = std::min(h, y0 + 1 + static_cast<u32>(rng() % 6));
                            if (round == 0) {
                                x0 = 0;
                                y0 = 0;
                            } else if (round == 1) {
                                x1 = w;
                                y1 = h;
                            } else if (round == 2) {
                                x0 = 0;
                                x1 = 1;
                            } else if (round == 3) {
                                x0 = w - 1;
                                x1 = w;
                                y0 = h - 1;
                                y1 = h;
                            }
                            for (u32 y = y0; y < y1; ++y) {
                                for (u32 x = x0; x < x1; ++x) {
                                    f.heights[static_cast<usize>(y) * w + x] = static_cast<f32>(rng() % 10001) / 10000.0f;
                                }
                            }
                            const HierarchyDirtyTexels dirty{x0, y0, x1, y1};
                            for (const auto &p : plan_hierarchy_build(layout, w, h, wrap, prec, ch, dirty)) {
                                dirty_threads += static_cast<u64>(p.group_count_x) * p.group_count_y;
                            }
                            const std::vector<u8> got = build_via_model(layout, f.view(), prec, ch, words, dirty);
                            const PackedHierarchy expect = HeightfieldHierarchy::build(f.view()).pack(prec, ch);
                            if (got != expect.data) {
                                std::cerr << "  dirty mismatch " << w << "x" << h << " wrap=" << wrap << " round=" << round
                                          << " rect " << x0 << ',' << y0 << ',' << x1 << ',' << y1 << '\n';
                            }
                            ok &= check(got == expect.data, "dirty-tile GPU build must equal a full rebuild");
                        }
                        if (w >= 64 && h >= 64) {
                            ok &= check(dirty_threads < 12 * full_threads, "dirty passes must be cheaper than full rebuilds");
                        }
                    }
                }
            }
        }
        // A tiny edit on a big field must dispatch far fewer groups than a full build.
        const HierarchyBufferLayout layout =
            hierarchy_buffer_layout(512, 512, HierarchyPrecision::Unorm16, HierarchyChannels::MaxOnly);
        u64 full = 0;
        u64 tiny = 0;
        for (const auto &p : plan_hierarchy_build(layout, 512, 512, true, HierarchyPrecision::Unorm16,
                                                  HierarchyChannels::MaxOnly)) {
            full += static_cast<u64>(p.group_count_x) * p.group_count_y;
        }
        for (const auto &p : plan_hierarchy_build(layout, 512, 512, true, HierarchyPrecision::Unorm16,
                                                  HierarchyChannels::MaxOnly, HierarchyDirtyTexels{100, 100, 104, 104})) {
            tiny += static_cast<u64>(p.group_count_x) * p.group_count_y;
        }
        ok &= check(tiny * 20 < full, "a 4x4 edit of a 512x512 field costs a small fraction of a full build");
        ok &= check(plan_hierarchy_build(layout, 0, 0, true, HierarchyPrecision::Unorm16, HierarchyChannels::MaxOnly)
                        .empty(),
                    "invalid size -> no passes");
        return ok;
    }

    bool layout_alignment_is_copy_legal() {
        bool ok = true;
        const HierarchyBufferLayout l = hierarchy_buffer_layout(1000, 700, HierarchyPrecision::Unorm16,
                                                                HierarchyChannels::MaxOnly, 256, 512);
        for (u32 mip = 0; mip < l.mip_count; ++mip) {
            ok &= check(l.mip_row_pitch_bytes[mip] % 256 == 0 && l.mip_offset_bytes[mip] % 512 == 0,
                        "texture-copy layout: 256-byte pitch, 512-byte offsets");
            ok &= check(l.mip_row_pitch_bytes[mip] >= l.mip_width(mip) * 2, "pitch covers the row");
        }
        const HierarchyBufferLayout t = hierarchy_buffer_layout(1000, 700, HierarchyPrecision::Unorm16,
                                                                HierarchyChannels::MaxOnly);
        const PackedHierarchy p = HeightfieldHierarchy::build(
                                      HeightfieldView{std::vector<f32>(700 * 1000, 0.5f), 1000, 700, true})
                                      .pack(HierarchyPrecision::Unorm16, HierarchyChannels::MaxOnly);
        ok &= check(t.mip_count == p.mip_count && t.width == p.width && t.height == p.height,
                    "buffer layout mip chain matches PackedHierarchy");
        return ok;
    }

    // ---- Welded displacement normals -----------------------------------------------------------------

    // A unit cube with hard edges: 24 vertices, split normals.
    void make_hard_cube(std::vector<GeometryVertex> &v, std::vector<u32> &idx) {
        const glm::vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        for (int face = 0; face < 6; ++face) {
            const glm::vec3 n = axes[face % 3] * (face < 3 ? 1.0f : -1.0f);
            const glm::vec3 t = axes[(face + 1) % 3];
            const glm::vec3 b = glm::cross(n, t);
            const u32 base = static_cast<u32>(v.size());
            for (int c = 0; c < 4; ++c) {
                GeometryVertex gv;
                gv.position = n * 0.5f + t * ((c & 1) ? 0.5f : -0.5f) + b * ((c & 2) ? 0.5f : -0.5f);
                gv.normal = n;
                v.push_back(gv);
            }
            // Winding so the geometric normal agrees with n.
            const bool flip = glm::dot(glm::cross(v[base + 1].position - v[base].position,
                                                  v[base + 2].position - v[base].position), n) < 0.0f;
            const u32 tri[6] = {0, 1, 2, 1, 3, 2};
            for (int i = 0; i < 6; i += 3) {
                if (flip) {
                    idx.insert(idx.end(), {base + tri[i], base + tri[i + 2], base + tri[i + 1]});
                } else {
                    idx.insert(idx.end(), {base + tri[i], base + tri[i + 1], base + tri[i + 2]});
                }
            }
        }
    }

    f32 max_displaced_gap(const std::vector<GeometryVertex> &v, const std::vector<glm::vec3> &dir, f32 amount) {
        f32 worst = 0.0f;
        for (usize i = 0; i < v.size(); ++i) {
            for (usize j = i + 1; j < v.size(); ++j) {
                if (glm::distance(v[i].position, v[j].position) < 1.0e-6f) {
                    worst = std::max(worst, glm::distance(v[i].position + dir[i] * amount, v[j].position + dir[j] * amount));
                }
            }
        }
        return worst;
    }

    bool welded_normals_close_hard_edges() {
        bool ok = true;
        std::vector<GeometryVertex> v;
        std::vector<u32> idx;
        make_hard_cube(v, idx);
        ok &= check(v.size() == 24 && idx.size() == 36, "cube fixture");

        std::vector<glm::vec3> split(v.size());
        for (usize i = 0; i < v.size(); ++i) {
            split[i] = v[i].normal;
        }
        ok &= check(max_displaced_gap(v, split, 0.1f) > 0.05f, "split normals tear a displaced hard-edged mesh");

        const std::vector<glm::vec3> welded = compute_welded_displacement_normals(v, idx);
        ok &= check(welded.size() == v.size(), "one direction per vertex");
        ok &= check(max_displaced_gap(v, welded, 0.1f) == 0.0f, "welded normals displace coincident vertices identically");
        for (const glm::vec3 &n : welded) {
            ok &= check(std::abs(glm::length(n) - 1.0f) < 1.0e-5f, "welded normals are unit length");
        }
        // Cube corners point along the diagonal (all three faces contribute equal angles).
        const f32 diag = 1.0f / std::sqrt(3.0f);
        for (usize i = 0; i < v.size(); ++i) {
            ok &= check(std::abs(std::abs(welded[i].x) - diag) < 1.0e-5f && std::abs(std::abs(welded[i].y) - diag) < 1.0e-5f &&
                            std::abs(std::abs(welded[i].z) - diag) < 1.0e-5f,
                        "cube corner direction is the diagonal");
        }
        const std::vector<u32> groups = position_weld_groups(v, 1.0e-5f);
        std::vector<u32> distinct = groups;
        std::sort(distinct.begin(), distinct.end());
        distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
        ok &= check(distinct.size() == 8, "24 cube vertices weld to 8 positions");

        // Smooth mesh: welding must not change already-consistent normals (a flat quad stays flat).
        std::vector<GeometryVertex> quad(4);
        quad[0].position = {0, 0, 0};
        quad[1].position = {1, 0, 0};
        quad[2].position = {0, 1, 0};
        quad[3].position = {1, 1, 0};
        for (auto &q : quad) {
            q.normal = {0, 0, 1};
        }
        const std::vector<u32> qi = {0, 1, 2, 1, 3, 2};
        for (const glm::vec3 &n : compute_welded_displacement_normals(quad, qi)) {
            ok &= check(std::abs(n.z - 1.0f) < 1.0e-6f, "flat quad keeps its normal");
        }
        // Degenerate/garbage input: no crash, own-normal fallback.
        const std::vector<u32> bad = {0, 0, 0, 9, 9, 9, 0, 1};
        for (const glm::vec3 &n : compute_welded_displacement_normals(quad, bad)) {
            ok &= check(std::abs(glm::length(n) - 1.0f) < 1.0e-5f, "degenerate triangles fall back to the vertex normals");
        }
        // Tolerance: a vertex 5e-6 away welds at 1e-5, not at 1e-7.
        std::vector<GeometryVertex> pair(2);
        pair[0].position = {0, 0, 0};
        pair[1].position = {5.0e-6f, 0, 0};
        ok &= check(position_weld_groups(pair, 1.0e-5f)[1] == 0 && position_weld_groups(pair, 1.0e-7f)[1] == 1,
                    "weld epsilon is honoured");
        return ok;
    }

} // namespace

int main() {
    struct Case {
        const char *name;
        bool (*run)();
    };
    const Case cases[] = {
        {"gather component order + wrap/clamp parity", gather_component_order_and_parity},
        {"start level never changes the hit", start_level_never_changes_the_hit},
        {"shadow start level never changes the answer", shadow_start_level_never_changes_the_answer},
        {"planner start level / budgets / defines", planner_start_level_and_budgets},
        {"binning groups by variant stably", binning_groups_by_variant_stably},
        {"GPU hierarchy build model == pack()", gpu_build_matches_pack},
        {"GPU dirty-tile build == full rebuild", gpu_dirty_build_matches_full_rebuild},
        {"build buffer layout is copy-legal", layout_alignment_is_copy_legal},
        {"welded normals close hard edges", welded_normals_close_hard_edges},
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
