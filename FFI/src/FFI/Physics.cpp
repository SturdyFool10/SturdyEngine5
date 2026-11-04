/// C ABI for the solver-agnostic physics helpers in `Physics/`: the fixed-step accumulator, collision layers, the
/// analytic collision world with its character controller, contact-event filtering, surface materials with
/// penetration/ricochet ballistics, cloth, and swing/twist joint maths.
///
/// Every object is a plain heap object behind an owned handle. None of this needs an engine handle: the physics package
/// is pure CPU state, which is what lets a foreign-language game run its own simulation loop on top of it.

#include <Foundation/Foundation.hpp>

#include <cmath>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <Physics/Ballistics.hpp>
#include <Physics/CharacterController.hpp>
#include <Physics/Cloth.hpp>
#include <Physics/ContactEvents.hpp>
#include <Physics/FixedStepper.hpp>
#include <Physics/Layers.hpp>
#include <Physics/SimpleWorld.hpp>
#include <Physics/SwingTwist.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::HandleKind;
    using SFT::Ffi::copy_string_out;
    using SFT::Ffi::guarded;
    using SFT::Ffi::mint_handle;
    using SFT::Ffi::resolve_handle;
    using SFT::Ffi::revoke_handle;
    using SFT::Ffi::set_error;
    using SFT::u32;
    using SFT::u64;

    [[nodiscard]] glm::vec3 to_glm(const SturdyVec3 &v) noexcept { return {v.x, v.y, v.z}; }
    [[nodiscard]] SturdyVec3 from_glm(const glm::vec3 &v) noexcept { return {v.x, v.y, v.z}; }
    [[nodiscard]] glm::quat to_glm(const SturdyQuat &q) noexcept { return glm::quat{q.w, q.x, q.y, q.z}; }
    [[nodiscard]] SturdyQuat from_glm(const glm::quat &q) noexcept { return {q.x, q.y, q.z, q.w}; }

    [[nodiscard]] SturdyResult null_argument(const char *what) noexcept {
        return set_error(STURDY_ERROR_INVALID_ARGUMENT, what);
    }

    /// Resolves `token` to a `T` of the expected handle kind.
    template <class T>
    [[nodiscard]] SturdyResult resolve(u64 token, HandleKind kind, T **out) noexcept {
        void *pointer = nullptr;
        const SturdyResult result = resolve_handle(token, kind, &pointer);
        if (result != STURDY_OK) {
            return result;
        }
        *out = static_cast<T *>(pointer);
        return STURDY_OK;
    }

    /// Takes ownership of `object` and mints its handle into `out`.
    template <class T, class Handle>
    [[nodiscard]] SturdyResult publish(HandleKind kind, std::unique_ptr<T> object, Handle *out) {
        if (out == nullptr) {
            return null_argument("output pointer must not be null");
        }
        out->token = mint_handle(kind, object.get());
        static_cast<void>(object.release());
        return STURDY_OK;
    }

    /// Revokes the handle and frees the object behind it.
    template <class T>
    [[nodiscard]] SturdyResult destroy(u64 token, HandleKind kind) noexcept {
        T *object = nullptr;
        const SturdyResult resolved = resolve(token, kind, &object);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        revoke_handle(token);
        delete object;
        return STURDY_OK;
    }

    /// Resolves `handle` and runs `body(object)`, the shape almost every entry point takes.
    template <class T, class Handle, class Body>
    [[nodiscard]] SturdyResult with(Handle handle, HandleKind kind, Body &&body) noexcept {
        return guarded([&]() -> SturdyResult {
            T *object = nullptr;
            const SturdyResult resolved = resolve(handle.token, kind, &object);
            if (resolved != STURDY_OK) {
                return resolved;
            }
            return body(*object);
        });
    }

    template <class T>
    [[nodiscard]] bool have_struct_size(const T *value) noexcept {
        return value->struct_size == sizeof(T);
    }

    constexpr u32 default_max_steps = 8;
    constexpr double default_step = 1.0 / 60.0;
    // Cloth is O(particles); this keeps a bad size from asking for gigabytes.
    constexpr u64 max_cloth_particles = 1u << 22;

    [[nodiscard]] SFT::Physics::CharacterSettings to_engine(const SturdyPhysicsCharacterSettings &s) noexcept {
        SFT::Physics::CharacterSettings out;
        out.radius = s.radius;
        out.standing_height = s.standing_height;
        out.crouching_height = s.crouching_height;
        out.up = to_glm(s.up);
        out.gravity = s.gravity;
        out.slope_limit_degrees = s.slope_limit_degrees;
        out.step_height = s.step_height;
        out.snap_distance = s.snap_distance;
        out.skin_width = s.skin_width;
        out.max_slides = s.max_slides;
        out.jump_speed = s.jump_speed;
        out.terminal_speed = s.terminal_speed;
        return out;
    }

    [[nodiscard]] SFT::Physics::ContactEventConfig to_engine(const SturdyPhysicsContactConfig &c) noexcept {
        SFT::Physics::ContactEventConfig out;
        out.min_impulse = c.min_impulse;
        out.min_speed = c.min_speed;
        out.pair_cooldown_seconds = c.pair_cooldown_seconds;
        out.merge_radius = c.merge_radius;
        out.max_events_per_frame = c.max_events_per_frame;
        out.full_intensity_impulse = c.full_intensity_impulse;
        return out;
    }

    [[nodiscard]] SFT::Physics::ContactPoint to_engine(const SturdyPhysicsContactPoint &c) noexcept {
        SFT::Physics::ContactPoint out;
        out.body_a = c.body_a;
        out.body_b = c.body_b;
        out.point = to_glm(c.point);
        out.normal = to_glm(c.normal);
        out.normal_impulse = c.normal_impulse;
        out.approach_speed = c.approach_speed;
        out.slide_speed = c.slide_speed;
        out.material_a = c.material_a;
        out.material_b = c.material_b;
        return out;
    }

    [[nodiscard]] SturdyPhysicsContactPoint from_engine(const SFT::Physics::ContactPoint &c) noexcept {
        return {c.body_a,      c.body_b,         from_glm(c.point), from_glm(c.normal), c.normal_impulse,
                c.approach_speed, c.slide_speed, c.material_a,      c.material_b};
    }

    [[nodiscard]] SFT::Physics::SurfaceMaterial to_engine(const SturdyPhysicsSurfaceMaterial &m) {
        SFT::Physics::SurfaceMaterial out;
        if (m.name != nullptr) {
            out.name = SFT::UString::from_c_str(m.name);
        }
        out.penetration_resistance = m.penetration_resistance;
        out.ricochet_angle_degrees = m.ricochet_angle_degrees;
        out.ricochet_energy_retained = m.ricochet_energy_retained;
        out.ricochet_scatter = m.ricochet_scatter;
        out.exit_deflection = m.exit_deflection;
        return out;
    }

    [[nodiscard]] SturdyPhysicsSurfaceMaterial from_engine(const SFT::Physics::SurfaceMaterial &m) noexcept {
        SturdyPhysicsSurfaceMaterial out{};
        out.struct_size = sizeof(SturdyPhysicsSurfaceMaterial);
        out.name = nullptr;
        out.penetration_resistance = m.penetration_resistance;
        out.ricochet_angle_degrees = m.ricochet_angle_degrees;
        out.ricochet_energy_retained = m.ricochet_energy_retained;
        out.ricochet_scatter = m.ricochet_scatter;
        out.exit_deflection = m.exit_deflection;
        return out;
    }

    [[nodiscard]] SturdyPhysicsImpactResult from_engine(const SFT::Physics::ImpactResult &r) noexcept {
        SturdyPhysicsImpactResult out{};
        out.outcome = static_cast<SturdyPhysicsImpactOutcome>(static_cast<u32>(r.outcome));
        out.point = from_glm(r.point);
        out.direction = from_glm(r.direction);
        out.remaining_energy = r.remaining_energy;
        out.depth = r.depth;
        return out;
    }

    [[nodiscard]] SFT::Physics::SwingTwistLimits to_engine(const SturdyPhysicsSwingTwistLimits &l) noexcept {
        SFT::Physics::SwingTwistLimits out;
        out.axis = to_glm(l.axis);
        out.swing_y = l.swing_y;
        out.swing_z = l.swing_z;
        out.twist_min = l.twist_min;
        out.twist_max = l.twist_max;
        return out;
    }

    /// A direction the maths divides by must be finite and non-zero.
    [[nodiscard]] bool usable_axis(const glm::vec3 &axis) noexcept {
        const float length = glm::length(axis);
        return std::isfinite(length) && length > 1e-6f;
    }

    // Copies `source` into a caller buffer of `capacity` entries, reporting the full count the way every ABI list does.
    template <class Out, class Source, class Convert>
    [[nodiscard]] SturdyResult copy_list(const Source &source, Out *out, u32 capacity, u32 *out_count, Convert convert) noexcept {
        const u32 total = static_cast<u32>(source.size());
        if (out_count != nullptr) {
            *out_count = total;
        }
        if (out == nullptr) {
            return STURDY_OK;
        }
        const u32 count = total < capacity ? total : capacity;
        for (u32 i = 0; i < count; ++i) {
            out[i] = convert(source[i]);
        }
        return total > capacity ? set_error(STURDY_ERROR_BUFFER_TOO_SMALL, "output buffer is smaller than the list") : STURDY_OK;
    }

} // namespace

extern "C" {

// --- fixed stepper ---

SturdyResult STURDY_ABI_CALL sturdy_physics_stepper_create(double step_seconds, uint32_t max_steps_per_frame, SturdyPhysicsStepper *out_stepper) {
    return guarded([&]() -> SturdyResult {
        if (!std::isfinite(step_seconds)) {
            return null_argument("step_seconds must be finite");
        }
        return publish(HandleKind::PhysicsStepper,
                       std::make_unique<SFT::Physics::FixedStepper>(step_seconds > 0.0 ? step_seconds : default_step,
                                                                     max_steps_per_frame != 0 ? max_steps_per_frame : default_max_steps),
                       out_stepper);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_stepper_destroy(SturdyPhysicsStepper stepper) {
    return guarded([&]() -> SturdyResult { return destroy<SFT::Physics::FixedStepper>(stepper.token, HandleKind::PhysicsStepper); });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_stepper_advance(SturdyPhysicsStepper stepper, double delta_seconds, uint32_t *out_steps) {
    return with<SFT::Physics::FixedStepper>(stepper, HandleKind::PhysicsStepper, [&](SFT::Physics::FixedStepper &s) -> SturdyResult {
        if (out_steps == nullptr) {
            return null_argument("output pointer must not be null");
        }
        if (!std::isfinite(delta_seconds) || delta_seconds < 0.0) {
            return null_argument("delta_seconds must be finite and not negative");
        }
        *out_steps = s.advance(delta_seconds);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_stepper_alpha(SturdyPhysicsStepper stepper, double *out_alpha) {
    return with<SFT::Physics::FixedStepper>(stepper, HandleKind::PhysicsStepper, [&](SFT::Physics::FixedStepper &s) -> SturdyResult {
        if (out_alpha == nullptr) {
            return null_argument("output pointer must not be null");
        }
        *out_alpha = s.alpha();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_stepper_step(SturdyPhysicsStepper stepper, double *out_step_seconds) {
    return with<SFT::Physics::FixedStepper>(stepper, HandleKind::PhysicsStepper, [&](SFT::Physics::FixedStepper &s) -> SturdyResult {
        if (out_step_seconds == nullptr) {
            return null_argument("output pointer must not be null");
        }
        *out_step_seconds = s.step();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_stepper_total_steps(SturdyPhysicsStepper stepper, uint64_t *out_steps) {
    return with<SFT::Physics::FixedStepper>(stepper, HandleKind::PhysicsStepper, [&](SFT::Physics::FixedStepper &s) -> SturdyResult {
        if (out_steps == nullptr) {
            return null_argument("output pointer must not be null");
        }
        *out_steps = s.total_steps();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_stepper_simulated_time(SturdyPhysicsStepper stepper, double *out_seconds) {
    return with<SFT::Physics::FixedStepper>(stepper, HandleKind::PhysicsStepper, [&](SFT::Physics::FixedStepper &s) -> SturdyResult {
        if (out_seconds == nullptr) {
            return null_argument("output pointer must not be null");
        }
        *out_seconds = s.simulated_time();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_stepper_dropped_seconds(SturdyPhysicsStepper stepper, double *out_seconds) {
    return with<SFT::Physics::FixedStepper>(stepper, HandleKind::PhysicsStepper, [&](SFT::Physics::FixedStepper &s) -> SturdyResult {
        if (out_seconds == nullptr) {
            return null_argument("output pointer must not be null");
        }
        *out_seconds = s.dropped_seconds();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_stepper_set_step(SturdyPhysicsStepper stepper, double step_seconds) {
    return with<SFT::Physics::FixedStepper>(stepper, HandleKind::PhysicsStepper, [&](SFT::Physics::FixedStepper &s) -> SturdyResult {
        if (!std::isfinite(step_seconds) || step_seconds <= 0.0) {
            return null_argument("step_seconds must be finite and positive");
        }
        s.set_step(step_seconds);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_stepper_reset(SturdyPhysicsStepper stepper) {
    return with<SFT::Physics::FixedStepper>(stepper, HandleKind::PhysicsStepper, [&](SFT::Physics::FixedStepper &s) -> SturdyResult {
        s.reset();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_interpolate_pose(SturdyVec3 previous_position, SturdyQuat previous_rotation, SturdyVec3 current_position,
                                                             SturdyQuat current_rotation, float alpha, SturdyVec3 *out_position, SturdyQuat *out_rotation) {
    return guarded([&]() -> SturdyResult {
        if (out_position == nullptr || out_rotation == nullptr) {
            return null_argument("output pointers must not be null");
        }
        const SFT::Physics::BodyPose pose = SFT::Physics::interpolate({to_glm(previous_position), to_glm(previous_rotation)},
                                                                      {to_glm(current_position), to_glm(current_rotation)}, alpha);
        *out_position = from_glm(pose.position);
        *out_rotation = from_glm(pose.rotation);
        return STURDY_OK;
    });
}

// --- layers ---

SturdyResult STURDY_ABI_CALL sturdy_physics_layers_create(SturdyPhysicsLayers *out_layers) {
    return guarded([&]() -> SturdyResult { return publish(HandleKind::PhysicsLayers, std::make_unique<SFT::Physics::LayerTable>(), out_layers); });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_layers_destroy(SturdyPhysicsLayers layers) {
    return guarded([&]() -> SturdyResult { return destroy<SFT::Physics::LayerTable>(layers.token, HandleKind::PhysicsLayers); });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_layers_add(SturdyPhysicsLayers layers, const char *name, uint32_t *out_index) {
    return with<SFT::Physics::LayerTable>(layers, HandleKind::PhysicsLayers, [&](SFT::Physics::LayerTable &table) -> SturdyResult {
        if (name == nullptr || out_index == nullptr) {
            return null_argument("name and output pointer must not be null");
        }
        const auto index = table.add(SFT::ustr{std::string_view{name}});
        if (!index.has_value()) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "all 64 collision layers are taken");
        }
        *out_index = *index;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_layers_find(SturdyPhysicsLayers layers, const char *name, uint32_t *out_index) {
    return with<SFT::Physics::LayerTable>(layers, HandleKind::PhysicsLayers, [&](SFT::Physics::LayerTable &table) -> SturdyResult {
        if (name == nullptr || out_index == nullptr) {
            return null_argument("name and output pointer must not be null");
        }
        const auto index = table.find(SFT::ustr{std::string_view{name}});
        if (!index.has_value()) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no layer has that name");
        }
        *out_index = *index;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_layers_name(SturdyPhysicsLayers layers, uint32_t index, char *buffer, size_t capacity, size_t *out_length) {
    return with<SFT::Physics::LayerTable>(layers, HandleKind::PhysicsLayers, [&](SFT::Physics::LayerTable &table) -> SturdyResult {
        return copy_string_out(table.name(index).cpp_string_view(), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_layers_count(SturdyPhysicsLayers layers, uint32_t *out_count) {
    return with<SFT::Physics::LayerTable>(layers, HandleKind::PhysicsLayers, [&](SFT::Physics::LayerTable &table) -> SturdyResult {
        if (out_count == nullptr) {
            return null_argument("output pointer must not be null");
        }
        *out_count = table.count();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_layers_mask_of(SturdyPhysicsLayers layers, const char *const *names, uint32_t name_count, uint64_t *out_mask) {
    return with<SFT::Physics::LayerTable>(layers, HandleKind::PhysicsLayers, [&](SFT::Physics::LayerTable &table) -> SturdyResult {
        if (out_mask == nullptr || (names == nullptr && name_count != 0)) {
            return null_argument("output pointer and names must not be null");
        }
        std::vector<SFT::UString> list;
        list.reserve(name_count);
        for (u32 i = 0; i < name_count; ++i) {
            if (names[i] == nullptr) {
                return null_argument("a layer name is null");
            }
            list.push_back(SFT::UString::from_c_str(names[i]));
        }
        *out_mask = table.mask_of(list);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_layers_all(SturdyPhysicsLayers layers, uint64_t *out_mask) {
    return with<SFT::Physics::LayerTable>(layers, HandleKind::PhysicsLayers, [&](SFT::Physics::LayerTable &table) -> SturdyResult {
        if (out_mask == nullptr) {
            return null_argument("output pointer must not be null");
        }
        *out_mask = table.all();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_layers_set_collides(SturdyPhysicsLayers layers, uint32_t a, uint32_t b, SturdyBool collides) {
    return with<SFT::Physics::LayerTable>(layers, HandleKind::PhysicsLayers, [&](SFT::Physics::LayerTable &table) -> SturdyResult {
        if (a >= table.count() || b >= table.count()) {
            return null_argument("layer index is not registered");
        }
        table.set_collides(a, b, collides != 0);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_layers_collides(SturdyPhysicsLayers layers, uint32_t a, uint32_t b, SturdyBool *out_collides) {
    return with<SFT::Physics::LayerTable>(layers, HandleKind::PhysicsLayers, [&](SFT::Physics::LayerTable &table) -> SturdyResult {
        if (out_collides == nullptr) {
            return null_argument("output pointer must not be null");
        }
        if (a >= table.count() || b >= table.count()) {
            return null_argument("layer index is not registered");
        }
        *out_collides = table.collides(a, b) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_layers_collision_mask(SturdyPhysicsLayers layers, uint32_t layer, uint64_t *out_mask) {
    return with<SFT::Physics::LayerTable>(layers, HandleKind::PhysicsLayers, [&](SFT::Physics::LayerTable &table) -> SturdyResult {
        if (out_mask == nullptr) {
            return null_argument("output pointer must not be null");
        }
        if (layer >= table.count()) {
            return null_argument("layer index is not registered");
        }
        *out_mask = table.collision_mask(layer);
        return STURDY_OK;
    });
}

// --- simple world + character controller ---

SturdyResult STURDY_ABI_CALL sturdy_physics_world_create(SturdyPhysicsWorld *out_world) {
    return guarded([&]() -> SturdyResult { return publish(HandleKind::PhysicsWorld, std::make_unique<SFT::Physics::SimpleWorld>(), out_world); });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_world_destroy(SturdyPhysicsWorld world) {
    return guarded([&]() -> SturdyResult { return destroy<SFT::Physics::SimpleWorld>(world.token, HandleKind::PhysicsWorld); });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_world_add_box(SturdyPhysicsWorld world, const SturdyPhysicsBox *box, uint32_t *out_index) {
    return with<SFT::Physics::SimpleWorld>(world, HandleKind::PhysicsWorld, [&](SFT::Physics::SimpleWorld &w) -> SturdyResult {
        if (box == nullptr) {
            return null_argument("box must not be null");
        }
        SFT::Physics::SimpleWorld::Box added;
        added.min = to_glm(box->min);
        added.max = to_glm(box->max);
        added.object = box->object;
        added.velocity = to_glm(box->velocity);
        if (out_index != nullptr) {
            *out_index = static_cast<u32>(w.box_count());
        }
        w.add_box(added);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_world_add_plane(SturdyPhysicsWorld world, const SturdyPhysicsPlane *plane) {
    return with<SFT::Physics::SimpleWorld>(world, HandleKind::PhysicsWorld, [&](SFT::Physics::SimpleWorld &w) -> SturdyResult {
        if (plane == nullptr) {
            return null_argument("plane must not be null");
        }
        SFT::Physics::SimpleWorld::Plane added;
        added.normal = to_glm(plane->normal);
        added.offset = plane->offset;
        added.object = plane->object;
        w.add_plane(added);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_world_move_box(SturdyPhysicsWorld world, uint32_t index, SturdyVec3 delta) {
    return with<SFT::Physics::SimpleWorld>(world, HandleKind::PhysicsWorld, [&](SFT::Physics::SimpleWorld &w) -> SturdyResult {
        if (index >= w.box_count()) {
            return null_argument("box index is out of range");
        }
        w.move_box(index, to_glm(delta));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_world_overlaps_capsule(SturdyPhysicsWorld world, SturdyVec3 feet, float radius, float height, SturdyBool *out_overlaps) {
    return with<SFT::Physics::SimpleWorld>(world, HandleKind::PhysicsWorld, [&](SFT::Physics::SimpleWorld &w) -> SturdyResult {
        if (out_overlaps == nullptr) {
            return null_argument("output pointer must not be null");
        }
        *out_overlaps = w.overlaps_capsule(to_glm(feet), radius, height) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_world_sweep_capsule(SturdyPhysicsWorld world, SturdyVec3 feet, float radius, float height, SturdyVec3 delta,
                                                                SturdyBool *out_hit_something, SturdyPhysicsSweepHit *out_hit) {
    return with<SFT::Physics::SimpleWorld>(world, HandleKind::PhysicsWorld, [&](SFT::Physics::SimpleWorld &w) -> SturdyResult {
        if (out_hit_something == nullptr) {
            return null_argument("out_hit_something must not be null");
        }
        SFT::Physics::SweepHit hit;
        const bool found = w.sweep_capsule(to_glm(feet), radius, height, to_glm(delta), hit);
        *out_hit_something = found ? STURDY_TRUE : STURDY_FALSE;
        if (found && out_hit != nullptr) {
            *out_hit = {hit.fraction, from_glm(hit.normal), from_glm(hit.point), hit.object, from_glm(hit.object_velocity)};
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_character_settings_init(SturdyPhysicsCharacterSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return null_argument("settings must not be null");
        }
        const SFT::Physics::CharacterSettings d;
        *settings = {sizeof(SturdyPhysicsCharacterSettings), d.radius,      d.standing_height, d.crouching_height, from_glm(d.up),
                     d.gravity,                              d.slope_limit_degrees, d.step_height,   d.snap_distance,    d.skin_width,
                     d.max_slides,                           d.jump_speed,  d.terminal_speed};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_character_create(const SturdyPhysicsCharacterSettings *settings, SturdyVec3 feet, SturdyPhysicsCharacter *out_character) {
    return guarded([&]() -> SturdyResult {
        SFT::Physics::CharacterSettings engine_settings;
        if (settings != nullptr) {
            if (!have_struct_size(settings)) {
                return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyPhysicsCharacterSettings::struct_size does not match this build");
            }
            engine_settings = to_engine(*settings);
            if (!usable_axis(engine_settings.up)) {
                return null_argument("up must be a non-zero vector");
            }
            engine_settings.up = glm::normalize(engine_settings.up);
        }
        return publish(HandleKind::PhysicsCharacter, std::make_unique<SFT::Physics::CharacterController>(engine_settings, to_glm(feet)), out_character);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_character_destroy(SturdyPhysicsCharacter character) {
    return guarded([&]() -> SturdyResult { return destroy<SFT::Physics::CharacterController>(character.token, HandleKind::PhysicsCharacter); });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_character_move(SturdyPhysicsCharacter character, SturdyPhysicsWorld world, SturdyVec3 desired_velocity, float dt,
                                                           SturdyBool jump, SturdyPhysicsMoveResult *out_result) {
    return with<SFT::Physics::CharacterController>(character, HandleKind::PhysicsCharacter, [&](SFT::Physics::CharacterController &c) -> SturdyResult {
        SFT::Physics::SimpleWorld *w = nullptr;
        const SturdyResult resolved = resolve(world.token, HandleKind::PhysicsWorld, &w);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (!std::isfinite(dt) || dt < 0.0f) {
            return null_argument("dt must be finite and not negative");
        }
        const SFT::Physics::MoveResult result = c.move(*w, to_glm(desired_velocity), dt, jump != 0);
        if (out_result != nullptr) {
            *out_result = {from_glm(result.displacement), result.grounded,      result.hit_ceiling, result.stepped_up,
                           result.jumped,                 result.landed,        result.landing_speed};
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_character_set_crouching(SturdyPhysicsCharacter character, SturdyPhysicsWorld world, SturdyBool crouching,
                                                                    SturdyBool *out_crouching) {
    return with<SFT::Physics::CharacterController>(character, HandleKind::PhysicsCharacter, [&](SFT::Physics::CharacterController &c) -> SturdyResult {
        SFT::Physics::SimpleWorld *w = nullptr;
        const SturdyResult resolved = resolve(world.token, HandleKind::PhysicsWorld, &w);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const bool state = c.set_crouching(*w, crouching != 0);
        if (out_crouching != nullptr) {
            *out_crouching = state ? STURDY_TRUE : STURDY_FALSE;
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_character_teleport(SturdyPhysicsCharacter character, SturdyVec3 feet) {
    return with<SFT::Physics::CharacterController>(character, HandleKind::PhysicsCharacter, [&](SFT::Physics::CharacterController &c) -> SturdyResult {
        c.teleport(to_glm(feet));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_character_add_impulse(SturdyPhysicsCharacter character, SturdyVec3 velocity_change) {
    return with<SFT::Physics::CharacterController>(character, HandleKind::PhysicsCharacter, [&](SFT::Physics::CharacterController &c) -> SturdyResult {
        c.add_impulse(to_glm(velocity_change));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_character_state(SturdyPhysicsCharacter character, SturdyPhysicsCharacterState *out_state) {
    return with<SFT::Physics::CharacterController>(character, HandleKind::PhysicsCharacter, [&](SFT::Physics::CharacterController &c) -> SturdyResult {
        if (out_state == nullptr) {
            return null_argument("output pointer must not be null");
        }
        *out_state = {from_glm(c.position()), from_glm(c.velocity()), from_glm(c.ground_normal()), c.ground_object(),
                      c.height(),             c.grounded() ? STURDY_TRUE : STURDY_FALSE, c.crouching() ? STURDY_TRUE : STURDY_FALSE};
        return STURDY_OK;
    });
}

// --- contact events ---

SturdyResult STURDY_ABI_CALL sturdy_physics_contact_config_init(SturdyPhysicsContactConfig *config) {
    return guarded([&]() -> SturdyResult {
        if (config == nullptr) {
            return null_argument("config must not be null");
        }
        const SFT::Physics::ContactEventConfig d;
        *config = {sizeof(SturdyPhysicsContactConfig), d.min_impulse, d.min_speed, d.pair_cooldown_seconds, d.merge_radius, d.max_events_per_frame,
                   d.full_intensity_impulse};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_contact_filter_create(const SturdyPhysicsContactConfig *config, SturdyPhysicsContactFilter *out_filter) {
    return guarded([&]() -> SturdyResult {
        SFT::Physics::ContactEventConfig engine_config;
        if (config != nullptr) {
            if (!have_struct_size(config)) {
                return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyPhysicsContactConfig::struct_size does not match this build");
            }
            engine_config = to_engine(*config);
        }
        return publish(HandleKind::PhysicsContactFilter, std::make_unique<SFT::Physics::ContactEventFilter>(engine_config), out_filter);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_contact_filter_destroy(SturdyPhysicsContactFilter filter) {
    return guarded([&]() -> SturdyResult { return destroy<SFT::Physics::ContactEventFilter>(filter.token, HandleKind::PhysicsContactFilter); });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_contact_filter_set_config(SturdyPhysicsContactFilter filter, const SturdyPhysicsContactConfig *config) {
    return with<SFT::Physics::ContactEventFilter>(filter, HandleKind::PhysicsContactFilter, [&](SFT::Physics::ContactEventFilter &f) -> SturdyResult {
        if (config == nullptr) {
            return null_argument("config must not be null");
        }
        if (!have_struct_size(config)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyPhysicsContactConfig::struct_size does not match this build");
        }
        f.set_config(to_engine(*config));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_contact_filter_begin_frame(SturdyPhysicsContactFilter filter, double time_seconds) {
    return with<SFT::Physics::ContactEventFilter>(filter, HandleKind::PhysicsContactFilter, [&](SFT::Physics::ContactEventFilter &f) -> SturdyResult {
        f.begin_frame(time_seconds);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_contact_filter_submit(SturdyPhysicsContactFilter filter, const SturdyPhysicsContactPoint *contact,
                                                                  SturdyBool *out_accepted) {
    return with<SFT::Physics::ContactEventFilter>(filter, HandleKind::PhysicsContactFilter, [&](SFT::Physics::ContactEventFilter &f) -> SturdyResult {
        if (contact == nullptr) {
            return null_argument("contact must not be null");
        }
        const bool accepted = f.submit(to_engine(*contact));
        if (out_accepted != nullptr) {
            *out_accepted = accepted ? STURDY_TRUE : STURDY_FALSE;
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_contact_filter_event_count(SturdyPhysicsContactFilter filter, uint32_t *out_count) {
    return with<SFT::Physics::ContactEventFilter>(filter, HandleKind::PhysicsContactFilter, [&](SFT::Physics::ContactEventFilter &f) -> SturdyResult {
        if (out_count == nullptr) {
            return null_argument("output pointer must not be null");
        }
        *out_count = static_cast<u32>(f.events().size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_contact_filter_event(SturdyPhysicsContactFilter filter, uint32_t index, SturdyPhysicsContactEvent *out_event) {
    return with<SFT::Physics::ContactEventFilter>(filter, HandleKind::PhysicsContactFilter, [&](SFT::Physics::ContactEventFilter &f) -> SturdyResult {
        if (out_event == nullptr) {
            return null_argument("output pointer must not be null");
        }
        const auto events = f.events();
        if (index >= events.size()) {
            return null_argument("event index is out of range");
        }
        const SFT::Physics::ContactEvent &event = events[index];
        out_event->contact = from_engine(event.contact);
        out_event->kind = event.kind == SFT::Physics::ContactKind::Impact ? STURDY_PHYSICS_CONTACT_IMPACT : STURDY_PHYSICS_CONTACT_SCRAPE;
        out_event->intensity = event.intensity;
        return STURDY_OK;
    });
}

// --- surfaces and ballistics ---

SturdyResult STURDY_ABI_CALL sturdy_physics_surface_material_init(SturdyPhysicsSurfaceMaterial *material) {
    return guarded([&]() -> SturdyResult {
        if (material == nullptr) {
            return null_argument("material must not be null");
        }
        *material = from_engine(SFT::Physics::SurfaceMaterial{});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_surfaces_create(SturdyPhysicsSurfaces *out_surfaces) {
    return guarded([&]() -> SturdyResult { return publish(HandleKind::PhysicsSurfaces, std::make_unique<SFT::Physics::SurfaceTable>(), out_surfaces); });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_surfaces_destroy(SturdyPhysicsSurfaces surfaces) {
    return guarded([&]() -> SturdyResult { return destroy<SFT::Physics::SurfaceTable>(surfaces.token, HandleKind::PhysicsSurfaces); });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_surfaces_add(SturdyPhysicsSurfaces surfaces, const SturdyPhysicsSurfaceMaterial *material, uint32_t *out_id) {
    return with<SFT::Physics::SurfaceTable>(surfaces, HandleKind::PhysicsSurfaces, [&](SFT::Physics::SurfaceTable &table) -> SturdyResult {
        if (material == nullptr || out_id == nullptr) {
            return null_argument("material and output pointer must not be null");
        }
        if (!have_struct_size(material)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyPhysicsSurfaceMaterial::struct_size does not match this build");
        }
        *out_id = table.add(to_engine(*material));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_surfaces_find(SturdyPhysicsSurfaces surfaces, const char *name, uint32_t *out_id) {
    return with<SFT::Physics::SurfaceTable>(surfaces, HandleKind::PhysicsSurfaces, [&](SFT::Physics::SurfaceTable &table) -> SturdyResult {
        if (name == nullptr || out_id == nullptr) {
            return null_argument("name and output pointer must not be null");
        }
        const SFT::Physics::MaterialId id = table.find(SFT::ustr{std::string_view{name}});
        if (id == SFT::Physics::no_material) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no material has that name");
        }
        *out_id = id;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_surfaces_count(SturdyPhysicsSurfaces surfaces, uint32_t *out_count) {
    return with<SFT::Physics::SurfaceTable>(surfaces, HandleKind::PhysicsSurfaces, [&](SFT::Physics::SurfaceTable &table) -> SturdyResult {
        if (out_count == nullptr) {
            return null_argument("output pointer must not be null");
        }
        *out_count = static_cast<u32>(table.size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_surfaces_get(SturdyPhysicsSurfaces surfaces, uint32_t id, SturdyPhysicsSurfaceMaterial *out_material) {
    return with<SFT::Physics::SurfaceTable>(surfaces, HandleKind::PhysicsSurfaces, [&](SFT::Physics::SurfaceTable &table) -> SturdyResult {
        if (out_material == nullptr) {
            return null_argument("output pointer must not be null");
        }
        const SFT::Physics::SurfaceMaterial *material = table.get(id);
        if (material == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no material has that id");
        }
        *out_material = from_engine(*material);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_surfaces_name(SturdyPhysicsSurfaces surfaces, uint32_t id, char *buffer, size_t capacity, size_t *out_length) {
    return with<SFT::Physics::SurfaceTable>(surfaces, HandleKind::PhysicsSurfaces, [&](SFT::Physics::SurfaceTable &table) -> SturdyResult {
        const SFT::Physics::SurfaceMaterial *material = table.get(id);
        if (material == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no material has that id");
        }
        return copy_string_out(material->name.cpp_string_view(), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_resolve_impact(const SturdyPhysicsProjectile *projectile, const SturdyPhysicsSurfaceMaterial *material,
                                                           SturdyVec3 entry_point, SturdyVec3 normal, float thickness, float random_x, float random_y,
                                                           SturdyPhysicsImpactResult *out_result) {
    return guarded([&]() -> SturdyResult {
        if (projectile == nullptr || material == nullptr || out_result == nullptr) {
            return null_argument("projectile, material and output pointer must not be null");
        }
        if (!have_struct_size(material)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyPhysicsSurfaceMaterial::struct_size does not match this build");
        }
        if (!usable_axis(to_glm(projectile->direction)) || !usable_axis(to_glm(normal))) {
            return null_argument("projectile direction and surface normal must be non-zero vectors");
        }
        const SFT::Physics::Projectile shot{to_glm(projectile->position), glm::normalize(to_glm(projectile->direction)), projectile->energy};
        *out_result = from_engine(SFT::Physics::resolve_impact(shot, to_engine(*material), to_glm(entry_point), glm::normalize(to_glm(normal)), thickness,
                                                               {random_x, random_y}));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_shot_options_init(SturdyPhysicsShotOptions *options) {
    return guarded([&]() -> SturdyResult {
        if (options == nullptr) {
            return null_argument("options must not be null");
        }
        const SFT::Physics::ShotOptions d;
        *options = {sizeof(SturdyPhysicsShotOptions), d.max_distance, d.max_impacts, d.minimum_energy, d.seed};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_trace_shot(SturdyPhysicsSurfaces surfaces, const SturdyPhysicsProjectile *projectile, const SturdyPhysicsShotOptions *options,
                                                       SturdyPhysicsRayQuery query, void *user, SturdyPhysicsShotImpact *out_impacts, uint32_t capacity,
                                                       uint32_t *out_count) {
    return with<SFT::Physics::SurfaceTable>(surfaces, HandleKind::PhysicsSurfaces, [&](SFT::Physics::SurfaceTable &table) -> SturdyResult {
        if (projectile == nullptr || query == nullptr || out_count == nullptr) {
            return null_argument("projectile, query and out_count must not be null");
        }
        SFT::Physics::ShotOptions engine_options;
        if (options != nullptr) {
            if (!have_struct_size(options)) {
                return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyPhysicsShotOptions::struct_size does not match this build");
            }
            engine_options.max_distance = options->max_distance;
            engine_options.max_impacts = options->max_impacts;
            engine_options.minimum_energy = options->minimum_energy;
            engine_options.seed = options->seed;
        }
        if (!usable_axis(to_glm(projectile->direction))) {
            return null_argument("projectile direction must be a non-zero vector");
        }
        const SFT::Physics::Projectile shot{to_glm(projectile->position), glm::normalize(to_glm(projectile->direction)), projectile->energy};
        const SFT::Physics::RayQuery ray = [&](const glm::vec3 &origin, const glm::vec3 &direction, float max_distance, SFT::Physics::SurfaceHit &hit) {
            SturdyPhysicsSurfaceHit foreign{};
            if (query(user, from_glm(origin), from_glm(direction), max_distance, &foreign) == 0) {
                return false;
            }
            hit.point = to_glm(foreign.point);
            hit.normal = to_glm(foreign.normal);
            hit.distance = foreign.distance;
            hit.thickness = foreign.thickness;
            hit.material = foreign.material;
            hit.object = foreign.object;
            return true;
        };
        const std::vector<SFT::Physics::ShotImpact> impacts = SFT::Physics::trace_shot(shot, table, ray, engine_options);
        return copy_list(impacts, out_impacts, capacity, out_count, [](const SFT::Physics::ShotImpact &impact) {
            SturdyPhysicsShotImpact out{};
            out.hit = {from_glm(impact.hit.point), from_glm(impact.hit.normal), impact.hit.distance, impact.hit.thickness, impact.hit.material, impact.hit.object};
            out.result = from_engine(impact.result);
            return out;
        });
    });
}

// --- cloth ---

SturdyResult STURDY_ABI_CALL sturdy_physics_cloth_params_init(SturdyPhysicsClothParams *params) {
    return guarded([&]() -> SturdyResult {
        if (params == nullptr) {
            return null_argument("params must not be null");
        }
        const SFT::Physics::ClothParams d;
        *params = {sizeof(SturdyPhysicsClothParams), from_glm(d.gravity), d.stretch_compliance, d.bend_compliance, d.damping, d.substeps,
                   d.collision_thickness,            d.friction,          from_glm(d.wind),     d.drag};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_cloth_create_grid(uint32_t columns, uint32_t rows, float spacing, SturdyVec3 origin, SturdyVec3 right, SturdyVec3 down,
                                                              float total_mass, SturdyPhysicsCloth *out_cloth) {
    return guarded([&]() -> SturdyResult {
        if (columns < 2 || rows < 2) {
            return null_argument("a cloth grid needs at least 2 columns and 2 rows");
        }
        if (static_cast<u64>(columns) * rows > max_cloth_particles) {
            return null_argument("cloth grid is too large");
        }
        if (!(spacing > 0.0f) || !std::isfinite(spacing) || !(total_mass > 0.0f) || !std::isfinite(total_mass)) {
            return null_argument("spacing and total_mass must be finite and positive");
        }
        return publish(HandleKind::PhysicsCloth,
                       std::make_unique<SFT::Physics::Cloth>(SFT::Physics::Cloth::grid(columns, rows, spacing, to_glm(origin), to_glm(right), to_glm(down), total_mass)),
                       out_cloth);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_cloth_create_from_triangles(const SturdyVec3 *positions, uint32_t vertex_count, const uint32_t *indices,
                                                                        uint32_t index_count, float total_mass, SturdyPhysicsCloth *out_cloth) {
    return guarded([&]() -> SturdyResult {
        if (positions == nullptr || indices == nullptr || vertex_count == 0 || index_count == 0) {
            return null_argument("positions and indices must not be empty");
        }
        if (index_count % 3 != 0) {
            return null_argument("index_count must be a multiple of 3");
        }
        if (vertex_count > max_cloth_particles) {
            return null_argument("cloth mesh is too large");
        }
        if (!(total_mass > 0.0f) || !std::isfinite(total_mass)) {
            return null_argument("total_mass must be finite and positive");
        }
        for (u32 i = 0; i < index_count; ++i) {
            if (indices[i] >= vertex_count) {
                return null_argument("an index refers past the last vertex");
            }
        }
        std::vector<glm::vec3> converted;
        converted.reserve(vertex_count);
        for (u32 i = 0; i < vertex_count; ++i) {
            converted.push_back(to_glm(positions[i]));
        }
        return publish(HandleKind::PhysicsCloth,
                       std::make_unique<SFT::Physics::Cloth>(SFT::Physics::Cloth::from_triangles(converted, std::span<const u32>{indices, index_count}, total_mass)),
                       out_cloth);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_cloth_destroy(SturdyPhysicsCloth cloth) {
    return guarded([&]() -> SturdyResult { return destroy<SFT::Physics::Cloth>(cloth.token, HandleKind::PhysicsCloth); });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_cloth_particle_count(SturdyPhysicsCloth cloth, uint32_t *out_count) {
    return with<SFT::Physics::Cloth>(cloth, HandleKind::PhysicsCloth, [&](SFT::Physics::Cloth &c) -> SturdyResult {
        if (out_count == nullptr) {
            return null_argument("output pointer must not be null");
        }
        *out_count = static_cast<u32>(c.particle_count());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_cloth_constraint_counts(SturdyPhysicsCloth cloth, uint32_t *out_stretch, uint32_t *out_bend) {
    return with<SFT::Physics::Cloth>(cloth, HandleKind::PhysicsCloth, [&](SFT::Physics::Cloth &c) -> SturdyResult {
        if (out_stretch != nullptr) {
            *out_stretch = static_cast<u32>(c.stretch_constraint_count());
        }
        if (out_bend != nullptr) {
            *out_bend = static_cast<u32>(c.bend_constraint_count());
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_cloth_positions(SturdyPhysicsCloth cloth, SturdyVec3 *out_positions, uint32_t capacity, uint32_t *out_count) {
    return with<SFT::Physics::Cloth>(cloth, HandleKind::PhysicsCloth, [&](SFT::Physics::Cloth &c) -> SturdyResult {
        return copy_list(c.positions(), out_positions, capacity, out_count, [](const glm::vec3 &p) { return from_glm(p); });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_cloth_indices(SturdyPhysicsCloth cloth, uint32_t *out_indices, uint32_t capacity, uint32_t *out_count) {
    return with<SFT::Physics::Cloth>(cloth, HandleKind::PhysicsCloth, [&](SFT::Physics::Cloth &c) -> SturdyResult {
        return copy_list(c.indices(), out_indices, capacity, out_count, [](u32 index) { return index; });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_cloth_pin(SturdyPhysicsCloth cloth, uint32_t particle, SturdyBool pinned) {
    return with<SFT::Physics::Cloth>(cloth, HandleKind::PhysicsCloth, [&](SFT::Physics::Cloth &c) -> SturdyResult {
        if (particle >= c.particle_count()) {
            return null_argument("particle index is out of range");
        }
        c.pin(particle, pinned != 0);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_cloth_is_pinned(SturdyPhysicsCloth cloth, uint32_t particle, SturdyBool *out_pinned) {
    return with<SFT::Physics::Cloth>(cloth, HandleKind::PhysicsCloth, [&](SFT::Physics::Cloth &c) -> SturdyResult {
        if (out_pinned == nullptr) {
            return null_argument("output pointer must not be null");
        }
        if (particle >= c.particle_count()) {
            return null_argument("particle index is out of range");
        }
        *out_pinned = c.is_pinned(particle) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_cloth_move_pinned(SturdyPhysicsCloth cloth, uint32_t particle, SturdyVec3 position) {
    return with<SFT::Physics::Cloth>(cloth, HandleKind::PhysicsCloth, [&](SFT::Physics::Cloth &c) -> SturdyResult {
        if (particle >= c.particle_count()) {
            return null_argument("particle index is out of range");
        }
        c.move_pinned(particle, to_glm(position));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_cloth_step(SturdyPhysicsCloth cloth, float dt, const SturdyPhysicsClothParams *params,
                                                       const SturdyPhysicsClothColliders *colliders) {
    return with<SFT::Physics::Cloth>(cloth, HandleKind::PhysicsCloth, [&](SFT::Physics::Cloth &c) -> SturdyResult {
        if (!std::isfinite(dt) || dt < 0.0f) {
            return null_argument("dt must be finite and not negative");
        }
        SFT::Physics::ClothParams engine_params;
        if (params != nullptr) {
            if (!have_struct_size(params)) {
                return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyPhysicsClothParams::struct_size does not match this build");
            }
            engine_params.gravity = to_glm(params->gravity);
            engine_params.stretch_compliance = params->stretch_compliance;
            engine_params.bend_compliance = params->bend_compliance;
            engine_params.damping = params->damping;
            engine_params.substeps = params->substeps;
            engine_params.collision_thickness = params->collision_thickness;
            engine_params.friction = params->friction;
            engine_params.wind = to_glm(params->wind);
            engine_params.drag = params->drag;
        }
        std::vector<SFT::Physics::ClothSphere> spheres;
        std::vector<SFT::Physics::ClothCapsule> capsules;
        std::vector<SFT::Physics::ClothPlane> planes;
        if (colliders != nullptr) {
            if ((colliders->sphere_count != 0 && colliders->spheres == nullptr) || (colliders->capsule_count != 0 && colliders->capsules == nullptr) ||
                (colliders->plane_count != 0 && colliders->planes == nullptr)) {
                return null_argument("a collider array is null but its count is not zero");
            }
            for (u32 i = 0; i < colliders->sphere_count; ++i) {
                spheres.push_back({to_glm(colliders->spheres[i].center), colliders->spheres[i].radius});
            }
            for (u32 i = 0; i < colliders->capsule_count; ++i) {
                capsules.push_back({to_glm(colliders->capsules[i].a), to_glm(colliders->capsules[i].b), colliders->capsules[i].radius});
            }
            for (u32 i = 0; i < colliders->plane_count; ++i) {
                planes.push_back({to_glm(colliders->planes[i].normal), colliders->planes[i].offset});
            }
        }
        c.step(dt, engine_params, SFT::Physics::ClothColliders{spheres, capsules, planes});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_cloth_normals(SturdyPhysicsCloth cloth, SturdyVec3 *out_normals, uint32_t capacity, uint32_t *out_count) {
    return with<SFT::Physics::Cloth>(cloth, HandleKind::PhysicsCloth, [&](SFT::Physics::Cloth &c) -> SturdyResult {
        std::vector<glm::vec3> normals;
        c.compute_normals(normals);
        return copy_list(normals, out_normals, capacity, out_count, [](const glm::vec3 &n) { return from_glm(n); });
    });
}

// --- swing / twist joint maths ---

SturdyResult STURDY_ABI_CALL sturdy_physics_decompose_swing_twist(SturdyQuat rotation, SturdyVec3 axis, SturdyQuat *out_swing, SturdyQuat *out_twist) {
    return guarded([&]() -> SturdyResult {
        if (out_swing == nullptr || out_twist == nullptr) {
            return null_argument("output pointers must not be null");
        }
        if (!usable_axis(to_glm(axis))) {
            return null_argument("axis must be a non-zero vector");
        }
        const SFT::Physics::SwingTwist split = SFT::Physics::decompose_swing_twist(to_glm(rotation), glm::normalize(to_glm(axis)));
        *out_swing = from_glm(split.swing);
        *out_twist = from_glm(split.twist);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_twist_angle(SturdyQuat rotation, SturdyVec3 axis, float *out_angle) {
    return guarded([&]() -> SturdyResult {
        if (out_angle == nullptr) {
            return null_argument("output pointer must not be null");
        }
        if (!usable_axis(to_glm(axis))) {
            return null_argument("axis must be a non-zero vector");
        }
        *out_angle = SFT::Physics::twist_angle(to_glm(rotation), glm::normalize(to_glm(axis)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_within_limits(SturdyQuat rotation, const SturdyPhysicsSwingTwistLimits *limits, SturdyBool *out_within) {
    return guarded([&]() -> SturdyResult {
        if (limits == nullptr || out_within == nullptr) {
            return null_argument("limits and output pointer must not be null");
        }
        if (!usable_axis(to_glm(limits->axis))) {
            return null_argument("limits axis must be a non-zero vector");
        }
        SFT::Physics::SwingTwistLimits engine_limits = to_engine(*limits);
        engine_limits.axis = glm::normalize(engine_limits.axis);
        *out_within = SFT::Physics::within_limits(to_glm(rotation), engine_limits) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_clamp_to_limits(SturdyQuat rotation, const SturdyPhysicsSwingTwistLimits *limits, SturdyQuat *out_rotation) {
    return guarded([&]() -> SturdyResult {
        if (limits == nullptr || out_rotation == nullptr) {
            return null_argument("limits and output pointer must not be null");
        }
        if (!usable_axis(to_glm(limits->axis))) {
            return null_argument("limits axis must be a non-zero vector");
        }
        SFT::Physics::SwingTwistLimits engine_limits = to_engine(*limits);
        engine_limits.axis = glm::normalize(engine_limits.axis);
        *out_rotation = from_glm(SFT::Physics::clamp_to_limits(to_glm(rotation), engine_limits));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_physics_pd_drive(SturdyQuat current, SturdyQuat target, SturdyVec3 angular_velocity, float stiffness, float damping,
                                                     SturdyVec3 *out_velocity) {
    return guarded([&]() -> SturdyResult {
        if (out_velocity == nullptr) {
            return null_argument("output pointer must not be null");
        }
        *out_velocity = from_glm(SFT::Physics::pd_drive(to_glm(current), to_glm(target), to_glm(angular_velocity), stiffness, damping));
        return STURDY_OK;
    });
}

} // extern "C"
