#include <Animation/Ik.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace SFT::Animation {

    namespace {

        constexpr f32 kEps = 1e-6f;

        struct World {
            std::vector<glm::quat> rotation;
            std::vector<glm::vec3> position;
        };

        void compute_world(const Skeleton &skeleton, const Pose &pose, World &out) {
            const usize n = skeleton.joint_count();
            out.rotation.resize(n);
            out.position.resize(n);
            for (usize j = 0; j < n; ++j) {
                const u32 p = skeleton.parents[j];
                if (p == no_joint) {
                    out.rotation[j] = pose[j].rotation;
                    out.position[j] = pose[j].translation;
                } else {
                    out.rotation[j] = out.rotation[p] * pose[j].rotation;
                    out.position[j] = out.position[p] + out.rotation[p] * pose[j].translation;
                }
            }
        }

        // Shortest-arc rotation taking direction `from` to `to` (both need not be normalised).
        glm::quat from_to(glm::vec3 from, glm::vec3 to) {
            const f32 lf = glm::length(from), lt = glm::length(to);
            if (lf < kEps || lt < kEps) return glm::quat(1, 0, 0, 0);
            from /= lf;
            to /= lt;
            const f32 d = glm::dot(from, to);
            if (d > 1.0f - 1e-7f) return glm::quat(1, 0, 0, 0);
            if (d < -1.0f + 1e-7f) {
                glm::vec3 axis = glm::cross(from, glm::vec3(1, 0, 0));
                if (glm::length(axis) < 1e-3f) axis = glm::cross(from, glm::vec3(0, 1, 0));
                return glm::angleAxis(glm::pi<f32>(), glm::normalize(axis));
            }
            const glm::vec3 axis = glm::cross(from, to);
            return glm::normalize(glm::quat(1.0f + d, axis.x, axis.y, axis.z));
        }

        bool joint_ok(const Skeleton &skeleton, const Pose &pose, u32 j) {
            return j < skeleton.joint_count() && j < pose.size();
        }

    } // namespace

    bool solve_two_bone(const Skeleton &skeleton, Pose &pose, const TwoBoneIk &ik) {
        if (!joint_ok(skeleton, pose, ik.root) || !joint_ok(skeleton, pose, ik.mid) || !joint_ok(skeleton, pose, ik.tip) ||
            skeleton.parents[ik.mid] != ik.root || skeleton.parents[ik.tip] != ik.mid || ik.weight <= 0.0f) {
            return false;
        }
        World w;
        compute_world(skeleton, pose, w);
        const glm::vec3 a = w.position[ik.root], b = w.position[ik.mid], c = w.position[ik.tip];
        const f32 l1 = glm::length(b - a), l2 = glm::length(c - b);
        if (l1 < kEps || l2 < kEps) return false;

        glm::vec3 to_target = ik.target - a;
        const f32 distance = glm::length(to_target);
        if (distance < kEps) return false;
        const glm::vec3 u = to_target / distance;
        const f32 reach = l1 + l2;
        const f32 min_reach = std::fabs(l1 - l2);
        const bool reachable = distance <= reach && distance >= min_reach;
        const f32 d = std::clamp(distance, min_reach + 1e-4f, reach - 1e-4f);

        // Bend plane: spanned by the target direction and the pole (or the chain's current bend direction).
        glm::vec3 bend = glm::length(ik.pole) > kEps ? ik.pole : (b - a) - glm::dot(b - a, u) * u;
        bend -= glm::dot(bend, u) * u;
        if (glm::length(bend) < kEps) {
            // Straight chain with no pole: pick any perpendicular.
            bend = glm::cross(u, std::fabs(u.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0));
        }
        bend = glm::normalize(bend);

        const f32 cos_a = std::clamp((l1 * l1 + d * d - l2 * l2) / (2.0f * l1 * d), -1.0f, 1.0f);
        const f32 sin_a = std::sqrt(std::max(0.0f, 1.0f - cos_a * cos_a));
        const glm::vec3 new_b = a + l1 * (cos_a * u + sin_a * bend);
        const glm::vec3 new_t = a + u * d;

        const glm::quat upper_delta = from_to(b - a, new_b - a);
        const glm::vec3 c_after_upper = new_b + upper_delta * (c - b);
        const glm::quat mid_delta = from_to(c_after_upper - new_b, new_t - new_b);

        const u32 root_parent = skeleton.parents[ik.root];
        const glm::quat root_parent_rot = root_parent == no_joint ? glm::quat(1, 0, 0, 0) : w.rotation[root_parent];
        const glm::quat new_root_world = upper_delta * w.rotation[ik.root];
        const glm::quat new_mid_world = mid_delta * upper_delta * w.rotation[ik.mid];

        const glm::quat solved_root = glm::normalize(glm::inverse(root_parent_rot) * new_root_world);
        const glm::quat solved_mid = glm::normalize(glm::inverse(new_root_world) * new_mid_world);
        const f32 weight = std::min(ik.weight, 1.0f);
        pose[ik.root].rotation = glm::normalize(glm::slerp(pose[ik.root].rotation, solved_root, weight));
        pose[ik.mid].rotation = glm::normalize(glm::slerp(pose[ik.mid].rotation, solved_mid, weight));
        return reachable;
    }

    void solve_look_at(const Skeleton &skeleton, Pose &pose, const LookAtIk &ik) {
        if (!joint_ok(skeleton, pose, ik.joint) || ik.weight <= 0.0f) return;
        World w;
        compute_world(skeleton, pose, w);
        const glm::vec3 current = w.rotation[ik.joint] * glm::normalize(ik.forward_axis);
        glm::vec3 wanted = ik.target - w.position[ik.joint];
        if (glm::length(wanted) < kEps) return;
        wanted = glm::normalize(wanted);

        glm::quat delta = from_to(current, wanted);
        const f32 limit = glm::radians(std::clamp(ik.max_angle_degrees, 0.0f, 180.0f));
        const f32 angle = glm::angle(delta);
        if (angle > limit && angle > kEps) {
            delta = glm::angleAxis(limit, glm::axis(delta));
        }
        const u32 p = skeleton.parents[ik.joint];
        const glm::quat parent_rot = p == no_joint ? glm::quat(1, 0, 0, 0) : w.rotation[p];
        const glm::quat solved = glm::normalize(glm::inverse(parent_rot) * (delta * w.rotation[ik.joint]));
        pose[ik.joint].rotation = glm::normalize(glm::slerp(pose[ik.joint].rotation, solved, std::min(ik.weight, 1.0f)));
    }

    bool solve_chain(const Skeleton &skeleton, Pose &pose, const ChainIk &ik) {
        const usize n = ik.joints.size();
        if (n < 2 || ik.weight <= 0.0f) return false;
        for (usize i = 0; i < n; ++i) {
            if (!joint_ok(skeleton, pose, ik.joints[i])) return false;
            if (i > 0 && skeleton.parents[ik.joints[i]] != ik.joints[i - 1]) return false;
        }
        World w;
        compute_world(skeleton, pose, w);
        std::vector<glm::vec3> original(n), p(n);
        std::vector<f32> length(n - 1);
        for (usize i = 0; i < n; ++i) original[i] = p[i] = w.position[ik.joints[i]];
        f32 total = 0.0f;
        for (usize i = 0; i + 1 < n; ++i) {
            length[i] = glm::length(original[i + 1] - original[i]);
            total += length[i];
        }
        const glm::vec3 root = p[0];
        const bool reachable = glm::length(ik.target - root) <= total;

        if (!reachable) {
            const glm::vec3 dir = glm::normalize(ik.target - root);
            for (usize i = 1; i < n; ++i) p[i] = p[i - 1] + dir * length[i - 1];
        } else {
            for (u32 it = 0; it < ik.iterations; ++it) {
                if (glm::length(p[n - 1] - ik.target) < ik.tolerance) break;
                p[n - 1] = ik.target; // backward
                for (usize i = n - 1; i-- > 0;) {
                    p[i] = p[i + 1] + glm::normalize(p[i] - p[i + 1]) * length[i];
                }
                p[0] = root; // forward
                for (usize i = 1; i < n; ++i) {
                    p[i] = p[i - 1] + glm::normalize(p[i] - p[i - 1]) * length[i - 1];
                }
            }
        }

        // Aim each bone along its solved segment; the tip keeps the orientation its parent gives it.
        std::vector<glm::quat> solved_world(n);
        for (usize i = 0; i < n; ++i) solved_world[i] = w.rotation[ik.joints[i]];
        for (usize i = 0; i + 1 < n; ++i) {
            solved_world[i] = from_to(original[i + 1] - original[i], p[i + 1] - p[i]) * w.rotation[ik.joints[i]];
        }
        const f32 weight = std::min(ik.weight, 1.0f);
        glm::quat parent_world = skeleton.parents[ik.joints[0]] == no_joint ? glm::quat(1, 0, 0, 0)
                                                                          : w.rotation[skeleton.parents[ik.joints[0]]];
        for (usize i = 0; i + 1 < n; ++i) {
            const glm::quat local = glm::normalize(glm::inverse(parent_world) * solved_world[i]);
            JointTransform &joint = pose[ik.joints[i]];
            joint.rotation = glm::normalize(glm::slerp(joint.rotation, local, weight));
            parent_world = parent_world * joint.rotation;
        }
        return reachable;
    }

} // namespace SFT::Animation
