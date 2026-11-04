#pragma once

// Plain-data records for the physics surface (`CxxApi/Physics.hpp`). bindgen-safe (see plans/cxx-api.md): `<cstdint>` only, C arrays,
// trivially copyable. Vectors are x, y, z; quaternions x, y, z, w; metres, right-handed.

#include <cstddef>
#include <cstdint>

namespace SFT::CxxApi {

    /// "No material" in a `PhysicsContactPoint`.
    inline constexpr std::uint32_t physics_no_material = 0xFFFFFFFFu;

    struct PhysicsPose {
        float position[3] = {0.0f, 0.0f, 0.0f};
        float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    };

    struct PhysicsBox {
        float min[3] = {0.0f, 0.0f, 0.0f};
        float max[3] = {1.0f, 1.0f, 1.0f};
        std::uint64_t object = 0;
        /// Reported to a character so it can ride the box.
        float velocity[3] = {0.0f, 0.0f, 0.0f};
    };

    struct PhysicsPlane {
        float normal[3] = {0.0f, 1.0f, 0.0f};
        float offset = 0.0f;
        std::uint64_t object = 0;
    };

    struct PhysicsSweepHit {
        float fraction = 1.0f;
        float normal[3] = {0.0f, 1.0f, 0.0f};
        float point[3] = {0.0f, 0.0f, 0.0f};
        std::uint64_t object = 0;
        float object_velocity[3] = {0.0f, 0.0f, 0.0f};
    };

    /// Mirrors `Physics::CharacterSettings` (defaults checked by `CxxApiPhysicsTest`).
    struct PhysicsCharacterSettings {
        float radius = 0.3f;
        float standing_height = 1.8f;
        float crouching_height = 1.0f;
        float up[3] = {0.0f, 1.0f, 0.0f};
        float gravity = 20.0f;
        float slope_limit_degrees = 50.0f;
        float step_height = 0.35f;
        float snap_distance = 0.25f;
        float skin_width = 0.01f;
        std::uint32_t max_slides = 4;
        float jump_speed = 7.0f;
        float terminal_speed = 60.0f;
    };

    struct PhysicsMoveResult {
        /// Position change this call (feet), including platform carry.
        float displacement[3] = {0.0f, 0.0f, 0.0f};
        bool grounded = false;
        bool hit_ceiling = false;
        bool stepped_up = false;
        bool jumped = false;
        bool landed = false;
        float landing_speed = 0.0f;
    };

    struct PhysicsCharacterState {
        float position[3] = {0.0f, 0.0f, 0.0f};
        float velocity[3] = {0.0f, 0.0f, 0.0f};
        float ground_normal[3] = {0.0f, 1.0f, 0.0f};
        std::uint64_t ground_object = 0;
        float height = 0.0f;
        bool grounded = false;
        bool crouching = false;
    };

    struct PhysicsContactPoint {
        std::uint64_t body_a = 0;
        std::uint64_t body_b = 0;
        float point[3] = {0.0f, 0.0f, 0.0f};
        float normal[3] = {0.0f, 1.0f, 0.0f};
        /// Impulse along the normal (N*s).
        float normal_impulse = 0.0f;
        float approach_speed = 0.0f;
        float slide_speed = 0.0f;
        std::uint32_t material_a = physics_no_material;
        std::uint32_t material_b = physics_no_material;
    };

    struct PhysicsContactEvent {
        PhysicsContactPoint contact;
        /// False = impact, true = scrape.
        bool scrape = false;
        /// 0..1, a perceptual mapping of the impulse (log scale) for volume and effect intensity.
        float intensity = 0.0f;
    };

    struct PhysicsContactConfig {
        float min_impulse = 0.5f;
        float min_speed = 0.6f;
        float pair_cooldown_seconds = 0.1f;
        float merge_radius = 0.25f;
        std::uint32_t max_events_per_frame = 64;
        float full_intensity_impulse = 200.0f;
    };

    /// A material's numbers; its name is given separately (`physics_surfaces_add`).
    struct PhysicsSurfaceMaterial {
        float penetration_resistance = 5000.0f;
        float ricochet_angle_degrees = 10.0f;
        float ricochet_energy_retained = 0.5f;
        float ricochet_scatter = 0.08f;
        float exit_deflection = 0.12f;
    };

    struct PhysicsProjectile {
        float position[3] = {0.0f, 0.0f, 0.0f};
        float direction[3] = {0.0f, 0.0f, -1.0f};
        float energy = 3000.0f;
    };

    /// `outcome`: 0 = stopped, 1 = penetrated, 2 = ricocheted.
    struct PhysicsImpactResult {
        std::uint8_t outcome = 0;
        float point[3] = {0.0f, 0.0f, 0.0f};
        float direction[3] = {0.0f, 0.0f, 0.0f};
        float remaining_energy = 0.0f;
        float depth = 0.0f;
    };

    struct PhysicsSurfaceHit {
        float point[3] = {0.0f, 0.0f, 0.0f};
        float normal[3] = {0.0f, 1.0f, 0.0f};
        float distance = 0.0f;
        float thickness = 0.0f;
        std::uint32_t material = physics_no_material;
        std::uint64_t object = 0;
    };

    struct PhysicsShotImpact {
        PhysicsSurfaceHit hit;
        PhysicsImpactResult result;
    };

    struct PhysicsShotOptions {
        float max_distance = 1000.0f;
        std::uint32_t max_impacts = 8;
        float minimum_energy = 1.0f;
        std::uint64_t seed = 1;
    };

    struct PhysicsClothParams {
        float gravity[3] = {0.0f, -9.81f, 0.0f};
        float stretch_compliance = 0.0f;
        float bend_compliance = 5e-3f;
        float damping = 0.2f;
        std::uint32_t substeps = 8;
        float collision_thickness = 0.005f;
        float friction = 0.4f;
        float wind[3] = {0.0f, 0.0f, 0.0f};
        float drag = 0.5f;
    };

    struct PhysicsClothSphere {
        float center[3] = {0.0f, 0.0f, 0.0f};
        float radius = 0.1f;
    };

    struct PhysicsClothCapsule {
        float a[3] = {0.0f, 0.0f, 0.0f};
        float b[3] = {0.0f, 1.0f, 0.0f};
        float radius = 0.1f;
    };

    /// Plane through `normal * offset` facing `normal`; particles are kept on the positive side.
    struct PhysicsClothPlane {
        float normal[3] = {0.0f, 1.0f, 0.0f};
        float offset = 0.0f;
    };

    struct PhysicsSwingTwistLimits {
        float axis[3] = {1.0f, 0.0f, 0.0f};
        float swing_y = 0.8f;
        float swing_z = 0.8f;
        float twist_min = -0.5f;
        float twist_max = 0.5f;
    };

    struct PhysicsSwingTwist {
        float swing[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float twist[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    };

} // namespace SFT::CxxApi
