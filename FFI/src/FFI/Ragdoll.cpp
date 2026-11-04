/// C ABI for ragdoll definitions (`Physics/Ragdoll.hpp`): capsules, joints, limits and masses built from an animated entity's humanoid skeleton, and the
/// two conversions between the animation pose and simulated body rotations.
///
/// The definition keeps the skeleton alive (a shared pointer) so it can be used after the entity is gone.

#include <Foundation/Foundation.hpp>

#include <cmath>
#include <memory>

#include <Animation/Retarget.hpp>
#include <Animation/Skeleton.hpp>
#include <Engine/EcsAnimation.hpp>
#include <Engine/Engine.hpp>
#include <Physics/Ragdoll.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    namespace P = SFT::Physics;
    namespace A = SFT::Animation;

    using SFT::Ffi::HandleKind;
    using SFT::Ffi::guarded;
    using SFT::Ffi::mint_handle;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::resolve_handle;
    using SFT::Ffi::revoke_handle;
    using SFT::Ffi::set_error;

    struct RagdollRecord {
        std::shared_ptr<const A::Skeleton> skeleton;
        P::RagdollDef definition;
    };

    [[nodiscard]] SturdyResult invalid(const char *what) noexcept { return set_error(STURDY_ERROR_INVALID_ARGUMENT, what); }

    [[nodiscard]] SturdyVec3 from_glm(const glm::vec3 &v) noexcept { return {v.x, v.y, v.z}; }
    [[nodiscard]] SturdyQuat from_glm(const glm::quat &q) noexcept { return {q.x, q.y, q.z, q.w}; }
    [[nodiscard]] glm::quat to_glm(const SturdyQuat &q) noexcept { return glm::quat{q.w, q.x, q.y, q.z}; }

    /// The skeleton and current pose of an animated entity (a skeleton animator, else an animation-graph player).
    struct EntityRig {
        std::shared_ptr<const A::Skeleton> skeleton;
        A::Pose pose;
    };

    [[nodiscard]] SturdyResult rig_of(SFT::Engine::Engine &engine, SturdyEntity entity, EntityRig *out) {
        const SFT::Ecs::Entity target{.index = entity.index, .generation = entity.generation};
        SFT::Ecs::World &world = engine.ecs_world();
        if (auto animator = world.get_component<SFT::Engine::SkeletonAnimator>(target); animator && animator->skeleton) {
            out->skeleton = animator->skeleton;
            out->pose = animator->pose;
            return STURDY_OK;
        }
        if (auto player = world.get_component<SFT::Engine::AnimationGraphPlayer>(target); player && player->skeleton) {
            out->skeleton = player->skeleton;
            out->pose = player->pose;
            return STURDY_OK;
        }
        return set_error(STURDY_ERROR_NOT_AVAILABLE, "the entity has no skeleton (it needs a skeleton animator or an animation-graph player)");
    }

    template <class Body>
    [[nodiscard]] SturdyResult with_ragdoll(SturdyRagdoll handle, Body &&body) noexcept {
        return guarded([&]() -> SturdyResult {
            void *pointer = nullptr;
            const SturdyResult resolved = resolve_handle(handle.token, HandleKind::Ragdoll, &pointer);
            if (resolved != STURDY_OK) {
                return resolved;
            }
            return body(*static_cast<RagdollRecord *>(pointer));
        });
    }

    /// The pose to work from: the entity's current one, or the rest pose before its first tick.
    [[nodiscard]] A::Pose current_pose(const RagdollRecord &record, const EntityRig &rig) {
        if (rig.pose.size() == record.skeleton->names.size()) {
            return rig.pose;
        }
        return A::rest_pose_of(*record.skeleton);
    }

    // The entity must still match the skeleton the ragdoll was built for.
    [[nodiscard]] SturdyResult matches(const RagdollRecord &record, const EntityRig &rig) noexcept {
        if (rig.skeleton->names.size() != record.skeleton->names.size()) {
            return invalid("the entity's skeleton is not the one this ragdoll was built for");
        }
        return STURDY_OK;
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_ragdoll_options_init(SturdyRagdollOptions *options) {
    return guarded([&]() -> SturdyResult {
        if (options == nullptr) {
            return invalid("options must not be null");
        }
        const P::RagdollOptions d;
        *options = {sizeof(SturdyRagdollOptions), d.total_mass, d.limb_radius_ratio, d.torso_radius_ratio};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ragdoll_build(SturdyEngine engine, SturdyEntity entity, const SturdyRagdollOptions *options, SturdyRagdoll *out_ragdoll) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::Engine *resolved = nullptr;
        if (const SturdyResult r = resolve_engine(engine, &resolved); r != STURDY_OK) {
            return r;
        }
        if (out_ragdoll == nullptr) {
            return invalid("output pointer must not be null");
        }
        P::RagdollOptions native;
        if (options != nullptr) {
            if (options->struct_size != sizeof(SturdyRagdollOptions)) {
                return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyRagdollOptions::struct_size does not match this build");
            }
            if (!(options->total_mass > 0.0f) || !(options->limb_radius_ratio > 0.0f) || !(options->torso_radius_ratio > 0.0f) || !std::isfinite(options->total_mass) ||
                !std::isfinite(options->limb_radius_ratio) || !std::isfinite(options->torso_radius_ratio)) {
                return invalid("mass and radius ratios must be finite and positive");
            }
            native = {options->total_mass, options->limb_radius_ratio, options->torso_radius_ratio};
        }
        EntityRig rig;
        if (const SturdyResult found = rig_of(*resolved, entity, &rig); found != STURDY_OK) {
            return found;
        }
        const A::HumanoidMap map = A::guess_humanoid_map(*rig.skeleton);
        if (!map.usable()) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "the skeleton has no usable humanoid mapping (it needs hips, both legs and both arms)");
        }
        auto record = std::make_unique<RagdollRecord>();
        record->skeleton = rig.skeleton;
        record->definition = P::build_humanoid_ragdoll(*rig.skeleton, map, native);
        if (record->definition.bodies.empty()) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no ragdoll could be built from the skeleton");
        }
        out_ragdoll->token = mint_handle(HandleKind::Ragdoll, record.get());
        static_cast<void>(record.release());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ragdoll_destroy(SturdyRagdoll ragdoll) {
    return guarded([&]() -> SturdyResult {
        void *pointer = nullptr;
        const SturdyResult resolved = resolve_handle(ragdoll.token, HandleKind::Ragdoll, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        revoke_handle(ragdoll.token);
        delete static_cast<RagdollRecord *>(pointer);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ragdoll_body_count(SturdyRagdoll ragdoll, uint32_t *out_count) {
    return with_ragdoll(ragdoll, [&](RagdollRecord &record) -> SturdyResult {
        if (out_count == nullptr) {
            return invalid("output pointer must not be null");
        }
        *out_count = static_cast<uint32_t>(record.definition.bodies.size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ragdoll_joint_count(SturdyRagdoll ragdoll, uint32_t *out_count) {
    return with_ragdoll(ragdoll, [&](RagdollRecord &record) -> SturdyResult {
        if (out_count == nullptr) {
            return invalid("output pointer must not be null");
        }
        *out_count = static_cast<uint32_t>(record.definition.joints.size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ragdoll_total_mass(SturdyRagdoll ragdoll, float *out_mass) {
    return with_ragdoll(ragdoll, [&](RagdollRecord &record) -> SturdyResult {
        if (out_mass == nullptr) {
            return invalid("output pointer must not be null");
        }
        *out_mass = record.definition.total_mass;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ragdoll_body(SturdyRagdoll ragdoll, uint32_t index, SturdyRagdollBody *out_body) {
    return with_ragdoll(ragdoll, [&](RagdollRecord &record) -> SturdyResult {
        if (out_body == nullptr) {
            return invalid("output pointer must not be null");
        }
        if (index >= record.definition.bodies.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "body index is out of range");
        }
        const P::RagdollBody &body = record.definition.bodies[index];
        *out_body = {body.joint,
                     body.parent_body,
                     static_cast<uint32_t>(body.bone),
                     body.mass,
                     body.radius,
                     body.length,
                     from_glm(body.center_offset),
                     from_glm(body.axis),
                     from_glm(body.rest_position),
                     from_glm(body.rest_rotation)};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ragdoll_joint(SturdyRagdoll ragdoll, uint32_t index, SturdyRagdollJoint *out_joint) {
    return with_ragdoll(ragdoll, [&](RagdollRecord &record) -> SturdyResult {
        if (out_joint == nullptr) {
            return invalid("output pointer must not be null");
        }
        if (index >= record.definition.joints.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "joint index is out of range");
        }
        const P::RagdollJoint &joint = record.definition.joints[index];
        SturdyRagdollJoint out{};
        out.body = joint.body;
        out.parent_body = joint.parent_body;
        out.kind = joint.kind == P::RagdollJointKind::Hinge ? STURDY_RAGDOLL_JOINT_HINGE : STURDY_RAGDOLL_JOINT_BALL;
        out.anchor_in_parent = from_glm(joint.anchor_in_parent);
        out.limits = {from_glm(joint.limits.axis), joint.limits.swing_y, joint.limits.swing_z, joint.limits.twist_min, joint.limits.twist_max};
        out.hinge_axis = from_glm(joint.hinge_axis);
        out.hinge_min = joint.hinge_min;
        out.hinge_max = joint.hinge_max;
        out.drive_stiffness = joint.drive_stiffness;
        out.drive_damping = joint.drive_damping;
        *out_joint = out;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ragdoll_body_for_joint(SturdyRagdoll ragdoll, uint32_t joint, uint32_t *out_body) {
    return with_ragdoll(ragdoll, [&](RagdollRecord &record) -> SturdyResult {
        if (out_body == nullptr) {
            return invalid("output pointer must not be null");
        }
        const SFT::u32 body = record.definition.body_for_joint(joint);
        if (body == P::no_body) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no body drives that joint");
        }
        *out_body = body;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ragdoll_target_body_rotations(SturdyEngine engine, SturdyRagdoll ragdoll, SturdyEntity entity, SturdyQuat *out_rotations,
                                                                  uint32_t capacity, uint32_t *out_count) {
    return with_ragdoll(ragdoll, [&](RagdollRecord &record) -> SturdyResult {
        SFT::Engine::Engine *resolved = nullptr;
        if (const SturdyResult r = resolve_engine(engine, &resolved); r != STURDY_OK) {
            return r;
        }
        if (out_count == nullptr) {
            return invalid("out_count must not be null");
        }
        EntityRig rig;
        if (const SturdyResult found = rig_of(*resolved, entity, &rig); found != STURDY_OK) {
            return found;
        }
        if (const SturdyResult same = matches(record, rig); same != STURDY_OK) {
            return same;
        }
        const std::vector<glm::quat> targets = P::target_body_rotations(*record.skeleton, record.definition, current_pose(record, rig));
        *out_count = static_cast<uint32_t>(targets.size());
        if (out_rotations == nullptr) {
            return STURDY_OK;
        }
        const uint32_t count = *out_count < capacity ? *out_count : capacity;
        for (uint32_t i = 0; i < count; ++i) {
            out_rotations[i] = from_glm(targets[i]);
        }
        return *out_count > capacity ? set_error(STURDY_ERROR_BUFFER_TOO_SMALL, "output buffer is smaller than the body count") : STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ragdoll_apply_body_rotations(SturdyEngine engine, SturdyRagdoll ragdoll, SturdyEntity entity, const SturdyQuat *body_rotations,
                                                                 uint32_t body_count, SturdyQuat *out_joint_rotations, uint32_t capacity, uint32_t *out_joint_count) {
    return with_ragdoll(ragdoll, [&](RagdollRecord &record) -> SturdyResult {
        SFT::Engine::Engine *resolved = nullptr;
        if (const SturdyResult r = resolve_engine(engine, &resolved); r != STURDY_OK) {
            return r;
        }
        if (body_rotations == nullptr || out_joint_count == nullptr) {
            return invalid("body_rotations and out_joint_count must not be null");
        }
        if (body_count != record.definition.bodies.size()) {
            return invalid("body_count must equal the ragdoll's body count");
        }
        EntityRig rig;
        if (const SturdyResult found = rig_of(*resolved, entity, &rig); found != STURDY_OK) {
            return found;
        }
        if (const SturdyResult same = matches(record, rig); same != STURDY_OK) {
            return same;
        }
        std::vector<glm::quat> rotations;
        rotations.reserve(body_count);
        for (uint32_t i = 0; i < body_count; ++i) {
            const glm::quat q = to_glm(body_rotations[i]);
            if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w) || glm::length(q) < 1e-6f) {
                return invalid("a body rotation is not a usable quaternion");
            }
            rotations.push_back(glm::normalize(q));
        }
        A::Pose pose = current_pose(record, rig);
        P::apply_body_rotations(*record.skeleton, record.definition, rotations, pose);
        *out_joint_count = static_cast<uint32_t>(pose.size());
        if (out_joint_rotations == nullptr) {
            return STURDY_OK;
        }
        const uint32_t count = *out_joint_count < capacity ? *out_joint_count : capacity;
        for (uint32_t i = 0; i < count; ++i) {
            out_joint_rotations[i] = from_glm(pose[i].rotation);
        }
        return *out_joint_count > capacity ? set_error(STURDY_ERROR_BUFFER_TOO_SMALL, "output buffer is smaller than the joint count") : STURDY_OK;
    });
}

} // extern "C"
