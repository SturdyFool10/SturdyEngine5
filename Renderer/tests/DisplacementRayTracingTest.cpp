#include <Renderer/Displacement/HeightfieldHierarchy.hpp>
#include <Renderer/Displacement/HeightfieldTrace.hpp>
#include <Renderer/Displacement/RayTracingReference.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>
#include <glm/vector_relational.hpp>

// CPU-only tests for the ray-tracing displacement slot: prism construction, exact ray/prism crossings
// (caps + bilinear sides), and the displaced-triangle intersection that chains them into the existing
// heightfield blocks. Ground truth is brute_force_displaced_triangle — a fine march in world space that
// inverts the prism map by Newton iteration and shares no code with the prism/chord/cell-solve path.

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
        [[nodiscard]] HeightfieldView view() const { return HeightfieldView{heights, width, height, true}; }
    };

    Field make_rolling_field(u32 w, u32 h, f32 amplitude) {
        Field f{{}, w, h};
        f.heights.resize(static_cast<usize>(w) * h);
        const f32 two_pi = 6.28318530718f;
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const f32 u = static_cast<f32>(x) / static_cast<f32>(w);
                const f32 v = static_cast<f32>(y) / static_cast<f32>(h);
                const f32 s = 0.5f + amplitude * (0.5f * std::sin(two_pi * 2.0f * u) * std::cos(two_pi * 3.0f * v) +
                                                  0.3f * std::sin(two_pi * 5.0f * (u + v)) + 0.2f * std::cos(two_pi * 7.0f * u));
                f.heights[static_cast<usize>(y) * w + x] = std::clamp(s, 0.0f, 1.0f);
            }
        }
        return f;
    }

    Field make_noise_field(u32 w, u32 h, u32 seed) {
        Field f{{}, w, h};
        f.heights.resize(static_cast<usize>(w) * h);
        std::mt19937 rng(seed);
        std::uniform_real_distribution<f32> dist(0.0f, 1.0f);
        for (f32 &v : f.heights) {
            v = dist(rng);
        }
        return f;
    }

    struct Rng {
        std::mt19937 gen;
        explicit Rng(u32 seed) : gen(seed) {}
        f32 uniform(f32 lo, f32 hi) { return std::uniform_real_distribution<f32>(lo, hi)(gen); }
        glm::vec3 unit_vector() {
            for (;;) {
                const glm::vec3 v(uniform(-1, 1), uniform(-1, 1), uniform(-1, 1));
                const f32 l = glm::length(v);
                if (l > 0.1f && l <= 1.0f) {
                    return v / l;
                }
            }
        }
    };

    /// Random triangle of edge length ~1 in a random orientation. `normal_spread` 0 = the flat face
    /// normal at every vertex (right prism, chord approximation exact); > 0 tilts each vertex normal.
    DisplacedTriangle random_triangle(Rng &rng, f32 normal_spread, f32 uv_scale) {
        DisplacedTriangle tri;
        const glm::vec3 a = rng.unit_vector();
        glm::vec3 b = glm::normalize(glm::cross(a, rng.unit_vector()));
        const glm::vec3 c = glm::cross(a, b);
        // Corners of a triangle in the plane spanned by b, c around a random centre.
        const glm::vec3 centre(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1));
        const glm::vec2 local[3] = {{rng.uniform(0.6f, 1.0f), 0.0f},
                                    {rng.uniform(-0.6f, -0.2f), rng.uniform(0.5f, 0.9f)},
                                    {rng.uniform(-0.6f, -0.2f), -rng.uniform(0.5f, 0.9f)}};
        for (int i = 0; i < 3; ++i) {
            tri.position[i] = centre + b * local[i].x + c * local[i].y;
        }
        glm::vec3 face = glm::normalize(glm::cross(tri.position[1] - tri.position[0], tri.position[2] - tri.position[0]));
        for (int i = 0; i < 3; ++i) {
            tri.normal[i] = glm::normalize(face + rng.unit_vector() * normal_spread);
        }
        // UVs: an affine function of the corners so the mapping is non-degenerate, plus a random offset.
        const glm::vec2 offset(rng.uniform(0.0f, 4.0f), rng.uniform(0.0f, 4.0f));
        for (int i = 0; i < 3; ++i) {
            tri.uv[i] = offset + uv_scale * glm::vec2(glm::dot(tri.position[i] - centre, b), glm::dot(tri.position[i] - centre, c));
        }
        return tri;
    }

    glm::vec3 face_normal(const DisplacedTriangle &t) {
        return glm::normalize(glm::cross(t.position[1] - t.position[0], t.position[2] - t.position[0]));
    }

    glm::vec3 centroid(const DisplacedTriangle &t) { return (t.position[0] + t.position[1] + t.position[2]) / 3.0f; }

    // ---- Prism crossings against an independent domain march ------------------------------------------

    /// Ground truth for the prism alone: march along the ray and record the first/last t at which the
    /// point lies inside the prism (barycentrics in [0,1], displacement inside [s_min, s_max]).
    bool march_prism(const DisplacedTriangle &tri, const DisplacedMaterialParams &params, const glm::vec3 &o,
                     const glm::vec3 &d, f32 t_max, u32 samples, f32 &first, f32 &last) {
        const f32 s_min = (params.height_min - params.reference_height) * params.height_scale;
        const f32 s_max = (params.height_max - params.reference_height) * params.height_scale;
        bool any = false;
        glm::vec3 state(1.0f / 3.0f, 1.0f / 3.0f, 0.0f);
        for (u32 i = 0; i <= samples; ++i) {
            const f32 t = t_max * static_cast<f32>(i) / static_cast<f32>(samples);
            const glm::vec3 x = o + d * t;
            // Newton solve, restarted from the centre when it diverges.
            bool converged = false;
            for (int attempt = 0; attempt < 2 && !converged; ++attempt) {
                glm::vec3 st = attempt == 0 ? state : glm::vec3(1.0f / 3.0f, 1.0f / 3.0f, 0.0f);
                for (int it = 0; it < 30; ++it) {
                    const f32 b1 = st.x, b2 = st.y, s = st.z, b0 = 1.0f - b1 - b2;
                    const glm::vec3 q0 = tri.position[0] + s * tri.normal[0];
                    const glm::vec3 q1 = tri.position[1] + s * tri.normal[1];
                    const glm::vec3 q2 = tri.position[2] + s * tri.normal[2];
                    const glm::vec3 r = b0 * q0 + b1 * q1 + b2 * q2 - x;
                    if (glm::dot(r, r) < 1.0e-14f) {
                        converged = true;
                        break;
                    }
                    const glm::vec3 n = b0 * tri.normal[0] + b1 * tri.normal[1] + b2 * tri.normal[2];
                    const glm::mat3 J(q1 - q0, q2 - q0, n);
                    if (std::abs(glm::determinant(J)) < 1.0e-12f) {
                        break;
                    }
                    st -= glm::inverse(J) * r;
                }
                if (converged) {
                    state = st;
                }
            }
            if (!converged) {
                continue;
            }
            const f32 b0 = 1.0f - state.x - state.y;
            constexpr f32 e = 1.0e-4f;
            if (b0 >= -e && state.x >= -e && state.y >= -e && state.z >= s_min - e && state.z <= s_max + e) {
                if (!any) {
                    first = t;
                }
                last = t;
                any = true;
            }
        }
        return any;
    }

    bool prism_crossings_match_a_domain_march() {
        bool ok = true;
        Rng rng(11);
        u32 compared = 0;
        u32 bad = 0;
        u32 miss_disagree = 0;
        for (int i = 0; i < 400; ++i) {
            // Half right prisms, half strongly flared ones (bilinear, non-planar sides).
            const DisplacedTriangle tri = random_triangle(rng, i % 2 == 0 ? 0.0f : 0.6f, 1.0f);
            DisplacedMaterialParams params;
            params.height_scale = rng.uniform(0.1f, 0.5f);
            params.reference_height = rng.uniform(0.0f, 1.0f);
            const Prism prism = make_prism(tri, params);
            // Aim at a random point of the prism from a random direction, so about half enter through the
            // sides rather than the caps.
            const glm::vec3 target = centroid(tri) + face_normal(tri) * rng.uniform(-0.05f, 0.05f) +
                                     (tri.position[0] - centroid(tri)) * rng.uniform(-1.0f, 1.0f);
            const glm::vec3 dir = rng.unit_vector();
            const glm::vec3 origin = target - dir * 3.0f;
            const RayPrismRange r = intersect_prism(prism, origin, dir, 0.0f, 6.0f);
            f32 first = 0, last = 0;
            const bool truth = march_prism(tri, params, origin, dir, 6.0f, 6000, first, last);
            if (truth != r.hit) {
                // A marginal graze can legitimately be missed by a 1e-3-step march; only count clear ones.
                if (truth && (last - first) > 4.0e-3f) {
                    ++miss_disagree;
                }
                if (!truth && r.hit && (r.t_exit - r.t_enter) > 4.0e-3f) {
                    ++miss_disagree;
                }
                continue;
            }
            if (!truth) {
                continue;
            }
            ++compared;
            const f32 tol = 3.0e-3f;
            if (std::abs(r.t_enter - first) > tol || std::abs(r.t_exit - last) > tol) {
                ++bad;
            }
        }
        ok &= check(compared > 100, "prism test fixture must actually cross prisms");
        ok &= check(miss_disagree == 0, "exact prism crossing never contradicts the domain march about hit/miss");
        ok &= check(bad == 0, "prism entry/exit t match the independent domain march (caps and bilinear sides)");
        std::cout << "  prism crossings: compared " << compared << ", t mismatches " << bad << ", hit/miss contradictions "
                  << miss_disagree << '\n';
        return ok;
    }

    bool prism_bounds_contain_the_prism() {
        Rng rng(3);
        bool ok = true;
        for (int i = 0; i < 100; ++i) {
            const DisplacedTriangle tri = random_triangle(rng, 0.7f, 1.0f);
            DisplacedMaterialParams params;
            params.height_scale = 0.4f;
            params.reference_height = 0.5f;
            const Prism prism = make_prism(tri, params);
            const PrismBounds bounds = prism_bounds(prism);
            for (int k = 0; k < 40; ++k) {
                const f32 a = rng.uniform(0, 1), b = rng.uniform(0, 1 - a);
                const glm::vec3 bary(1.0f - a - b, a, b);
                const f32 h = rng.uniform(0, 1);
                const glm::vec3 p = prism_position(tri, params, bary, h);
                ok &= check(glm::all(glm::greaterThanEqual(p, bounds.min - 1.0e-4f)) &&
                                glm::all(glm::lessThanEqual(p, bounds.max + 1.0e-4f)),
                            "every point of the prism lies inside prism_bounds");
            }
        }
        return ok;
    }

    // ---- Displaced-triangle intersection ---------------------------------------------------------------

    struct HitStats {
        u32 rays = 0;
        u32 both_hit = 0;
        u32 both_miss = 0;
        u32 disagree = 0;
        u32 t_off = 0;
        f32 max_t_error = 0.0f;
        u32 hier_vs_cell_mismatch = 0;
        u32 side_entries = 0;
        u32 origin_inside = 0;
    };

    enum class RayKind { FromOutside, ThroughSide, Grazing, FromSurface };

    HitStats compare_displaced(const Field &field, const DisplacedMaterialParams &params, f32 normal_spread,
                               RayKind kind, u32 seed, u32 count, f32 t_tolerance) {
        HitStats stats;
        Rng rng(seed);
        const HeightfieldView view = field.view();
        const HeightfieldHierarchy hier = HeightfieldHierarchy::build(view);
        for (u32 i = 0; i < count; ++i) {
            const DisplacedTriangle tri = random_triangle(rng, normal_spread, 1.0f);
            const glm::vec3 n = face_normal(tri);
            glm::vec3 origin, dir;
            switch (kind) {
                case RayKind::FromOutside: {
                    const glm::vec3 target = centroid(tri) + (tri.position[0] - centroid(tri)) * rng.uniform(-0.8f, 0.8f);
                    dir = glm::normalize(-n + rng.unit_vector() * 0.6f);
                    origin = target - dir * 2.0f;
                    break;
                }
                case RayKind::ThroughSide: {
                    // Start beside the triangle, in its plane, aim at a point near the triangle's edge and just
                    // below the top of the prism so the ray enters through a side face.
                    const glm::vec3 edge_mid = (tri.position[0] + tri.position[1]) * 0.5f;
                    const glm::vec3 outward = glm::normalize(edge_mid - centroid(tri));
                    const f32 top = std::max(0.0f, (1.0f - params.reference_height)) * params.height_scale;
                    origin = edge_mid + outward * 1.5f + n * rng.uniform(0.0f, top);
                    dir = glm::normalize(centroid(tri) - origin + n * rng.uniform(-0.1f, 0.05f));
                    break;
                }
                case RayKind::Grazing: {
                    const glm::vec3 t1 = glm::normalize(tri.position[1] - tri.position[0]);
                    const glm::vec3 t2 = glm::cross(n, t1);
                    const f32 ang = rng.uniform(0.0f, 6.2831853f);
                    const glm::vec3 lateral = t1 * std::cos(ang) + t2 * std::sin(ang);
                    dir = glm::normalize(lateral - n * rng.uniform(0.02f, 0.12f));
                    origin = centroid(tri) - lateral * 1.5f + n * rng.uniform(0.15f, 0.35f) * params.height_scale;
                    break;
                }
                case RayKind::FromSurface: {
                    // Secondary-ray style: start just above the surface at a random spot, leave into the
                    // hemisphere (or sideways, where the relief may block it).
                    glm::vec3 b(rng.uniform(0.05f, 0.9f), 0, 0);
                    b.y = rng.uniform(0.05f, 0.95f - b.x);
                    b.z = 1.0f - b.x - b.y;
                    const glm::vec2 uv = tri.uv[0] * b.z + tri.uv[1] * b.x + tri.uv[2] * b.y;
                    (void)uv;
                    const glm::vec3 bary(b.z, b.x, b.y);
                    const glm::vec2 uv2 = tri.uv[0] * bary.x + tri.uv[1] * bary.y + tri.uv[2] * bary.z;
                    const f32 h = sample_height(view, uv2.x, uv2.y);
                    const glm::vec3 surface = prism_position(tri, params, bary, h);
                    const glm::vec3 up = glm::normalize(bary.x * tri.normal[0] + bary.y * tri.normal[1] + bary.z * tri.normal[2]);
                    origin = surface + up * 2.0e-3f;
                    dir = glm::normalize(up * rng.uniform(0.05f, 1.0f) + glm::normalize(glm::cross(up, rng.unit_vector())) * 0.9f);
                    if (glm::dot(dir, up) < 0.02f) {
                        dir = glm::normalize(dir + up * 0.05f);
                    }
                    ++stats.origin_inside;
                    break;
                }
            }
            const f32 t_max = 6.0f;
            const DisplacedHit got = intersect_displaced_triangle(tri, params, view, &hier, origin, dir, 0.0f, t_max);
            const DisplacedHit cell = intersect_displaced_triangle(tri, params, view, nullptr, origin, dir, 0.0f, t_max);
            const DisplacedHit truth = brute_force_displaced_triangle(tri, params, view, origin, dir, 0.0f, t_max, 24000);
            ++stats.rays;
            if ((got.status == HitStatus::Hit) != (cell.status == HitStatus::Hit) ||
                (got.status == HitStatus::Hit && std::abs(got.t - cell.t) > 1.0e-4f)) {
                ++stats.hier_vs_cell_mismatch;
            }
            if (kind == RayKind::ThroughSide && got.status == HitStatus::Hit) {
                ++stats.side_entries;
            }
            const bool a = got.status == HitStatus::Hit;
            const bool b = truth.status == HitStatus::Hit;
            if (a && b) {
                ++stats.both_hit;
                const f32 err = std::abs(got.t - truth.t);
                stats.max_t_error = std::max(stats.max_t_error, err);
                if (err > t_tolerance) {
                    ++stats.t_off;
                }
            } else if (!a && !b) {
                ++stats.both_miss;
            } else {
                ++stats.disagree;
            }
        }
        return stats;
    }

    void print_stats(const char *label, const HitStats &s) {
        std::cout << "  " << label << ": rays " << s.rays << ", both hit " << s.both_hit << ", both miss " << s.both_miss
                  << ", disagree " << s.disagree << ", t off " << s.t_off << ", max |dt| " << s.max_t_error << '\n';
    }

    bool right_prisms_match_brute_force_exactly() {
        bool ok = true;
        const Field smooth = make_rolling_field(64, 64, 0.9f);
        for (f32 reference : {1.0f, 0.5f, 0.0f}) {
            DisplacedMaterialParams params;
            params.height_scale = 0.35f;
            params.reference_height = reference;
            const struct {
                RayKind kind;
                const char *name;
            } kinds[] = {{RayKind::FromOutside, "from outside"},
                         {RayKind::ThroughSide, "through side"},
                         {RayKind::Grazing, "grazing"},
                         {RayKind::FromSurface, "from surface"}};
            for (const auto &k : kinds) {
                const HitStats s = compare_displaced(smooth, params, 0.0f, k.kind, 100 + static_cast<u32>(reference * 10), 250, 4.0e-3f);
                print_stats(k.name, s);
                ok &= check(s.hier_vs_cell_mismatch == 0, "hierarchy must not change the displaced-triangle result");
                // A right prism makes the chord exact, so beyond marching resolution the two must agree.
                ok &= check(s.disagree <= s.rays / 100 + 1, "right prism: hit/miss agrees with brute force (a rare graze aside)");
                ok &= check(s.t_off <= s.both_hit / 100 + 1, "right prism: hit distance agrees with brute force");
                ok &= check(s.both_hit > 20, "fixture must produce hits");
            }
            const HitStats side = compare_displaced(smooth, params, 0.0f, RayKind::ThroughSide, 5, 250, 4.0e-3f);
            ok &= check(side.side_entries > 10, "some rays must enter through prism sides and hit");
        }
        return ok;
    }

    bool noisy_field_and_thin_features() {
        bool ok = true;
        const Field noisy = make_noise_field(24, 24, 9);
        DisplacedMaterialParams params;
        params.height_scale = 0.3f;
        params.reference_height = 1.0f;
        params.height_min = 0.0f;
        params.height_max = 1.0f;
        const HitStats s = compare_displaced(noisy, params, 0.0f, RayKind::FromOutside, 21, 300, 4.0e-3f);
        print_stats("noise field", s);
        ok &= check(s.hier_vs_cell_mismatch == 0, "noisy: hierarchy must not change the result");
        ok &= check(s.disagree <= s.rays / 50 + 1, "noisy: hit/miss agrees with brute force");
        ok &= check(s.t_off <= s.both_hit / 50 + 1, "noisy: hit distance agrees with brute force");
        return ok;
    }

    bool flared_prisms_are_a_close_approximation() {
        bool ok = true;
        // Varying vertex normals: the chord is the projective approximation, so agreement is statistical.
        // The prism crossings are exact (tested above); the residual error is only the straight-chord
        // assumption. The bounds asserted here are the accuracy the approximation is expected to keep at
        // moderate normal divergence.
        const Field smooth = make_rolling_field(64, 64, 0.9f);
        DisplacedMaterialParams params;
        params.height_scale = 0.2f;
        params.reference_height = 1.0f;
        for (f32 spread : {0.15f, 0.35f}) {
            const HitStats s = compare_displaced(smooth, params, spread, RayKind::FromOutside, 31, 300, 2.5e-2f);
            print_stats(spread < 0.2f ? "flared 0.15 from outside" : "flared 0.35 from outside", s);
            ok &= check(s.hier_vs_cell_mismatch == 0, "flared: hierarchy must not change the result");
            ok &= check(s.disagree <= s.rays / 8 + 1, "flared: hit/miss mostly agrees with world-space brute force");
            ok &= check(s.t_off <= s.both_hit / 6 + 1, "flared: hit distances stay close to brute force");
        }
        return ok;
    }

    bool hit_records_are_self_consistent() {
        bool ok = true;
        Rng rng(77);
        const Field smooth = make_rolling_field(64, 64, 0.9f);
        const HeightfieldView view = smooth.view();
        const HeightfieldHierarchy hier = HeightfieldHierarchy::build(view);
        DisplacedMaterialParams params;
        params.height_scale = 0.3f;
        params.reference_height = 0.7f;
        u32 hits = 0;
        f32 worst_normal = 1.0f;
        for (int i = 0; i < 300; ++i) {
            const DisplacedTriangle tri = random_triangle(rng, 0.25f, 1.0f);
            const glm::vec3 n = face_normal(tri);
            const glm::vec3 target = centroid(tri) + (tri.position[0] - centroid(tri)) * rng.uniform(-0.7f, 0.7f);
            const glm::vec3 dir = glm::normalize(-n + rng.unit_vector() * 0.5f);
            const glm::vec3 origin = target - dir * 2.0f;
            const DisplacedHit h = intersect_displaced_triangle(tri, params, view, &hier, origin, dir, 0.0f, 8.0f);
            if (h.status != HitStatus::Hit) {
                continue;
            }
            ++hits;
            // The reported point is on the displaced surface: P(b, height(uv)).
            const glm::vec3 p = prism_position(tri, params, h.barycentrics, sample_height(view, h.uv.x, h.uv.y));
            ok &= check(glm::length(p - h.position) < 1.0e-4f, "hit position is P(b, surface height)");
            ok &= check(std::abs(h.barycentrics.x + h.barycentrics.y + h.barycentrics.z - 1.0f) < 1.0e-5f,
                        "barycentrics sum to one");
            ok &= check(h.barycentrics.x > -1.0e-3f && h.barycentrics.y > -1.0e-3f && h.barycentrics.z > -1.0e-3f,
                        "hit lies inside the triangle's prism");
            ok &= check(glm::length(h.normal) > 0.999f && glm::length(h.normal) < 1.001f, "unit normal");
            const glm::vec3 nb = glm::normalize(h.barycentrics.x * tri.normal[0] + h.barycentrics.y * tri.normal[1] +
                                                h.barycentrics.z * tri.normal[2]);
            ok &= check(glm::dot(h.normal, nb) > 0.0f, "normal is on the vertex-normal side");
            // Finite-difference the surface for the true normal.
            const f32 e = 1.0e-3f;
            auto surf = [&](f32 b1, f32 b2) {
                const glm::vec3 bary(1.0f - b1 - b2, b1, b2);
                const glm::vec2 uv = tri.uv[0] * bary.x + tri.uv[1] * bary.y + tri.uv[2] * bary.z;
                return prism_position(tri, params, bary, sample_height(view, uv.x, uv.y));
            };
            const f32 b1 = h.barycentrics.y, b2 = h.barycentrics.z;
            const glm::vec3 d1 = surf(b1 + e, b2) - surf(b1 - e, b2);
            const glm::vec3 d2 = surf(b1, b2 + e) - surf(b1, b2 - e);
            glm::vec3 fd = glm::normalize(glm::cross(d1, d2));
            if (glm::dot(fd, nb) < 0.0f) {
                fd = -fd;
            }
            worst_normal = std::min(worst_normal, glm::dot(fd, h.normal));
        }
        ok &= check(hits > 100, "enough hits to check");
        ok &= check(worst_normal > 0.95f, "analytic normal matches a finite-difference normal of the displaced surface");
        std::cout << "  hit records: " << hits << " hits, worst normal dot " << worst_normal << '\n';
        return ok;
    }

    bool tight_height_bounds_do_not_change_the_answer() {
        bool ok = true;
        Rng rng(5);
        // A field that never exceeds [0.2, 0.6]: tight prisms must give the same hits as [0, 1] prisms.
        Field f = make_rolling_field(48, 48, 0.4f);
        f32 lo = 1.0f, hi = 0.0f;
        for (f32 &v : f.heights) {
            v = 0.2f + 0.4f * v;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        const HeightfieldView view = f.view();
        const HeightfieldHierarchy hier = HeightfieldHierarchy::build(view);
        DisplacedMaterialParams wide;
        wide.height_scale = 0.4f;
        wide.reference_height = 0.5f;
        DisplacedMaterialParams tight = wide;
        tight.height_min = lo;
        tight.height_max = hi;
        u32 compared = 0;
        for (int i = 0; i < 200; ++i) {
            const DisplacedTriangle tri = random_triangle(rng, 0.0f, 1.0f);
            const glm::vec3 n = face_normal(tri);
            const glm::vec3 target = centroid(tri) + (tri.position[0] - centroid(tri)) * rng.uniform(-0.7f, 0.7f);
            const glm::vec3 dir = glm::normalize(-n + rng.unit_vector() * 0.5f);
            const glm::vec3 origin = target - dir * 2.0f;
            const DisplacedHit a = intersect_displaced_triangle(tri, wide, view, &hier, origin, dir, 0.0f, 8.0f);
            const DisplacedHit b = intersect_displaced_triangle(tri, tight, view, &hier, origin, dir, 0.0f, 8.0f);
            if (a.status == HitStatus::Hit || b.status == HitStatus::Hit) {
                ++compared;
                // A hit within a hair of the prism's side wall is a graze: whether the last epsilon of the
                // chord counts depends on where the walls sit, so it is allowed to differ between the two.
                const DisplacedHit &hit = a.status == HitStatus::Hit ? a : b;
                const bool grazing_wall = hit.status == HitStatus::Hit &&
                                          std::min({hit.barycentrics.x, hit.barycentrics.y, hit.barycentrics.z}) < 2.0e-3f;
                const bool same = (a.status == b.status && std::abs(a.t - b.t) < 2.0e-3f) || grazing_wall;
                ok &= check(same, "tight height bounds give the same hit as [0, 1] bounds");
            }
        }
        ok &= check(compared > 30, "fixture must produce hits");
        return ok;
    }

    bool degenerate_inputs_miss_cleanly() {
        bool ok = true;
        const Field f = make_rolling_field(16, 16, 0.5f);
        const HeightfieldView view = f.view();
        DisplacedTriangle tri;
        tri.position[0] = {0, 0, 0};
        tri.position[1] = {1, 0, 0};
        tri.position[2] = {0, 1, 0};
        for (auto &n : tri.normal) {
            n = {0, 0, 1};
        }
        tri.uv[0] = {0, 0};
        tri.uv[1] = {1, 0};
        tri.uv[2] = {0, 1};
        DisplacedMaterialParams params;
        params.height_scale = 0.0f; // no displacement: zero-thickness prism, callers use the base triangle
        ok &= check(intersect_displaced_triangle(tri, params, view, nullptr, {0.3f, 0.3f, 2.0f}, {0, 0, -1}, 0, 10).status ==
                        HitStatus::Miss,
                    "zero height scale is a zero-volume prism: miss (caller uses the base triangle)");
        params.height_scale = 0.3f;
        ok &= check(intersect_displaced_triangle(tri, params, view, nullptr, {5, 5, 2}, {0, 0, -1}, 0, 10).status ==
                        HitStatus::Miss,
                    "a ray beside the prism misses");
        ok &= check(intersect_displaced_triangle(tri, params, view, nullptr, {0.3f, 0.3f, 2.0f}, {0, 0, 1}, 0, 10).status ==
                        HitStatus::Miss,
                    "a ray pointing away misses");
        ok &= check(intersect_displaced_triangle(tri, params, view, nullptr, {0.3f, 0.3f, 2.0f}, {0, 0, -1}, 0, 0.5f).status ==
                        HitStatus::Miss,
                    "t_max short of the prism misses");
        const DisplacedHit down = intersect_displaced_triangle(tri, params, view, nullptr, {0.3f, 0.3f, 2.0f}, {0, 0, -1}, 0, 10);
        ok &= check(down.status == HitStatus::Hit && down.t > 1.99f && down.t < 2.35f, "a head-on ray finds the surface");
        ok &= check(!down.back_face && down.normal.z > 0.0f, "head-on hit is front-facing with an upward normal");
        return ok;
    }

} // namespace

int main() {
    struct Case {
        const char *name;
        bool (*run)();
    };
    const Case cases[] = {
        {"prism crossings vs domain march", prism_crossings_match_a_domain_march},
        {"prism bounds", prism_bounds_contain_the_prism},
        {"right prisms vs brute force", right_prisms_match_brute_force_exactly},
        {"noisy field", noisy_field_and_thin_features},
        {"flared prisms approximation", flared_prisms_are_a_close_approximation},
        {"hit records", hit_records_are_self_consistent},
        {"tight height bounds", tight_height_bounds_do_not_change_the_answer},
        {"degenerate inputs", degenerate_inputs_miss_cleanly},
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
