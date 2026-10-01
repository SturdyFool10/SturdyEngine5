#include <Animation/Ik.hpp>
#include <Animation/Retarget.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <iostream>

using namespace SFT::Animation;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    }
    bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

    // root -> a -> b -> c, each 1 unit along +X.
    Skeleton make_arm() {
        Skeleton s;
        s.names = {"root", "a", "b", "c"};
        s.parents = {no_joint, 0, 1, 2};
        s.rest_pose.assign(4, JointTransform{});
        s.rest_pose[1].translation = {0, 0, 0};
        s.rest_pose[2].translation = {1, 0, 0};
        s.rest_pose[3].translation = {1, 0, 0};
        s.inverse_bind.assign(4, glm::mat4(1.0f));
        return s;
    }

    glm::vec3 joint_position(const Skeleton &s, const Pose &p, u32 j) {
        std::vector<glm::mat4> m;
        local_to_model(s, p, m);
        return glm::vec3(m[j][3]);
    }

    Skeleton make_humanoid(const std::vector<std::string> &names) {
        Skeleton s;
        s.names = names;
        s.parents.assign(names.size(), no_joint);
        for (usize i = 1; i < names.size(); ++i) s.parents[i] = 0; // flat is enough for naming tests
        s.rest_pose.assign(names.size(), JointTransform{});
        s.inverse_bind.assign(names.size(), glm::mat4(1.0f));
        return s;
    }
} // namespace

int main() {
    // ---- two-bone IK -------------------------------------------------------------------------
    {
        const Skeleton arm = make_arm();
        Pose pose = arm.rest_pose;
        TwoBoneIk ik{.root = 1, .mid = 2, .tip = 3, .target = {1.0f, 1.0f, 0.0f}, .pole = {0, 0, 1}};
        const bool reached = solve_two_bone(arm, pose, ik);
        check(reached, "target within reach");
        const glm::vec3 tip = joint_position(arm, pose, 3);
        check(near(tip.x, 1.0f) && near(tip.y, 1.0f) && near(tip.z, 0.0f), "tip reaches the target");
        const glm::vec3 mid = joint_position(arm, pose, 2);
        check(near(glm::length(mid - joint_position(arm, pose, 1)), 1.0f), "upper bone keeps its length");
        check(mid.z > 0.1f, "elbow bends toward the pole");

        Pose far = arm.rest_pose;
        ik.target = {10, 0, 0};
        check(!solve_two_bone(arm, far, ik), "unreachable target reported");
        check(near(joint_position(arm, far, 3).x, 2.0f), "unreachable target leaves the chain straight");

        Pose half = arm.rest_pose;
        ik.target = {1.0f, 1.0f, 0.0f};
        ik.weight = 0.0f;
        (void)solve_two_bone(arm, half, ik);
        check(near(joint_position(arm, half, 3).x, 2.0f), "weight 0 leaves the pose alone");
    }

    // ---- look-at -----------------------------------------------------------------------------
    {
        const Skeleton arm = make_arm();
        Pose pose = arm.rest_pose;
        solve_look_at(arm, pose, LookAtIk{.joint = 1, .forward_axis = {1, 0, 0}, .target = {0, 5, 0}});
        const glm::vec3 c = joint_position(arm, pose, 3);
        check(near(c.x, 0.0f) && c.y > 1.9f, "chain now points up");

        Pose limited = arm.rest_pose;
        solve_look_at(arm, limited, LookAtIk{.joint = 1, .forward_axis = {1, 0, 0}, .target = {0, 5, 0}, .max_angle_degrees = 30.0f});
        const glm::vec3 d = glm::normalize(joint_position(arm, limited, 3));
        check(near(glm::degrees(std::acos(d.x)), 30.0f, 0.5f), "look-at honours the angle limit");
    }

    // ---- FABRIK ------------------------------------------------------------------------------
    {
        const Skeleton arm = make_arm();
        Pose pose = arm.rest_pose;
        ChainIk chain{.joints = {1, 2, 3}, .target = {1.0f, 1.0f, 0.0f}};
        check(solve_chain(arm, pose, chain), "chain reaches");
        const glm::vec3 tip = joint_position(arm, pose, 3);
        check(near(tip.x, 1.0f, 0.02f) && near(tip.y, 1.0f, 0.02f), "FABRIK tip on target");
    }

    // ---- humanoid name mapping ---------------------------------------------------------------
    {
        const Skeleton mixamo = make_humanoid({"mixamorig:Hips", "mixamorig:Spine", "mixamorig:Spine1", "mixamorig:Spine2",
            "mixamorig:Neck", "mixamorig:Head", "mixamorig:LeftShoulder", "mixamorig:LeftArm", "mixamorig:LeftForeArm",
            "mixamorig:LeftHand", "mixamorig:RightShoulder", "mixamorig:RightArm", "mixamorig:RightForeArm",
            "mixamorig:RightHand", "mixamorig:LeftUpLeg", "mixamorig:LeftLeg", "mixamorig:LeftFoot", "mixamorig:LeftToeBase",
            "mixamorig:RightUpLeg", "mixamorig:RightLeg", "mixamorig:RightFoot", "mixamorig:RightToeBase"});
        const HumanoidMap m = guess_humanoid_map(mixamo);
        check(m[HumanoidBone::Hips] == 0 && m[HumanoidBone::Head] == 5, "mixamo hips/head");
        check(m[HumanoidBone::LeftUpperArm] == 7 && m[HumanoidBone::LeftLowerArm] == 8 && m[HumanoidBone::RightHand] == 13,
              "mixamo arms");
        check(m[HumanoidBone::LeftUpperLeg] == 14 && m[HumanoidBone::RightFoot] == 20, "mixamo legs (UpLeg / Leg)");
        check(m[HumanoidBone::Chest] == 2 && m[HumanoidBone::UpperChest] == 3, "mixamo spine chain");
        check(m.usable(), "mixamo rig is usable");

        const Skeleton ue = make_humanoid({"pelvis", "spine_01", "spine_02", "neck_01", "head", "clavicle_l", "upperarm_l",
            "lowerarm_l", "hand_l", "clavicle_r", "upperarm_r", "lowerarm_r", "hand_r", "thigh_l", "calf_l", "foot_l",
            "ball_l", "thigh_r", "calf_r", "foot_r", "ball_r"});
        const HumanoidMap u = guess_humanoid_map(ue);
        check(u.usable() && u[HumanoidBone::LeftUpperArm] == 6 && u[HumanoidBone::RightLowerLeg] == 18, "unreal naming");

        const Skeleton rigify = make_humanoid({"hips", "spine", "chest", "neck", "head", "shoulder.L", "upper_arm.L",
            "forearm.L", "hand.L", "shoulder.R", "upper_arm.R", "forearm.R", "hand.R", "thigh.L", "shin.L", "foot.L",
            "thigh.R", "shin.R", "foot.R"});
        check(guess_humanoid_map(rigify).usable(), "rigify naming");

        const Skeleton biped = make_humanoid({"Bip01 Pelvis", "Bip01 Spine", "Bip01 Neck", "Bip01 Head", "Bip01 L Clavicle",
            "Bip01 L UpperArm", "Bip01 L Forearm", "Bip01 L Hand", "Bip01 R Clavicle", "Bip01 R UpperArm", "Bip01 R Forearm",
            "Bip01 R Hand", "Bip01 L Thigh", "Bip01 L Calf", "Bip01 L Foot", "Bip01 R Thigh", "Bip01 R Calf", "Bip01 R Foot"});
        check(guess_humanoid_map(biped).usable(), "3ds Max biped naming");
    }

    // ---- retarget: same motion, different bone axes ------------------------------------------
    {
        // Source: hips with one child "LeftUpperArm" whose local frame is rotated 90 degrees about Z in its rest pose.
        Skeleton src;
        src.names = {"Hips", "LeftUpperArm"};
        src.parents = {no_joint, 0};
        src.rest_pose = {JointTransform{.translation = {0, 1, 0}},
                         JointTransform{.translation = {0, 0, 0}, .rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1))}};
        src.inverse_bind.assign(2, glm::mat4(1.0f));
        Skeleton dst;
        dst.names = {"mixamorig:Hips", "mixamorig:LeftArm"};
        dst.parents = {no_joint, 0};
        dst.rest_pose = {JointTransform{.translation = {0, 2, 0}}, JointTransform{}};
        dst.inverse_bind.assign(2, glm::mat4(1.0f));
        HumanoidMap sm, dm;
        sm[HumanoidBone::Hips] = 0; sm[HumanoidBone::LeftUpperArm] = 1;
        dm[HumanoidBone::Hips] = 0; dm[HumanoidBone::LeftUpperArm] = 1;

        Clip clip;
        clip.name = "Raise";
        clip.channels.resize(2);
        // The source arm rotates a further 90 degrees about Y (on top of its rest rotation) and the hips move 1 up in x.
        const glm::quat rest = src.rest_pose[1].rotation;
        const glm::quat anim = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)) * rest;
        clip.channels[1].rotation.times = {0.0f, 1.0f};
        clip.channels[1].rotation.values = {rest.x, rest.y, rest.z, rest.w, anim.x, anim.y, anim.z, anim.w};
        clip.channels[0].translation.times = {0.0f, 1.0f};
        clip.channels[0].translation.values = {0, 1, 0, 1, 1, 0};
        clip.recompute_duration();

        const Clip out = retarget_clip(clip, src, sm, dst, dm);
        Pose pose;
        sample_clip(dst, out, 1.0f, false, pose);
        // World rotation change of the arm must be 90 degrees about Y in the target too.
        check(near(glm::degrees(glm::angle(pose[1].rotation)), 90.0f, 0.5f) && near(std::fabs(glm::axis(pose[1].rotation).y), 1.0f, 0.01f),
              "arm rotation delta carried over despite different rest axes");
        // Hips moved by 1 in x on a rig twice as tall (rest height 2 vs 1): scaled to 2.
        check(near(pose[0].translation.x, 2.0f) && near(pose[0].translation.y, 2.0f), "hips travel scaled by body size");
        sample_clip(dst, out, 0.0f, false, pose);
        check(near(glm::degrees(glm::angle(pose[1].rotation)), 0.0f, 0.5f), "rest frame maps to the target rest pose");
    }

    // ---- remap by name -----------------------------------------------------------------------
    {
        Skeleton target = make_humanoid({"mixamorig:Hips", "mixamorig:Spine"});
        Clip clip;
        clip.joint_names = {"Spine", "Hips", "Unrelated"};
        clip.channels.resize(3);
        clip.channels[1].translation.times = {0.0f};
        clip.channels[1].translation.values = {5, 0, 0};
        const Clip out = remap_clip_by_name(clip, target);
        check(out.channels.size() == 2 && !out.channels[0].translation.empty() && out.channels[1].translation.empty(),
              "name remap finds Hips behind the Mixamo prefix");
    }

    return failures == 0 ? 0 : 1;
}
