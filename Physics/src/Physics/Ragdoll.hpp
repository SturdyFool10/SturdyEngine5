#pragma once

#include <Physics/SwingTwist.hpp>

#include <Animation/Retarget.hpp>
#include <Animation/Skeleton.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <vector>

namespace SFT::Physics {

    inline constexpr u32 no_body = 0xFFFFFFFFu;

    /// One rigid capsule of a ragdoll. The body's frame is its skeleton joint's frame, so a solver can start it at
    /// `rest_rotation` and read its world rotation straight back into the pose.
    struct RagdollBody {
        u32 joint = Animation::no_joint;     // skeleton joint this body drives
        u32 parent_body = no_body;           // body of the nearest simulated ancestor
        Animation::HumanoidBone bone = Animation::HumanoidBone::Hips;
        f32 mass = 1.0f;
        f32 radius = 0.05f;
        f32 length = 0.2f;                   // tip-to-tip segment length (capsule core, excluding caps)
        glm::vec3 center_offset{0.0f};       // capsule centre relative to the joint, in the joint's frame
        glm::vec3 axis{1.0f, 0.0f, 0.0f};    // capsule axis in the joint's frame (unit)
        glm::vec3 rest_position{0.0f};       // joint position in model space at rest
        glm::quat rest_rotation{1.0f, 0.0f, 0.0f, 0.0f}; // joint orientation in model space at rest
    };

    enum class RagdollJointKind : u8 { Ball, Hinge };

    struct RagdollJoint {
        u32 body = no_body;                  // the child body this joint attaches
        u32 parent_body = no_body;
        RagdollJointKind kind = RagdollJointKind::Ball;
        /// Anchor on the parent body (its frame) where the child's origin is attached.
        glm::vec3 anchor_in_parent{0.0f};
        /// Ball joints: swing/twist limits in the child's frame, relative to the parent.
        SwingTwistLimits limits;
        /// Hinge joints: rotation axis in the child's frame and its angle range in radians.
        glm::vec3 hinge_axis{1.0f, 0.0f, 0.0f};
        f32 hinge_min = 0.0f;
        f32 hinge_max = 0.0f;
        /// Active-ragdoll motor gains (see `pd_drive`): pull toward the animated pose.
        f32 drive_stiffness = 40.0f;
        f32 drive_damping = 4.0f;
    };

    struct RagdollDef {
        std::vector<RagdollBody> bodies;
        std::vector<RagdollJoint> joints; // one per non-root body, parent-first
        f32 total_mass = 0.0f;

        [[nodiscard]] u32 body_for_joint(u32 joint) const noexcept;
    };

    struct RagdollOptions {
        f32 total_mass = 75.0f;
        /// Capsule radius as a fraction of segment length for limbs/torso.
        f32 limb_radius_ratio = 0.2f;
        f32 torso_radius_ratio = 0.35f;
    };

    /// Builds capsules, joints, limits and masses for a humanoid skeleton from its rest pose. Needs
    /// `map.usable()`; returns an empty definition otherwise. Mass follows standard anthropometric fractions, bone
    /// lengths come from the rig, and limits are human-plausible defaults a game can tune afterwards.
    [[nodiscard]] RagdollDef build_humanoid_ragdoll(const Animation::Skeleton &skeleton, const Animation::HumanoidMap &map,
                                                    const RagdollOptions &options = {});

    /// Writes simulated body orientations (model space, one per `def.bodies` entry) into `pose`: every driven joint
    /// takes its body's rotation relative to its skeleton parent, all other joints keep what `pose` already holds.
    void apply_body_rotations(const Animation::Skeleton &skeleton, const RagdollDef &def,
                              const std::vector<glm::quat> &body_rotations, Animation::Pose &pose);

    /// Model-space orientation each body should have to match `pose` (the targets a drive or a kinematic-follow
    /// tracks), one per `def.bodies` entry.
    [[nodiscard]] std::vector<glm::quat> target_body_rotations(const Animation::Skeleton &skeleton, const RagdollDef &def,
                                                               const Animation::Pose &pose);

} // namespace SFT::Physics
