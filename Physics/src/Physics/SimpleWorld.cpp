#include <Physics/SimpleWorld.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace SFT::Physics {

    bool SimpleWorld::sweep_sphere(const glm::vec3 &center, f32 radius, const glm::vec3 &delta, SweepHit &hit) const {
        bool found = false;
        f32 best = std::numeric_limits<f32>::max();

        for (const Plane &plane : planes_) {
            const f32 distance = glm::dot(center, plane.normal) - plane.offset;
            const f32 approach = glm::dot(delta, plane.normal);
            if (approach >= 0.0f) {
                continue;
            }
            const f32 t = distance <= radius ? 0.0f : (radius - distance) / approach;
            if (t >= 0.0f && t <= 1.0f && t < best) {
                best = t;
                hit = SweepHit{t, plane.normal, center + delta * t - plane.normal * radius, plane.object, glm::vec3(0.0f)};
                found = true;
            }
        }

        for (const Box &box : boxes_) {
            const glm::vec3 lo = box.min - glm::vec3(radius), hi = box.max + glm::vec3(radius);
            f32 t_near = 0.0f, t_far = 1.0f;
            glm::vec3 normal(0.0f);
            bool miss = false;
            for (int axis = 0; axis < 3 && !miss; ++axis) {
                const f32 o = center[axis], d = delta[axis];
                if (std::fabs(d) < 1e-9f) {
                    miss = o < lo[axis] || o > hi[axis];
                    continue;
                }
                f32 t0 = (lo[axis] - o) / d, t1 = (hi[axis] - o) / d;
                f32 sign = -1.0f; // entering through the low face means the normal points toward -axis
                if (t0 > t1) {
                    std::swap(t0, t1);
                    sign = 1.0f;
                }
                if (t0 > t_near) {
                    t_near = t0;
                    normal = glm::vec3(0.0f);
                    normal[axis] = sign;
                }
                t_far = std::min(t_far, t1);
                if (t_near > t_far) {
                    miss = true;
                }
            }
            if (miss) {
                continue;
            }
            if (glm::dot(normal, normal) == 0.0f) {
                // Started inside the expanded box: escape through the nearest face.
                f32 least = std::numeric_limits<f32>::max();
                for (int axis = 0; axis < 3; ++axis) {
                    const f32 to_low = center[axis] - lo[axis], to_high = hi[axis] - center[axis];
                    if (to_low < least) {
                        least = to_low;
                        normal = glm::vec3(0.0f);
                        normal[axis] = -1.0f;
                    }
                    if (to_high < least) {
                        least = to_high;
                        normal = glm::vec3(0.0f);
                        normal[axis] = 1.0f;
                    }
                }
                t_near = 0.0f;
            }
            if (t_near < best) {
                best = t_near;
                hit = SweepHit{t_near, normal, center + delta * t_near - normal * radius, box.object, box.velocity};
                found = true;
            }
        }
        return found;
    }

    bool SimpleWorld::sweep_capsule(const glm::vec3 &feet, f32 radius, f32 height, const glm::vec3 &delta, SweepHit &hit) const {
        const f32 top = std::max(height - radius, radius);
        const std::array<f32, 3> heights{radius, (radius + top) * 0.5f, top};
        bool found = false;
        for (f32 h : heights) {
            SweepHit candidate;
            if (sweep_sphere(feet + glm::vec3(0.0f, h, 0.0f), radius, delta, candidate) && (!found || candidate.fraction < hit.fraction)) {
                hit = candidate;
                found = true;
            }
        }
        return found;
    }

    bool SimpleWorld::overlaps_capsule(const glm::vec3 &feet, f32 radius, f32 height) const {
        const f32 top = std::max(height - radius, radius);
        const std::array<f32, 3> heights{radius, (radius + top) * 0.5f, top};
        for (f32 h : heights) {
            const glm::vec3 c = feet + glm::vec3(0.0f, h, 0.0f);
            for (const Plane &p : planes_) {
                if (glm::dot(c, p.normal) - p.offset < radius - 1e-5f) {
                    return true;
                }
            }
            for (const Box &b : boxes_) {
                const glm::vec3 nearest = glm::clamp(c, b.min, b.max);
                if (glm::dot(c - nearest, c - nearest) < (radius - 1e-5f) * (radius - 1e-5f)) {
                    return true;
                }
            }
        }
        return false;
    }

} // namespace SFT::Physics
