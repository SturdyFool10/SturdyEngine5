#pragma once

#include <Animation/Skeleton.hpp>

#include <glm/vec3.hpp>

#include <span>
#include <vector>

/// Inverse-kinematics solvers. They edit the local rotations of a `Pose` in place so a joint chain reaches a
/// model-space target, and blend with the incoming pose by `weight` (0 = untouched, 1 = solved). Run them after the
/// animation graph and before `skin_matrices`; targets typically come from gameplay (a ground hit for a foot, a
/// ledge for a hand, a camera or object for a head).
namespace SFT::Animation {

    struct TwoBoneIk {
        u32 root = no_joint; // thigh / upper arm
        u32 mid = no_joint;  // knee / elbow, child of root
        u32 tip = no_joint;  // ankle / wrist, child of mid
        glm::vec3 target{0.0f};
        /// Direction the knee/elbow should bend toward (model space), e.g. forward for a knee. Zero = keep the
        /// current bend plane.
        glm::vec3 pole{0.0f};
        f32 weight = 1.0f;
    };

    /// Analytic two-bone solve (legs, arms). Returns true when the target was within reach; out-of-reach targets
    /// stretch the chain straight toward them without lengthening it. Bone lengths are taken from the pose.
    bool solve_two_bone(const Skeleton &skeleton, Pose &pose, const TwoBoneIk &ik);

    struct LookAtIk {
        u32 joint = no_joint;
        /// Which local axis of the joint should face the target (default +Z, glTF/Blender style forward).
        glm::vec3 forward_axis{0.0f, 0.0f, 1.0f};
        glm::vec3 target{0.0f};
        f32 weight = 1.0f;
        /// Largest deviation from the animated direction in degrees (head/neck limits).
        f32 max_angle_degrees = 180.0f;
    };

    void solve_look_at(const Skeleton &skeleton, Pose &pose, const LookAtIk &ik);

    struct ChainIk {
        /// Parent-to-child joints, each the child of the previous; the last is the tip.
        std::vector<u32> joints;
        glm::vec3 target{0.0f};
        f32 weight = 1.0f;
        u32 iterations = 12;
        f32 tolerance = 1e-3f;
    };

    /// FABRIK for longer chains (spines, tails, tentacles, ropes). The first joint stays put.
    bool solve_chain(const Skeleton &skeleton, Pose &pose, const ChainIk &ik);

} // namespace SFT::Animation
