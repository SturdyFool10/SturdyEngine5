#include <Renderer/Displacement/RayTracingReference.hpp>

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

namespace SFT::Renderer::Displacement {

    namespace {

        constexpr f32 kBaryTolerance = 1.0e-5f;

        /// One boundary crossing: ray parameter, barycentric coordinates, normalized height.
        struct Crossing {
            f32 t = 0.0f;
            glm::vec3 bary{};
            f32 height = 0.0f;
        };

        struct CrossingSet {
            Crossing items[10]{};
            u32 count = 0;
            void add(f32 t, const glm::vec3 &bary, f32 height) noexcept {
                if (count < 10) {
                    items[count++] = Crossing{t, bary, height};
                }
            }
        };

        /// Moller-Trumbore without culling, plus the barycentric range check. `corner` are the three cap
        /// corners in vertex order, so the returned (1-u-v, u, v) are directly the prism barycentrics.
        void cross_cap(const glm::vec3 *corner, f32 height, const glm::vec3 &o, const glm::vec3 &d,
                       CrossingSet &out) noexcept {
            const glm::vec3 e1 = corner[1] - corner[0];
            const glm::vec3 e2 = corner[2] - corner[0];
            const glm::vec3 pv = glm::cross(d, e2);
            const f32 det = glm::dot(e1, pv);
            if (std::abs(det) < 1.0e-20f) {
                return;
            }
            const f32 inv = 1.0f / det;
            const glm::vec3 tv = o - corner[0];
            const f32 u = glm::dot(tv, pv) * inv;
            const glm::vec3 qv = glm::cross(tv, e1);
            const f32 v = glm::dot(d, qv) * inv;
            if (u < -kBaryTolerance || v < -kBaryTolerance || u + v > 1.0f + kBaryTolerance) {
                return;
            }
            out.add(glm::dot(e2, qv) * inv, glm::vec3(1.0f - u - v, u, v), height);
        }

        /// Ray vs the bilinear side patch  S(a, w) = c00 + a e1 + w e2 + a w e3  (a along the edge, w up the
        /// height range). Eliminates t by projecting onto the plane perpendicular to the ray, which leaves
        /// two bilinear equations and a quadratic in `a`.
        void cross_side(const glm::vec3 &c00, const glm::vec3 &c10, const glm::vec3 &c01, const glm::vec3 &c11,
                        u32 edge_i, u32 edge_j, f32 h_min, f32 h_max, const glm::vec3 &o, const glm::vec3 &d,
                        CrossingSet &out) noexcept {
            const f32 d_len2 = glm::dot(d, d);
            if (d_len2 <= 0.0f) {
                return;
            }
            // Orthonormal pair perpendicular to d.
            const glm::vec3 dn = d * (1.0f / std::sqrt(d_len2));
            const glm::vec3 helper = std::abs(dn.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
            const glm::vec3 u1 = glm::normalize(glm::cross(dn, helper));
            const glm::vec3 u2 = glm::cross(dn, u1);

            const glm::vec3 e1 = c10 - c00;
            const glm::vec3 e2 = c01 - c00;
            const glm::vec3 e3 = c00 - c10 - c01 + c11;
            const glm::vec3 r = c00 - o;
            const f32 A1 = glm::dot(u1, r), A2 = glm::dot(u2, r);
            const f32 B1 = glm::dot(u1, e1), B2 = glm::dot(u2, e1);
            const f32 C1 = glm::dot(u1, e2), C2 = glm::dot(u2, e2);
            const f32 D1 = glm::dot(u1, e3), D2 = glm::dot(u2, e3);

            // (A1 + a B1)(C2 + a D2) = (A2 + a B2)(C1 + a D1)
            const f32 qa = B1 * D2 - B2 * D1;
            const f32 qb = B1 * C2 + A1 * D2 - B2 * C1 - A2 * D1;
            const f32 qc = A1 * C2 - A2 * C1;

            f32 roots[2]{};
            u32 root_count = 0;
            const f32 scale = std::max({std::abs(qb), std::abs(qc), 1.0e-30f});
            if (std::abs(qa) <= 1.0e-6f * scale) {
                if (std::abs(qb) > 1.0e-30f) {
                    roots[root_count++] = -qc / qb;
                }
            } else {
                const f32 disc = qb * qb - 4.0f * qa * qc;
                if (disc >= 0.0f) {
                    const f32 sq = std::sqrt(disc);
                    const f32 q = -0.5f * (qb + (qb >= 0.0f ? sq : -sq));
                    roots[root_count++] = q / qa;
                    if (q != 0.0f) {
                        roots[root_count++] = qc / q;
                    }
                }
            }

            for (u32 i = 0; i < root_count; ++i) {
                const f32 a = roots[i];
                if (a < -kBaryTolerance || a > 1.0f + kBaryTolerance) {
                    continue;
                }
                // w from whichever equation has the better-conditioned denominator.
                const f32 den1 = C1 + a * D1;
                const f32 den2 = C2 + a * D2;
                f32 w;
                if (std::abs(den1) >= std::abs(den2)) {
                    if (std::abs(den1) < 1.0e-30f) {
                        continue;
                    }
                    w = -(A1 + a * B1) / den1;
                } else {
                    w = -(A2 + a * B2) / den2;
                }
                if (w < -kBaryTolerance || w > 1.0f + kBaryTolerance) {
                    continue;
                }
                const glm::vec3 point = c00 + e1 * a + e2 * w + e3 * (a * w);
                const f32 t = glm::dot(point - o, d) / d_len2;
                glm::vec3 bary(0.0f);
                bary[static_cast<glm::length_t>(edge_i)] = 1.0f - a;
                bary[static_cast<glm::length_t>(edge_j)] = a;
                out.add(t, bary, h_min + (h_max - h_min) * w);
            }
        }

        [[nodiscard]] glm::vec3 lerp3(const glm::vec3 &a, const glm::vec3 &b, f32 t) noexcept { return a + (b - a) * t; }

        [[nodiscard]] glm::vec2 uv_at(const DisplacedTriangle &tri, const glm::vec3 &b) noexcept {
            return tri.uv[0] * b.x + tri.uv[1] * b.y + tri.uv[2] * b.z;
        }

        [[nodiscard]] f32 displacement_of(const DisplacedMaterialParams &params, f32 height) noexcept {
            return (height - params.reference_height) * params.height_scale;
        }

    } // namespace

    glm::vec3 prism_position(const DisplacedTriangle &tri, const DisplacedMaterialParams &params,
                             const glm::vec3 &b, f32 height) noexcept {
        const f32 s = displacement_of(params, height);
        return b.x * (tri.position[0] + s * tri.normal[0]) + b.y * (tri.position[1] + s * tri.normal[1]) +
               b.z * (tri.position[2] + s * tri.normal[2]);
    }

    Prism make_prism(const DisplacedTriangle &tri, const DisplacedMaterialParams &params) noexcept {
        Prism prism;
        prism.s_min = displacement_of(params, params.height_min);
        prism.s_max = displacement_of(params, params.height_max);
        for (u32 i = 0; i < 3; ++i) {
            prism.corner[i] = tri.position[i] + prism.s_min * tri.normal[i];
            prism.corner[3 + i] = tri.position[i] + prism.s_max * tri.normal[i];
        }
        return prism;
    }

    PrismBounds prism_bounds(const Prism &prism) noexcept {
        PrismBounds bounds{prism.corner[0], prism.corner[0]};
        for (const glm::vec3 &c : prism.corner) {
            bounds.min = glm::min(bounds.min, c);
            bounds.max = glm::max(bounds.max, c);
        }
        return bounds;
    }

    RayPrismRange intersect_prism(const Prism &prism, const glm::vec3 &o, const glm::vec3 &d, f32 t_min,
                                  f32 t_max) noexcept {
        // Heights are recovered from the crossing itself, so the prism only needs its height range; the
        // caller passes it through the corner layout (bottom cap / top cap) and s_min/s_max ordering.
        // h_min/h_max are encoded as 0/1 here and remapped by the caller (intersect_displaced_triangle).
        CrossingSet set;
        cross_cap(prism.corner, 0.0f, o, d, set);
        cross_cap(prism.corner + 3, 1.0f, o, d, set);
        for (u32 k = 0; k < 3; ++k) {
            const u32 i = (k + 1) % 3;
            const u32 j = (k + 2) % 3;
            cross_side(prism.corner[i], prism.corner[j], prism.corner[3 + i], prism.corner[3 + j], i, j, 0.0f, 1.0f,
                       o, d, set);
        }

        RayPrismRange range;
        if (set.count == 0) {
            return range;
        }
        u32 lo = 0;
        u32 hi = 0;
        for (u32 i = 1; i < set.count; ++i) {
            if (set.items[i].t < set.items[lo].t) {
                lo = i;
            }
            if (set.items[i].t > set.items[hi].t) {
                hi = i;
            }
        }
        if (set.items[hi].t < t_min || set.items[lo].t > t_max || !(set.items[hi].t > set.items[lo].t)) {
            return range;
        }
        range.hit = true;
        range.t_enter = set.items[lo].t;
        range.t_exit = set.items[hi].t;
        range.bary_enter = set.items[lo].bary;
        range.bary_exit = set.items[hi].bary;
        range.height_enter = set.items[lo].height; // 0 = bottom cap, 1 = top cap (normalized within the prism)
        range.height_exit = set.items[hi].height;
        return range;
    }

    DisplacedHit intersect_displaced_triangle(const DisplacedTriangle &tri, const DisplacedMaterialParams &params,
                                              const HeightfieldView &heightfield, const HeightfieldHierarchy *hierarchy,
                                              const glm::vec3 &o, const glm::vec3 &d, f32 t_min, f32 t_max) noexcept {
        DisplacedHit result;
        if (!heightfield.valid() || !(params.height_max > params.height_min)) {
            return result;
        }
        const Prism prism = make_prism(tri, params);
        const RayPrismRange range = intersect_prism(prism, o, d, t_min, t_max);
        if (!range.hit) {
            return result;
        }

        // Prism-local heights are 0..1 across [height_min, height_max]; convert to normalized heights.
        const f32 h_span = params.height_max - params.height_min;
        const f32 h_enter = params.height_min + range.height_enter * h_span;
        const f32 h_exit = params.height_min + range.height_exit * h_span;

        // Clip the crossing interval to the caller's [t_min, t_max]. The (b, h) state is linear in t along
        // the chord, so clipping is a lerp — no inverse map of the origin needed.
        const f32 span = range.t_exit - range.t_enter;
        const f32 tau0 = std::clamp((t_min - range.t_enter) / span, 0.0f, 1.0f);
        const f32 tau1 = std::clamp((t_max - range.t_enter) / span, 0.0f, 1.0f);
        if (!(tau1 > tau0)) {
            return result;
        }
        const glm::vec3 b_a = lerp3(range.bary_enter, range.bary_exit, tau0);
        const glm::vec3 b_b = lerp3(range.bary_enter, range.bary_exit, tau1);
        const f32 h_a = h_enter + (h_exit - h_enter) * tau0;
        const f32 h_b = h_enter + (h_exit - h_enter) * tau1;
        const f32 t_a = range.t_enter + span * tau0;
        const f32 t_b = range.t_enter + span * tau1;

        HeightfieldRay ray;
        const glm::vec2 uv_a = uv_at(tri, b_a);
        const glm::vec2 uv_b = uv_at(tri, b_b);
        ray.origin = glm::vec3(uv_a.x, uv_a.y, h_a);
        ray.direction = glm::vec3(uv_b.x - uv_a.x, uv_b.y - uv_a.y, h_b - h_a);
        ray.max_t = 1.0f;

        const HeightfieldHit hit = hierarchy != nullptr && hierarchy->valid()
                                       ? trace_hierarchical(heightfield, *hierarchy, ray, params.max_steps)
                                       : trace_cell_exact(heightfield, ray, params.max_steps);
        result.status = hit.status;
        result.steps = hit.steps;
        if (hit.status == HitStatus::Miss) {
            return result;
        }

        const f32 tau = std::clamp(hit.t, 0.0f, 1.0f);
        const glm::vec3 b = lerp3(b_a, b_b, tau);
        result.barycentrics = b;
        result.uv = hit.uv;
        result.height = hit.height;
        // The chord approximation can put the point marginally off the ray; keep t inside the caller's interval
    // (a legal value for a procedural-primitive commit).
    result.t = std::clamp(t_a + (t_b - t_a) * tau, t_min, t_max);
        result.position = prism_position(tri, params, b, hit.height);

        // Exact surface derivative of P(b1, b2) = sum_i b_i (p_i + s n_i), s = height_scale (h(uv(b)) - ref).
        const f32 s = displacement_of(params, hit.height);
        const glm::vec3 n_interp = b.x * tri.normal[0] + b.y * tri.normal[1] + b.z * tri.normal[2];
        const glm::vec3 q0 = tri.position[0] + s * tri.normal[0];
        const glm::vec3 q1 = tri.position[1] + s * tri.normal[1];
        const glm::vec3 q2 = tri.position[2] + s * tri.normal[2];
        const glm::vec2 du1 = tri.uv[1] - tri.uv[0];
        const glm::vec2 du2 = tri.uv[2] - tri.uv[0];
        const f32 ds1 = params.height_scale * glm::dot(hit.gradient, du1);
        const f32 ds2 = params.height_scale * glm::dot(hit.gradient, du2);
        const glm::vec3 dp1 = (q1 - q0) + n_interp * ds1;
        const glm::vec3 dp2 = (q2 - q0) + n_interp * ds2;
        glm::vec3 normal = glm::cross(dp1, dp2);
        const f32 len2 = glm::dot(normal, normal);
        const f32 n_len2 = glm::dot(n_interp, n_interp);
        if (len2 > 1.0e-30f) {
            normal *= 1.0f / std::sqrt(len2);
            if (glm::dot(normal, n_interp) < 0.0f) {
                normal = -normal;
            }
        } else {
            normal = n_len2 > 0.0f ? n_interp * (1.0f / std::sqrt(n_len2)) : glm::vec3(0.0f, 0.0f, 1.0f);
        }
        result.normal = normal;
        result.back_face = glm::dot(d, normal) > 0.0f;
        return result;
    }

    // ------------------------------------------------------------------------------------------------
    // Independent ground truth
    // ------------------------------------------------------------------------------------------------

    namespace {

        /// Newton inversion of P(b1, b2, s) = X starting from `state` (b1, b2, s). Returns false if the
        /// iteration does not converge (point is off the prism's map, or the Jacobian is singular).
        [[nodiscard]] bool invert_prism(const DisplacedTriangle &tri, const glm::vec3 &x, glm::vec3 &state) noexcept {
            for (int iter = 0; iter < 24; ++iter) {
                const f32 b1 = state.x, b2 = state.y, s = state.z;
                const f32 b0 = 1.0f - b1 - b2;
                const glm::vec3 q0 = tri.position[0] + s * tri.normal[0];
                const glm::vec3 q1 = tri.position[1] + s * tri.normal[1];
                const glm::vec3 q2 = tri.position[2] + s * tri.normal[2];
                const glm::vec3 p = b0 * q0 + b1 * q1 + b2 * q2;
                const glm::vec3 residual = p - x;
                if (glm::dot(residual, residual) < 1.0e-14f) {
                    return true;
                }
                const glm::vec3 n = b0 * tri.normal[0] + b1 * tri.normal[1] + b2 * tri.normal[2];
                const glm::mat3 jacobian(q1 - q0, q2 - q0, n);
                if (std::abs(glm::determinant(jacobian)) < 1.0e-18f) {
                    return false;
                }
                state -= glm::inverse(jacobian) * residual;
            }
            const f32 b0 = 1.0f - state.x - state.y;
            const glm::vec3 p = b0 * (tri.position[0] + state.z * tri.normal[0]) +
                                state.x * (tri.position[1] + state.z * tri.normal[1]) +
                                state.y * (tri.position[2] + state.z * tri.normal[2]);
            return glm::dot(p - x, p - x) < 1.0e-10f;
        }

        struct SolidSample {
            bool solid = false;
            glm::vec3 state{1.0f / 3.0f, 1.0f / 3.0f, 0.0f};
        };

        [[nodiscard]] SolidSample sample_solid(const DisplacedTriangle &tri, const DisplacedMaterialParams &params,
                                               const HeightfieldView &hf, const glm::vec3 &x,
                                               glm::vec3 seed) noexcept {
            SolidSample out;
            out.state = seed;
            if (!invert_prism(tri, x, out.state)) {
                return out;
            }
            const f32 b0 = 1.0f - out.state.x - out.state.y;
            constexpr f32 kEdge = 1.0e-5f;
            if (b0 < -kEdge || out.state.x < -kEdge || out.state.y < -kEdge) {
                return out;
            }
            const f32 h = params.reference_height + out.state.z / params.height_scale;
            if (h < params.height_min - 1.0e-6f || h > params.height_max + 1.0e-6f) {
                return out;
            }
            const glm::vec2 uv = tri.uv[0] * b0 + tri.uv[1] * out.state.x + tri.uv[2] * out.state.y;
            out.solid = h <= sample_height(hf, uv.x, uv.y);
            return out;
        }

    } // namespace

    DisplacedHit brute_force_displaced_triangle(const DisplacedTriangle &tri, const DisplacedMaterialParams &params,
                                                const HeightfieldView &hf, const glm::vec3 &o, const glm::vec3 &d,
                                                f32 t_min, f32 t_max, u32 samples) noexcept {
        DisplacedHit result;
        if (!hf.valid() || !(params.height_scale > 0.0f) || !(t_max > t_min)) {
            return result;
        }
        glm::vec3 seed{1.0f / 3.0f, 1.0f / 3.0f, 0.0f};
        auto eval = [&](f32 t, glm::vec3 &state) {
            const SolidSample s = sample_solid(tri, params, hf, o + d * t, state);
            return s;
        };

        f32 prev_t = t_min;
        SolidSample prev = eval(prev_t, seed);
        if (prev.solid) {
            result.status = HitStatus::Hit;
        }
        for (u32 i = 1; i <= samples && result.status != HitStatus::Hit; ++i) {
            const f32 t = t_min + (t_max - t_min) * (static_cast<f32>(i) / static_cast<f32>(samples));
            glm::vec3 warm = prev.state;
            SolidSample cur = eval(t, warm);
            if (std::isnan(cur.state.x) || std::isnan(cur.state.z)) {
                cur.solid = false;
                cur.state = glm::vec3(1.0f / 3.0f, 1.0f / 3.0f, 0.0f);
            }
            if (cur.solid && !prev.solid) {
                // Bisect the transition.
                f32 lo = prev_t;
                f32 hi = t;
                SolidSample hi_sample = cur;
                for (int k = 0; k < 40; ++k) {
                    const f32 mid = 0.5f * (lo + hi);
                    glm::vec3 st = hi_sample.state;
                    const SolidSample m = eval(mid, st);
                    if (m.solid) {
                        hi = mid;
                        hi_sample = m;
                    } else {
                        lo = mid;
                    }
                }
                result.status = HitStatus::Hit;
                prev_t = hi;
                prev = hi_sample;
                break;
            }
            prev_t = t;
            prev = cur;
        }
        if (result.status != HitStatus::Hit) {
            return result;
        }

        const f32 b0 = 1.0f - prev.state.x - prev.state.y;
        result.t = prev_t;
        result.barycentrics = glm::vec3(b0, prev.state.x, prev.state.y);
        result.uv = uv_at(tri, result.barycentrics);
        result.height = std::clamp(params.reference_height + prev.state.z / params.height_scale, 0.0f, 1.0f);
        result.position = o + d * prev_t;
        return result;
    }

} // namespace SFT::Renderer::Displacement
