#include <Renderer/Displacement/HeightfieldTrace.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/geometric.hpp>

namespace SFT::Renderer::Displacement {

    namespace {

        constexpr f32 kInf = std::numeric_limits<f32>::infinity();
        // Distance (in level-0 cell units) the cell lookup is nudged along the direction of travel so a
        // ray sitting exactly on a cell boundary is assigned to the cell it is entering, not leaving.
        constexpr f32 kBoundaryNudge = 1.0e-4f;
        // Slack on the "ray is above every surface in this node" test. Absorbs float noise so a ray
        // grazing a bound is descended into rather than wrongly skipped.
        constexpr f32 kSkipSlack = 1.0e-6f;

        [[nodiscard]] i32 floor_div(i32 a, i32 b) noexcept {
            i32 q = a / b;
            if ((a % b != 0) && ((a < 0) != (b < 0))) {
                --q;
            }
            return q;
        }

        [[nodiscard]] f32 sign_or_zero(f32 v) noexcept { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }

        struct Bilinear {
            f32 a, b, c, d;
            [[nodiscard]] f32 eval(f32 fu, f32 fv) const noexcept { return a + b * fu + c * fv + d * fu * fv; }
        };

        [[nodiscard]] Bilinear make_bilinear(f32 h00, f32 h10, f32 h01, f32 h11) noexcept {
            return {h00, h10 - h00, h01 - h00, h00 - h10 - h01 + h11};
        }

        /// Slab entry/exit parameters for a ray in cell space. `bottom_limited` records that the exit is
        /// the floor of the slab, where a descending ray is guaranteed to have met the surface.
        struct RayRange {
            f32 t_enter = 0.0f;
            f32 t_exit = 0.0f;
            bool bottom_limited = false;
            bool empty = false;
        };

        [[nodiscard]] RayRange compute_range(const HeightfieldView &hf, const glm::vec3 &o, const glm::vec3 &d,
                                             f32 max_t) noexcept {
            RayRange r;
            r.t_exit = max_t;
            if (o.z < 0.0f || (o.z > 1.0f && d.z >= 0.0f)) {
                r.empty = true;
                return r;
            }
            if (o.z > 1.0f) {
                r.t_enter = (1.0f - o.z) / d.z;
            }
            if (d.z < 0.0f) {
                const f32 bottom = -o.z / d.z;
                if (bottom <= r.t_exit) {
                    r.t_exit = bottom;
                    r.bottom_limited = true;
                }
            } else if (d.z > 0.0f) {
                r.t_exit = std::min(r.t_exit, (1.0f - o.z) / d.z);
            }

            if (!hf.wrap) {
                // A non-wrapping field is only defined between the first and last texel centres, which is
                // where the cell polynomials are valid; rays leaving that rectangle miss.
                const f32 hi[2] = {static_cast<f32>(hf.width) - 1.0f, static_cast<f32>(hf.height) - 1.0f};
                const f32 oc[2] = {o.x, o.y};
                const f32 dc[2] = {d.x, d.y};
                for (int axis = 0; axis < 2; ++axis) {
                    if (dc[axis] == 0.0f) {
                        if (oc[axis] < 0.0f || oc[axis] > hi[axis]) {
                            r.empty = true;
                            return r;
                        }
                        continue;
                    }
                    f32 t0 = (0.0f - oc[axis]) / dc[axis];
                    f32 t1 = (hi[axis] - oc[axis]) / dc[axis];
                    if (t0 > t1) {
                        std::swap(t0, t1);
                    }
                    r.t_enter = std::max(r.t_enter, t0);
                    if (t1 < r.t_exit) {
                        r.t_exit = t1;
                        r.bottom_limited = false;
                    }
                }
            }
            r.empty = !(r.t_enter < r.t_exit);
            return r;
        }

        [[nodiscard]] HeightfieldHit finish_hit(const HeightfieldView &hf, HitStatus status, f32 t,
                                                const glm::vec3 &o_uv, const glm::vec3 &d_uv, u32 steps) noexcept {
            HeightfieldHit hit;
            hit.status = status;
            hit.t = t;
            hit.uv = glm::vec2(o_uv.x + d_uv.x * t, o_uv.y + d_uv.y * t);
            const f32 ray_h = o_uv.z + d_uv.z * t;
            hit.height = sample_height(hf, hit.uv.x, hit.uv.y);
            hit.gradient = sample_gradient(hf, hit.uv.x, hit.uv.y);
            hit.residual = std::abs(ray_h - hit.height);
            hit.steps = steps;
            return hit;
        }

        [[nodiscard]] HeightfieldHit trace_grid(const HeightfieldView &hf, const HeightfieldHierarchy *hierarchy,
                                                const HeightfieldRay &ray, u32 max_steps,
                                                u32 start_level = kStartAtTopLevel) noexcept {
            HeightfieldHit miss;
            if (!hf.valid()) {
                return miss;
            }
            const auto w = static_cast<i32>(hf.width);
            const auto h = static_cast<i32>(hf.height);
            // Cell space: one unit = one texel, so cell (i, j) is [i, i+1) x [j, j+1) and texel centres sit on
            // integers. The mapping u -> u*W - 0.5 is what makes a cell the bilinear patch between centres.
            const glm::vec3 o(ray.origin.x * static_cast<f32>(w) - 0.5f, ray.origin.y * static_cast<f32>(h) - 0.5f,
                              ray.origin.z);
            const glm::vec3 d(ray.direction.x * static_cast<f32>(w), ray.direction.y * static_cast<f32>(h),
                              ray.direction.z);

            const RayRange range = compute_range(hf, o, d, ray.max_t);
            if (range.empty) {
                return miss;
            }

            const bool use_hierarchy = hierarchy != nullptr && hierarchy->valid();
            const u32 top_level = use_hierarchy ? hierarchy->level_count() - 1 : 0;
            u32 level = std::min(start_level, top_level);
            f32 t = range.t_enter;
            u32 steps = 0;

            while (t < range.t_exit && steps < max_steps) {
                ++steps;

                // Cell containing the ray just past `t`, in level-0 units.
                const glm::vec2 probe(o.x + d.x * t + sign_or_zero(d.x) * kBoundaryNudge,
                                      o.y + d.y * t + sign_or_zero(d.y) * kBoundaryNudge);
                i32 wi_x = static_cast<i32>(std::floor(probe.x));
                i32 wi_y = static_cast<i32>(std::floor(probe.y));
                i32 base_x = 0;
                i32 base_y = 0;
                if (hf.wrap) {
                    base_x = floor_div(wi_x, w);
                    base_y = floor_div(wi_y, h);
                } else {
                    wi_x = std::clamp(wi_x, 0, w - 1);
                    wi_y = std::clamp(wi_y, 0, h - 1);
                }
                const i32 ix = wi_x - base_x * w;
                const i32 iy = wi_y - base_y * h;

                // Extent of the node being tested. At level 0 this is exactly the cell; at coarser levels it
                // is the aligned 2^L block of cells, clipped to the field so a non-power-of-two size still
                // has a well-formed last node (the hierarchy builder clips identically).
                const i32 kx = ix >> level;
                const i32 ky = iy >> level;
                const f32 lo_x = static_cast<f32>(base_x * w + (kx << level));
                const f32 hi_x = static_cast<f32>(base_x * w + std::min((kx + 1) << level, w));
                const f32 lo_y = static_cast<f32>(base_y * h + (ky << level));
                const f32 hi_y = static_cast<f32>(base_y * h + std::min((ky + 1) << level, h));

                const f32 tx = d.x > 0.0f ? (hi_x - o.x) / d.x : (d.x < 0.0f ? (lo_x - o.x) / d.x : kInf);
                const f32 ty = d.y > 0.0f ? (hi_y - o.y) / d.y : (d.y < 0.0f ? (lo_y - o.y) / d.y : kInf);
                f32 t_next = std::min({tx, ty, range.t_exit});
                if (!(t_next > t)) {
                    t_next = std::nextafter(t, kInf); // always make progress, even on a degenerate box
                }

                const f32 h_a = o.z + d.z * t;
                const f32 h_b = o.z + d.z * t_next;
                const f32 ray_low = std::min(h_a, h_b);

                if (level > 0) {
                    const f32 node_max = hierarchy->node(level, static_cast<u32>(kx), static_cast<u32>(ky)).max_height;
                    if (ray_low > node_max + kSkipSlack) {
                        t = t_next;
                        level = std::min(level + 1, top_level); // after clearing a region, try a bigger step
                    } else {
                        --level;
                    }
                    continue;
                }

                const f32 h00 = hf.texel(ix, iy);
                const f32 h10 = hf.texel(ix + 1, iy);
                const f32 h01 = hf.texel(ix, iy + 1);
                const f32 h11 = hf.texel(ix + 1, iy + 1);
                if (ray_low > std::max({h00, h10, h01, h11}) + kSkipSlack) {
                    t = t_next;
                    level = std::min(level + 1, top_level);
                    continue;
                }

                const f32 fu0 = (o.x + d.x * t) - static_cast<f32>(wi_x);
                const f32 fv0 = (o.y + d.y * t) - static_cast<f32>(wi_y);
                if (const auto solve = solve_bilinear_cell(h00, h10, h01, h11, fu0, fv0, h_a, d.x, d.y, d.z, t_next - t)) {
                    const f32 hit_t = t + solve->s;
                    const f32 fu = std::clamp((o.x + d.x * hit_t) - static_cast<f32>(wi_x), 0.0f, 1.0f);
                    const f32 fv = std::clamp((o.y + d.y * hit_t) - static_cast<f32>(wi_y), 0.0f, 1.0f);
                    const Bilinear patch = make_bilinear(h00, h10, h01, h11);

                    HeightfieldHit hit;
                    hit.status = HitStatus::Hit;
                    hit.t = hit_t;
                    hit.uv = glm::vec2((o.x + d.x * hit_t + 0.5f) / static_cast<f32>(w),
                                       (o.y + d.y * hit_t + 0.5f) / static_cast<f32>(h));
                    hit.height = patch.eval(fu, fv);
                    hit.gradient = glm::vec2((patch.b + patch.d * fv) * static_cast<f32>(w),
                                             (patch.c + patch.d * fu) * static_cast<f32>(h));
                    hit.residual = solve->residual;
                    hit.steps = steps;
                    return hit;
                }
                t = t_next;
            }

            // uv-space copies of the ray for reporting positions.
            const glm::vec3 d_uv = ray.direction;
            const glm::vec3 o_uv = ray.origin;
            if (t < range.t_exit) {
                return finish_hit(hf, HitStatus::BudgetExhausted, t, o_uv, d_uv, steps);
            }
            if (range.bottom_limited) {
                // A descending ray that reached the floor of the slab must be at or under the surface; if the
                // per-cell solves disagreed by rounding, report the floor crossing rather than a spurious miss.
                return finish_hit(hf, HitStatus::Hit, range.t_exit, o_uv, d_uv, steps);
            }
            return miss;
        }

        [[nodiscard]] HeightfieldHit march_and_bisect(const HeightfieldView &hf, const HeightfieldRay &ray,
                                                      u32 samples, u32 refinements) noexcept {
            HeightfieldHit miss;
            if (!hf.valid() || samples == 0) {
                return miss;
            }
            const glm::vec3 &o = ray.origin;
            const glm::vec3 &d = ray.direction;
            f32 t_enter = 0.0f;
            f32 t_exit = ray.max_t;
            if (o.z < 0.0f || (o.z > 1.0f && d.z >= 0.0f)) {
                return miss;
            }
            if (o.z > 1.0f) {
                t_enter = (1.0f - o.z) / d.z;
            }
            if (d.z < 0.0f) {
                t_exit = std::min(t_exit, -o.z / d.z);
            } else if (d.z > 0.0f) {
                t_exit = std::min(t_exit, (1.0f - o.z) / d.z);
            }
            if (!(t_enter < t_exit)) {
                return miss;
            }

            auto signed_gap = [&](f32 t) {
                return (o.z + d.z * t) - sample_height(hf, o.x + d.x * t, o.y + d.y * t);
            };

            f32 prev_t = t_enter;
            if (signed_gap(prev_t) <= 0.0f) {
                return finish_hit(hf, HitStatus::Hit, prev_t, o, d, 0);
            }
            const f32 dt = (t_exit - t_enter) / static_cast<f32>(samples);
            // A descending ray that runs to the slab floor is under the surface there by definition (every
            // height is >= 0), exactly as the cell-exact block reports it. Without this the last sample of a
            // floor-touching ray (surface height exactly 0, e.g. a clamped field) is decided by rounding
            // noise, and a CPU and a GPU disagree about hit vs miss (found by DisplacementGpuParityTest).
            const bool floor_limited = d.z < 0.0f && (-o.z / d.z) <= ray.max_t;
            for (u32 i = 1; i <= samples; ++i) {
                const f32 t = t_enter + dt * static_cast<f32>(i);
                if ((floor_limited && i == samples) || signed_gap(t) <= 0.0f) {
                    f32 lo = prev_t; // above the surface
                    f32 hi = t;      // at or below it
                    for (u32 r = 0; r < refinements; ++r) {
                        const f32 mid = 0.5f * (lo + hi);
                        (signed_gap(mid) <= 0.0f ? hi : lo) = mid;
                    }
                    return finish_hit(hf, HitStatus::Hit, 0.5f * (lo + hi), o, d, i);
                }
                prev_t = t;
            }
            return miss;
        }

    } // namespace

    optional<CellSolve> solve_bilinear_cell(f32 h00, f32 h10, f32 h01, f32 h11, f32 fu0, f32 fv0, f32 h0, f32 dfu,
                                            f32 dfv, f32 dh, f32 segment_length) noexcept {
        const Bilinear patch = make_bilinear(h00, h10, h01, h11);

        // F(s) = ray height - surface height along the segment: F(0) = C, and the first s > 0 with
        // F(s) = 0 is where the ray goes under the surface.
        const f32 c = h0 - patch.eval(fu0, fv0);
        const f32 b = dh - patch.b * dfu - patch.c * dfv - patch.d * (fu0 * dfv + fv0 * dfu);
        const f32 a = -patch.d * dfu * dfv;

        auto residual_at = [&](f32 s) {
            return std::abs(h0 + dh * s - patch.eval(fu0 + dfu * s, fv0 + dfv * s));
        };

        if (c <= 0.0f) {
            return CellSolve{0.0f, std::abs(c)}; // already at or under the surface on entry
        }

        // Slack so a root a rounding error past the segment end (the cell's far boundary, shared with the
        // next cell) is still accepted rather than lost between two cells.
        const f32 limit = segment_length * (1.0f + 1.0e-4f) + 1.0e-7f;

        f32 root = kInf;
        const f32 scale = std::max({1.0f, std::abs(b), std::abs(c)});
        if (std::abs(a) <= 1.0e-7f * scale) {
            if (b < 0.0f) {
                root = -c / b;
            }
        } else {
            const f32 disc = b * b - 4.0f * a * c;
            if (disc >= 0.0f) {
                const f32 sq = std::sqrt(disc);
                // Cancellation-free pair: q avoids subtracting two nearly equal magnitudes; the roots are
                // then q/a and c/q.
                const f32 q = -0.5f * (b + std::copysign(sq, b));
                const f32 r1 = q / a;
                const f32 r2 = q != 0.0f ? c / q : kInf;
                for (const f32 r : {r1, r2}) {
                    if (r >= 0.0f && r < root) {
                        root = r;
                    }
                }
            }
        }

        if (root <= limit) {
            const f32 s = std::min(root, segment_length);
            return CellSolve{s, residual_at(s)};
        }

        // The polynomial found nothing inside the segment, but if the ray has ended up under the surface
        // the intermediate value theorem still guarantees a crossing — reachable only through rounding.
        // Report the far boundary, which is the tightest point we can vouch for.
        if (patch.eval(fu0 + dfu * segment_length, fv0 + dfv * segment_length) >=
            h0 + dh * segment_length) {
            return CellSolve{segment_length, residual_at(segment_length)};
        }
        return std::nullopt;
    }

    f32 sample_height(const HeightfieldView &hf, f32 u, f32 v) noexcept {
        const f32 x = u * static_cast<f32>(hf.width) - 0.5f;
        const f32 y = v * static_cast<f32>(hf.height) - 0.5f;
        const f32 fx = std::floor(x);
        const f32 fy = std::floor(y);
        const auto ix = static_cast<i32>(fx);
        const auto iy = static_cast<i32>(fy);
        return make_bilinear(hf.texel(ix, iy), hf.texel(ix + 1, iy), hf.texel(ix, iy + 1), hf.texel(ix + 1, iy + 1))
            .eval(x - fx, y - fy);
    }

    glm::vec2 sample_gradient(const HeightfieldView &hf, f32 u, f32 v) noexcept {
        const f32 x = u * static_cast<f32>(hf.width) - 0.5f;
        const f32 y = v * static_cast<f32>(hf.height) - 0.5f;
        const f32 fx = std::floor(x);
        const f32 fy = std::floor(y);
        const auto ix = static_cast<i32>(fx);
        const auto iy = static_cast<i32>(fy);
        const Bilinear patch =
            make_bilinear(hf.texel(ix, iy), hf.texel(ix + 1, iy), hf.texel(ix, iy + 1), hf.texel(ix + 1, iy + 1));
        const f32 fu = x - fx;
        const f32 fv = y - fy;
        return {(patch.b + patch.d * fv) * static_cast<f32>(hf.width), (patch.c + patch.d * fu) * static_cast<f32>(hf.height)};
    }

    glm::vec3 heightfield_normal(const glm::vec2 &gradient, f32 height_scale, const glm::vec2 &tile_size) noexcept {
        // z = scale * H(u, v) with u = x / size.x, so dz/dx = scale * dH/du / size.x.
        return glm::normalize(glm::vec3(-height_scale * gradient.x / tile_size.x, -height_scale * gradient.y / tile_size.y,
                                        1.0f));
    }

    HeightfieldRay make_view_ray(const glm::vec2 &uv, const glm::vec3 &view_ts, f32 height_scale,
                                 const glm::vec2 &tile_size, f32 reference_height) noexcept {
        // A grazing view has view.z -> 0, which sends the slab crossing (and the uv travel) to infinity;
        // bounding it keeps every downstream division finite.
        const f32 vz = std::max(view_ts.z, 1.0e-3f);
        const glm::vec3 dir(-view_ts.x / tile_size.x, -view_ts.y / tile_size.y, -vz / height_scale);
        // Back up from the base surface (height = reference) to the top of the slab; dir.z < 0, so t is <= 0.
        const f32 t_top = (1.0f - reference_height) / dir.z;
        HeightfieldRay ray;
        ray.origin = glm::vec3(uv.x + dir.x * t_top, uv.y + dir.y * t_top, 1.0f);
        ray.direction = dir;
        return ray;
    }

    HeightfieldHit trace_hierarchical(const HeightfieldView &heightfield, const HeightfieldHierarchy &hierarchy,
                                      const HeightfieldRay &ray, u32 max_steps, u32 start_level) noexcept {
        return trace_grid(heightfield, &hierarchy, ray, max_steps, start_level);
    }

    HeightfieldHit trace_cell_exact(const HeightfieldView &heightfield, const HeightfieldRay &ray,
                                    u32 max_steps) noexcept {
        return trace_grid(heightfield, nullptr, ray, max_steps);
    }

    HeightfieldHit trace_parallax_occlusion(const HeightfieldView &heightfield, const HeightfieldRay &ray, u32 steps,
                                            u32 refinements) noexcept {
        return march_and_bisect(heightfield, ray, steps, refinements);
    }

    HeightfieldHit trace_brute_force(const HeightfieldView &heightfield, const HeightfieldRay &ray,
                                     u32 samples) noexcept {
        return march_and_bisect(heightfield, ray, samples, 48);
    }

    bool trace_shadow(const HeightfieldView &heightfield, const HeightfieldHierarchy &hierarchy,
                      const glm::vec2 &surface_uv, f32 surface_height, const glm::vec3 &light_ts, f32 height_scale,
                      const glm::vec2 &tile_size, u32 max_steps, f32 bias, u32 start_level) noexcept {
        if (light_ts.z <= 0.0f) {
            return true; // light below the horizon of the base surface
        }
        HeightfieldRay ray;
        ray.origin = glm::vec3(surface_uv.x, surface_uv.y, surface_height + bias);
        ray.direction = glm::vec3(light_ts.x / tile_size.x, light_ts.y / tile_size.y, light_ts.z / height_scale);
        const HeightfieldHit hit = hierarchy.valid() ? trace_hierarchical(heightfield, hierarchy, ray, max_steps, start_level)
                                                     : trace_cell_exact(heightfield, ray, max_steps);
        return hit.status == HitStatus::Hit;
    }

} // namespace SFT::Renderer::Displacement
