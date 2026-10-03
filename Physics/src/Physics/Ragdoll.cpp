#include <Physics/Ragdoll.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>

namespace SFT::Physics {

    using Animation::HumanoidBone;

    u32 RagdollDef::body_for_joint(u32 joint) const noexcept {
        for (usize i = 0; i < bodies.size(); ++i) {
            if (bodies[i].joint == joint) {
                return static_cast<u32>(i);
            }
        }
        return no_body;
    }

    namespace {

        struct World {
            std::vector<glm::quat> rotation;
            std::vector<glm::vec3> position;
        };

        World compute_world(const Animation::Skeleton &skeleton, const Animation::Pose &pose) {
            World w;
            const usize n = skeleton.joint_count();
            w.rotation.resize(n);
            w.position.resize(n);
            for (usize j = 0; j < n; ++j) {
                const u32 p = skeleton.parents[j];
                if (p == Animation::no_joint) {
                    w.rotation[j] = pose[j].rotation;
                    w.position[j] = pose[j].translation;
                } else {
                    w.rotation[j] = w.rotation[p] * pose[j].rotation;
                    w.position[j] = w.position[p] + w.rotation[p] * pose[j].translation;
                }
            }
            return w;
        }

        // Per-bone tuning: mass fraction, the bone that continues the segment, candidate parents (first present wins),
        // and the joint behaviour.
        struct BoneSpec {
            HumanoidBone bone;
            f32 mass_fraction;
            HumanoidBone child; // segment end; Count = estimate from body height
            f32 tip_length;     // fraction of height when there is no child
            std::array<HumanoidBone, 4> parents;
            RagdollJointKind kind;
            f32 swing_y, swing_z, twist_min, twist_max;
            f32 hinge_min, hinge_max;
            glm::vec3 hinge_world_axis; // used for hinges, in model space at rest
            bool torso;
        };

        constexpr HumanoidBone NoBone = HumanoidBone::Count;

        const BoneSpec kSpecs[] = {
            {HumanoidBone::Hips, 0.14f, HumanoidBone::Spine, 0.0f, {NoBone, NoBone, NoBone, NoBone}, RagdollJointKind::Ball, 0, 0, 0, 0, 0, 0, {1, 0, 0}, true},
            {HumanoidBone::Spine, 0.12f, HumanoidBone::Chest, 0.0f, {HumanoidBone::Hips, NoBone, NoBone, NoBone}, RagdollJointKind::Ball, 0.35f, 0.35f, -0.3f, 0.3f, 0, 0, {1, 0, 0}, true},
            {HumanoidBone::Chest, 0.20f, HumanoidBone::UpperChest, 0.0f, {HumanoidBone::Spine, HumanoidBone::Hips, NoBone, NoBone}, RagdollJointKind::Ball, 0.35f, 0.35f, -0.3f, 0.3f, 0, 0, {1, 0, 0}, true},
            {HumanoidBone::UpperChest, 0.0f, HumanoidBone::Neck, 0.0f, {HumanoidBone::Chest, HumanoidBone::Spine, HumanoidBone::Hips, NoBone}, RagdollJointKind::Ball, 0.3f, 0.3f, -0.25f, 0.25f, 0, 0, {1, 0, 0}, true},
            {HumanoidBone::Neck, 0.02f, HumanoidBone::Head, 0.0f, {HumanoidBone::UpperChest, HumanoidBone::Chest, HumanoidBone::Spine, HumanoidBone::Hips}, RagdollJointKind::Ball, 0.5f, 0.5f, -0.6f, 0.6f, 0, 0, {1, 0, 0}, false},
            {HumanoidBone::Head, 0.07f, NoBone, 0.13f, {HumanoidBone::Neck, HumanoidBone::UpperChest, HumanoidBone::Chest, HumanoidBone::Spine}, RagdollJointKind::Ball, 0.6f, 0.6f, -0.8f, 0.8f, 0, 0, {1, 0, 0}, false},

            {HumanoidBone::LeftShoulder, 0.01f, HumanoidBone::LeftUpperArm, 0.0f, {HumanoidBone::UpperChest, HumanoidBone::Chest, HumanoidBone::Spine, HumanoidBone::Hips}, RagdollJointKind::Ball, 0.3f, 0.3f, -0.2f, 0.2f, 0, 0, {1, 0, 0}, false},
            {HumanoidBone::LeftUpperArm, 0.027f, HumanoidBone::LeftLowerArm, 0.0f, {HumanoidBone::LeftShoulder, HumanoidBone::UpperChest, HumanoidBone::Chest, HumanoidBone::Spine}, RagdollJointKind::Ball, 1.6f, 1.2f, -1.2f, 1.2f, 0, 0, {1, 0, 0}, false},
            {HumanoidBone::LeftLowerArm, 0.016f, HumanoidBone::LeftHand, 0.0f, {HumanoidBone::LeftUpperArm, NoBone, NoBone, NoBone}, RagdollJointKind::Hinge, 0, 0, 0, 0, 0.0f, 2.4f, {0, 1, 0}, false},
            {HumanoidBone::LeftHand, 0.006f, NoBone, 0.10f, {HumanoidBone::LeftLowerArm, NoBone, NoBone, NoBone}, RagdollJointKind::Ball, 0.8f, 0.6f, -0.4f, 0.4f, 0, 0, {1, 0, 0}, false},
            {HumanoidBone::RightShoulder, 0.01f, HumanoidBone::RightUpperArm, 0.0f, {HumanoidBone::UpperChest, HumanoidBone::Chest, HumanoidBone::Spine, HumanoidBone::Hips}, RagdollJointKind::Ball, 0.3f, 0.3f, -0.2f, 0.2f, 0, 0, {1, 0, 0}, false},
            {HumanoidBone::RightUpperArm, 0.027f, HumanoidBone::RightLowerArm, 0.0f, {HumanoidBone::RightShoulder, HumanoidBone::UpperChest, HumanoidBone::Chest, HumanoidBone::Spine}, RagdollJointKind::Ball, 1.6f, 1.2f, -1.2f, 1.2f, 0, 0, {1, 0, 0}, false},
            {HumanoidBone::RightLowerArm, 0.016f, HumanoidBone::RightHand, 0.0f, {HumanoidBone::RightUpperArm, NoBone, NoBone, NoBone}, RagdollJointKind::Hinge, 0, 0, 0, 0, -2.4f, 0.0f, {0, 1, 0}, false},
            {HumanoidBone::RightHand, 0.006f, NoBone, 0.10f, {HumanoidBone::RightLowerArm, NoBone, NoBone, NoBone}, RagdollJointKind::Ball, 0.8f, 0.6f, -0.4f, 0.4f, 0, 0, {1, 0, 0}, false},

            {HumanoidBone::LeftUpperLeg, 0.10f, HumanoidBone::LeftLowerLeg, 0.0f, {HumanoidBone::Hips, NoBone, NoBone, NoBone}, RagdollJointKind::Ball, 1.5f, 0.8f, -0.5f, 0.5f, 0, 0, {1, 0, 0}, false},
            {HumanoidBone::LeftLowerLeg, 0.047f, HumanoidBone::LeftFoot, 0.0f, {HumanoidBone::LeftUpperLeg, NoBone, NoBone, NoBone}, RagdollJointKind::Hinge, 0, 0, 0, 0, -2.4f, 0.0f, {1, 0, 0}, false},
            {HumanoidBone::LeftFoot, 0.014f, HumanoidBone::LeftToes, 0.15f, {HumanoidBone::LeftLowerLeg, NoBone, NoBone, NoBone}, RagdollJointKind::Ball, 0.6f, 0.4f, -0.3f, 0.3f, 0, 0, {1, 0, 0}, false},
            {HumanoidBone::LeftToes, 0.003f, NoBone, 0.05f, {HumanoidBone::LeftFoot, NoBone, NoBone, NoBone}, RagdollJointKind::Hinge, 0, 0, 0, 0, -0.6f, 0.6f, {1, 0, 0}, false},
            {HumanoidBone::RightUpperLeg, 0.10f, HumanoidBone::RightLowerLeg, 0.0f, {HumanoidBone::Hips, NoBone, NoBone, NoBone}, RagdollJointKind::Ball, 1.5f, 0.8f, -0.5f, 0.5f, 0, 0, {1, 0, 0}, false},
            {HumanoidBone::RightLowerLeg, 0.047f, HumanoidBone::RightFoot, 0.0f, {HumanoidBone::RightUpperLeg, NoBone, NoBone, NoBone}, RagdollJointKind::Hinge, 0, 0, 0, 0, -2.4f, 0.0f, {1, 0, 0}, false},
            {HumanoidBone::RightFoot, 0.014f, HumanoidBone::RightToes, 0.15f, {HumanoidBone::RightLowerLeg, NoBone, NoBone, NoBone}, RagdollJointKind::Ball, 0.6f, 0.4f, -0.3f, 0.3f, 0, 0, {1, 0, 0}, false},
            {HumanoidBone::RightToes, 0.003f, NoBone, 0.05f, {HumanoidBone::RightFoot, NoBone, NoBone, NoBone}, RagdollJointKind::Hinge, 0, 0, 0, 0, -0.6f, 0.6f, {1, 0, 0}, false},
        };

    } // namespace

    RagdollDef build_humanoid_ragdoll(const Animation::Skeleton &skeleton, const Animation::HumanoidMap &map,
                                      const RagdollOptions &options) {
        RagdollDef def;
        if (!map.usable() || !skeleton.valid()) {
            return def;
        }
        const World rest = compute_world(skeleton, skeleton.rest_pose);

        // Body height from the rig: head (or hips) down to the lowest foot.
        const u32 top_joint = map[HumanoidBone::Head] != Animation::no_joint ? map[HumanoidBone::Head] : map[HumanoidBone::Hips];
        f32 lowest = rest.position[map[HumanoidBone::Hips]].y;
        for (HumanoidBone foot : {HumanoidBone::LeftFoot, HumanoidBone::RightFoot}) {
            lowest = std::min(lowest, rest.position[map[foot]].y);
        }
        const f32 height = std::max(rest.position[top_joint].y - lowest, 0.5f);

        // Pass 1: bodies for every mapped bone, in table order (which is parent-first).
        std::array<u32, Animation::humanoid_bone_count> body_of_bone;
        body_of_bone.fill(no_body);
        f32 mass_sum = 0.0f;
        for (const BoneSpec &spec : kSpecs) {
            const u32 joint = map[spec.bone];
            if (joint == Animation::no_joint || spec.mass_fraction <= 0.0f) {
                continue;
            }
            RagdollBody body;
            body.joint = joint;
            body.bone = spec.bone;
            body.rest_position = rest.position[joint];
            body.rest_rotation = rest.rotation[joint];

            glm::vec3 axis_model(0.0f, -1.0f, 0.0f);
            f32 length = spec.tip_length * height;
            const u32 child_joint = spec.child == NoBone ? Animation::no_joint : map[spec.child];
            if (child_joint != Animation::no_joint) {
                const glm::vec3 to_child = rest.position[child_joint] - rest.position[joint];
                length = glm::length(to_child);
                if (length > 1e-5f) {
                    axis_model = to_child / length;
                }
            } else {
                // No child: continue in the direction the parent bone points (hands, feet, head), upward for the head.
                const u32 parent_joint = skeleton.parents[joint];
                if (parent_joint != Animation::no_joint) {
                    const glm::vec3 from_parent = rest.position[joint] - rest.position[parent_joint];
                    if (glm::length(from_parent) > 1e-5f) {
                        axis_model = glm::normalize(from_parent);
                    }
                }
            }
            length = std::max(length, 0.02f);
            body.length = length;
            body.axis = glm::normalize(glm::inverse(body.rest_rotation) * axis_model);
            body.center_offset = body.axis * (length * 0.5f);
            const f32 ratio = spec.torso ? options.torso_radius_ratio : options.limb_radius_ratio;
            body.radius = std::clamp(length * ratio, 0.02f, 0.25f * height);
            body.mass = spec.mass_fraction;
            mass_sum += spec.mass_fraction;
            body_of_bone[static_cast<usize>(spec.bone)] = static_cast<u32>(def.bodies.size());
            def.bodies.push_back(body);
        }
        if (def.bodies.empty() || mass_sum <= 0.0f) {
            return RagdollDef{};
        }
        for (RagdollBody &body : def.bodies) {
            body.mass = options.total_mass * body.mass / mass_sum;
        }
        def.total_mass = options.total_mass;

        // Pass 2: parent bodies and joints.
        for (const BoneSpec &spec : kSpecs) {
            const u32 body_index = body_of_bone[static_cast<usize>(spec.bone)];
            if (body_index == no_body) {
                continue;
            }
            RagdollBody &body = def.bodies[body_index];
            u32 parent_body = no_body;
            for (HumanoidBone candidate : spec.parents) {
                if (candidate != NoBone && body_of_bone[static_cast<usize>(candidate)] != no_body) {
                    parent_body = body_of_bone[static_cast<usize>(candidate)];
                    break;
                }
            }
            body.parent_body = parent_body;
            if (parent_body == no_body) {
                continue;
            }
            const RagdollBody &parent = def.bodies[parent_body];
            RagdollJoint joint;
            joint.body = body_index;
            joint.parent_body = parent_body;
            joint.kind = spec.kind;
            joint.anchor_in_parent = glm::inverse(parent.rest_rotation) * (body.rest_position - parent.rest_position);
            if (spec.kind == RagdollJointKind::Ball) {
                joint.limits.axis = body.axis;
                joint.limits.swing_y = spec.swing_y;
                joint.limits.swing_z = spec.swing_z;
                joint.limits.twist_min = spec.twist_min;
                joint.limits.twist_max = spec.twist_max;
            } else {
                joint.hinge_axis = glm::normalize(glm::inverse(body.rest_rotation) * spec.hinge_world_axis);
                joint.hinge_min = spec.hinge_min;
                joint.hinge_max = spec.hinge_max;
            }
            def.joints.push_back(joint);
        }
        return def;
    }

    std::vector<glm::quat> target_body_rotations(const Animation::Skeleton &skeleton, const RagdollDef &def,
                                                 const Animation::Pose &pose) {
        const World world = compute_world(skeleton, pose);
        std::vector<glm::quat> out;
        out.reserve(def.bodies.size());
        for (const RagdollBody &body : def.bodies) {
            out.push_back(world.rotation[body.joint]);
        }
        return out;
    }

    void apply_body_rotations(const Animation::Skeleton &skeleton, const RagdollDef &def,
                              const std::vector<glm::quat> &body_rotations, Animation::Pose &pose) {
        const usize n = skeleton.joint_count();
        std::vector<u32> body_of_joint(n, no_body);
        for (usize b = 0; b < def.bodies.size() && b < body_rotations.size(); ++b) {
            body_of_joint[def.bodies[b].joint] = static_cast<u32>(b);
        }
        std::vector<glm::quat> world(n);
        for (usize j = 0; j < n; ++j) {
            const u32 p = skeleton.parents[j];
            const glm::quat parent_world = p == Animation::no_joint ? glm::quat(1, 0, 0, 0) : world[p];
            if (body_of_joint[j] != no_body) {
                world[j] = glm::normalize(body_rotations[body_of_joint[j]]);
                pose[j].rotation = glm::normalize(glm::inverse(parent_world) * world[j]);
            } else {
                world[j] = parent_world * pose[j].rotation;
            }
        }
    }

} // namespace SFT::Physics
