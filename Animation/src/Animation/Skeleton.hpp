#pragma once

#include <Foundation/Foundation.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace SFT::Animation {

    inline constexpr u32 no_joint = 0xFFFFFFFFu;

    /// Local transform of one joint relative to its parent.
    struct JointTransform {
        glm::vec3 translation{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};

        [[nodiscard]] glm::mat4 to_matrix() const;
    };

    /// Joint hierarchy in structure-of-arrays form. Joints are stored parent-first: `parents[i] < i`
    /// for every non-root joint, so model-space evaluation is one forward pass.
    struct Skeleton {
        std::vector<std::string> names;
        std::vector<u32> parents;
        std::vector<JointTransform> rest_pose;
        std::vector<glm::mat4> inverse_bind;

        [[nodiscard]] usize joint_count() const noexcept { return parents.size(); }
        [[nodiscard]] u32 find_joint(std::string_view name) const noexcept;

        /// True when the arrays agree in length and the hierarchy is parent-first.
        [[nodiscard]] bool valid() const noexcept;
    };

    /// One local transform per joint.
    using Pose = std::vector<JointTransform>;

    [[nodiscard]] Pose rest_pose_of(const Skeleton &skeleton);

    /// Interpolates `a` toward `b` (nlerp for rotation, shortest path).
    [[nodiscard]] JointTransform blend(const JointTransform &a, const JointTransform &b, f32 weight);

    /// One joint of `apply_additive`: `base` plus the delta `additive` makes against `reference`, scaled by `weight`.
    [[nodiscard]] JointTransform add_joint(const JointTransform &base, const JointTransform &additive,
                                           const JointTransform &reference, f32 weight);

    /// Per-joint blend of two equally sized poses into `out`.
    void blend_poses(const Pose &a, const Pose &b, f32 weight, Pose &out);

    /// Adds `additive` (a delta from `reference`) on top of `base` with the given weight.
    void apply_additive(const Pose &base, const Pose &additive, const Pose &reference, f32 weight, Pose &out);

    /// Model-space matrix of every joint (parent-first forward pass).
    void local_to_model(const Skeleton &skeleton, const Pose &pose, std::vector<glm::mat4> &model);

    /// Final skinning matrices: model-space joint matrix times the inverse bind matrix.
    void skin_matrices(const Skeleton &skeleton, const Pose &pose, std::vector<glm::mat4> &out);

} // namespace SFT::Animation
