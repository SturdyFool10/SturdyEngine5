/// Exercises the ragdoll C ABI against a synthetic humanoid skeleton (Mixamo naming) on an entity with a skeleton animator: no model, window or
/// graphics device is needed. Static builds only (it mints an engine handle).

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include <Async/Scheduler.hpp>
#include <Animation/Skeleton.hpp>
#include <Engine/EcsAnimation.hpp>
#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "RagdollFfiTest: %s\n", description);
            ++failures;
        }
    }

    /// A 22-joint Mixamo-named humanoid standing 1.8 m tall, parents before children, with real bone lengths.
    std::shared_ptr<const SFT::Animation::Skeleton> make_humanoid() {
        using SFT::Animation::JointTransform;
        struct Joint {
            const char *name;
            SFT::u32 parent;
            glm::vec3 offset;
        };
        const std::vector<Joint> joints = {
            {"mixamorig:Hips", SFT::Animation::no_joint, {0, 1.0f, 0}},
            {"mixamorig:Spine", 0, {0, 0.1f, 0}},
            {"mixamorig:Spine1", 1, {0, 0.15f, 0}},
            {"mixamorig:Spine2", 2, {0, 0.15f, 0}},
            {"mixamorig:Neck", 3, {0, 0.15f, 0}},
            {"mixamorig:Head", 4, {0, 0.12f, 0}},
            {"mixamorig:LeftShoulder", 3, {0.05f, 0.1f, 0}},
            {"mixamorig:LeftArm", 6, {0.12f, 0, 0}},
            {"mixamorig:LeftForeArm", 7, {0.28f, 0, 0}},
            {"mixamorig:LeftHand", 8, {0.25f, 0, 0}},
            {"mixamorig:RightShoulder", 3, {-0.05f, 0.1f, 0}},
            {"mixamorig:RightArm", 10, {-0.12f, 0, 0}},
            {"mixamorig:RightForeArm", 11, {-0.28f, 0, 0}},
            {"mixamorig:RightHand", 12, {-0.25f, 0, 0}},
            {"mixamorig:LeftUpLeg", 0, {0.09f, -0.05f, 0}},
            {"mixamorig:LeftLeg", 14, {0, -0.45f, 0}},
            {"mixamorig:LeftFoot", 15, {0, -0.43f, 0}},
            {"mixamorig:LeftToeBase", 16, {0, -0.05f, 0.15f}},
            {"mixamorig:RightUpLeg", 0, {-0.09f, -0.05f, 0}},
            {"mixamorig:RightLeg", 18, {0, -0.45f, 0}},
            {"mixamorig:RightFoot", 19, {0, -0.43f, 0}},
            {"mixamorig:RightToeBase", 20, {0, -0.05f, 0.15f}},
        };
        auto skeleton = std::make_shared<SFT::Animation::Skeleton>();
        for (const Joint &joint : joints) {
            skeleton->names.push_back(SFT::UString::from_c_str(joint.name));
            skeleton->parents.push_back(joint.parent);
            JointTransform transform;
            transform.translation = joint.offset;
            skeleton->rest_pose.push_back(transform);
            skeleton->inverse_bind.push_back(glm::mat4(1.0f));
        }
        return skeleton;
    }

} // namespace

int main() {
    using SFT::Ffi::HandleKind;
    using SFT::Ffi::ScopedHandle;

    SFT::Engine::Engine engine_object;
    const ScopedHandle engine_handle{HandleKind::Engine, &engine_object};
    const SturdyEngine engine{engine_handle.token()};

    const auto skeleton = make_humanoid();
    SFT::Engine::SkeletonAnimator animator;
    animator.skeleton = skeleton;
    animator.pose = SFT::Animation::rest_pose_of(*skeleton);
    const SFT::Ecs::Entity spawned = engine_object.ecs_world().spawn(std::move(animator));
    const SturdyEntity entity{spawned.index, spawned.generation};

    SturdyRagdollOptions options{};
    check(sturdy_ragdoll_options_init(&options) == STURDY_OK && options.struct_size == sizeof(options) && options.total_mass > 0.0f, "options initialise");
    SturdyRagdoll ragdoll{};
    check(sturdy_ragdoll_build(engine, SturdyEntity{999, 1}, &options, &ragdoll) == STURDY_ERROR_NOT_AVAILABLE, "an entity with no skeleton is not available");
    SturdyRagdollOptions bad = options;
    bad.total_mass = -1.0f;
    check(sturdy_ragdoll_build(engine, entity, &bad, &ragdoll) == STURDY_ERROR_INVALID_ARGUMENT, "a negative mass is refused");
    bad = options;
    bad.struct_size = 3;
    check(sturdy_ragdoll_build(engine, entity, &bad, &ragdoll) == STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "a wrong options size is refused");
    check(sturdy_ragdoll_build(engine, entity, &options, &ragdoll) == STURDY_OK, "a ragdoll is built for the humanoid");

    uint32_t bodies = 0, joints = 0;
    float mass = 0.0f;
    check(sturdy_ragdoll_body_count(ragdoll, &bodies) == STURDY_OK && bodies >= 10, "the ragdoll has a body per major bone");
    check(sturdy_ragdoll_joint_count(ragdoll, &joints) == STURDY_OK && joints == bodies - 1, "one joint per non-root body");
    check(sturdy_ragdoll_total_mass(ragdoll, &mass) == STURDY_OK && std::fabs(mass - options.total_mass) < 1.0f, "the masses add up to the requested total");

    SturdyRagdollBody root{};
    check(sturdy_ragdoll_body(ragdoll, 0, &root) == STURDY_OK && root.parent_body == 0xFFFFFFFFu && root.mass > 0.0f && root.radius > 0.0f, "the root body is described");
    SturdyRagdollBody leg{};
    bool found_child = false;
    for (uint32_t i = 1; i < bodies; ++i) {
        check(sturdy_ragdoll_body(ragdoll, i, &leg) == STURDY_OK && leg.parent_body < bodies, "every other body has a valid parent");
        found_child = true;
    }
    check(found_child, "there are child bodies");
    check(sturdy_ragdoll_body(ragdoll, bodies, &leg) == STURDY_ERROR_OUT_OF_RANGE, "a body past the end is out of range");
    SturdyRagdollJoint joint{};
    check(sturdy_ragdoll_joint(ragdoll, 0, &joint) == STURDY_OK && joint.body < bodies && joint.parent_body < bodies && joint.limits.swing_y > 0.0f, "a joint is described with limits");
    check(sturdy_ragdoll_joint(ragdoll, joints, &joint) == STURDY_ERROR_OUT_OF_RANGE, "a joint past the end is out of range");
    uint32_t body_of_root = 99;
    check(sturdy_ragdoll_body_for_joint(ragdoll, root.joint, &body_of_root) == STURDY_OK && body_of_root == 0, "the root joint maps back to the root body");
    check(sturdy_ragdoll_body_for_joint(ragdoll, 9999, &body_of_root) == STURDY_ERROR_NOT_AVAILABLE, "an undriven joint is not available");

    // Pose round trip: the animation's targets, fed back as the simulated rotations, reproduce the pose.
    std::vector<SturdyQuat> targets(bodies);
    uint32_t count = 0;
    check(sturdy_ragdoll_target_body_rotations(engine, ragdoll, entity, targets.data(), bodies, &count) == STURDY_OK && count == bodies, "the target rotations are read");
    SturdyQuat tiny[1];
    check(sturdy_ragdoll_target_body_rotations(engine, ragdoll, entity, tiny, 1, &count) == STURDY_ERROR_BUFFER_TOO_SMALL && count == bodies, "a small buffer reports the count");
    std::vector<SturdyQuat> joint_rotations(skeleton->names.size());
    uint32_t joint_count = 0;
    check(sturdy_ragdoll_apply_body_rotations(engine, ragdoll, entity, targets.data(), bodies, joint_rotations.data(), static_cast<uint32_t>(joint_rotations.size()), &joint_count) ==
                  STURDY_OK &&
              joint_count == skeleton->names.size(),
          "body rotations are applied to the pose");
    bool same = true;
    for (size_t i = 0; i < joint_rotations.size(); ++i) {
        const auto &rest = skeleton->rest_pose[i].rotation;
        const float dot = std::fabs(joint_rotations[i].x * rest.x + joint_rotations[i].y * rest.y + joint_rotations[i].z * rest.z + joint_rotations[i].w * rest.w);
        same = same && dot > 0.999f;
    }
    check(same, "feeding the targets back reproduces the animation pose");
    check(sturdy_ragdoll_apply_body_rotations(engine, ragdoll, entity, targets.data(), bodies - 1, nullptr, 0, &joint_count) == STURDY_ERROR_INVALID_ARGUMENT,
          "a wrong body count is refused");
    std::vector<SturdyQuat> degenerate(bodies, SturdyQuat{0, 0, 0, 0});
    check(sturdy_ragdoll_apply_body_rotations(engine, ragdoll, entity, degenerate.data(), bodies, nullptr, 0, &joint_count) == STURDY_ERROR_INVALID_ARGUMENT,
          "a zero quaternion is refused");

    check(sturdy_ragdoll_destroy(ragdoll) == STURDY_OK, "the ragdoll is destroyed");
    check(sturdy_ragdoll_body_count(ragdoll, &bodies) != STURDY_OK, "a destroyed ragdoll no longer resolves");

    SFT::Async::Scheduler::shutdown();
    if (failures == 0) {
        std::printf("RagdollFfiTest: passed.\n");
    }
    return failures == 0 ? 0 : 1;
}
