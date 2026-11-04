#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <CxxApi/Types/Physics.hpp>
#include <Physics/Ballistics.hpp>
#include <Physics/CharacterController.hpp>
#include <Physics/Cloth.hpp>
#include <Physics/ContactEvents.hpp>
#include <Physics/FixedStepper.hpp>
#include <Physics/Layers.hpp>
#include <Physics/SimpleWorld.hpp>
#include <Physics/SwingTwist.hpp>

/// Solver-agnostic physics (`Physics/*`): a fixed-timestep accumulator, collision layers, an analytic collision world with a kinematic
/// character controller, contact-event filtering, surface materials with ballistics, XPBD cloth, and swing/twist joint maths. All CPU-side;
/// none of it needs an `Engine`. The objects are the engine's own classes, bound opaquely; construct them with the `*_new` functions.
/// Vectors are `[x, y, z]`, quaternions `[x, y, z, w]`.
namespace SFT::CxxApi {

    // ---- fixed stepper ----

    /// `step_seconds` <= 0 selects 1/60; `max_steps_per_frame` 0 selects 8.
    [[nodiscard]] std::unique_ptr<Physics::FixedStepper> physics_stepper_new(double step_seconds, std::uint32_t max_steps_per_frame);
    /// Adds `delta_seconds` and returns how many fixed steps are now due (negative or non-finite time counts as zero).
    [[nodiscard]] std::uint32_t physics_stepper_advance(Physics::FixedStepper &stepper, double delta_seconds) noexcept;
    [[nodiscard]] double physics_stepper_alpha(const Physics::FixedStepper &stepper) noexcept;
    [[nodiscard]] double physics_stepper_step(const Physics::FixedStepper &stepper) noexcept;
    [[nodiscard]] std::uint64_t physics_stepper_total_steps(const Physics::FixedStepper &stepper) noexcept;
    [[nodiscard]] double physics_stepper_simulated_time(const Physics::FixedStepper &stepper) noexcept;
    [[nodiscard]] double physics_stepper_dropped_seconds(const Physics::FixedStepper &stepper) noexcept;
    /// Ignored unless positive and finite.
    void physics_stepper_set_step(Physics::FixedStepper &stepper, double step_seconds) noexcept;
    void physics_stepper_reset(Physics::FixedStepper &stepper) noexcept;
    /// The pose to draw between two simulation states (positions lerp, rotations slerp the short way).
    [[nodiscard]] PhysicsPose physics_interpolate_pose(const PhysicsPose &previous, const PhysicsPose &current, float alpha) noexcept;

    // ---- layers ----

    [[nodiscard]] std::unique_ptr<Physics::LayerTable> physics_layers_new();
    /// Registers `name` (idempotent); returns false (index untouched) when all 64 layers are taken. A new layer collides with every layer.
    [[nodiscard]] bool physics_layers_add(Physics::LayerTable &layers, const std::string &name, std::uint32_t &index);
    [[nodiscard]] bool physics_layers_find(const Physics::LayerTable &layers, const std::string &name, std::uint32_t &index);
    [[nodiscard]] std::unique_ptr<std::string> physics_layers_name(const Physics::LayerTable &layers, std::uint32_t index);
    [[nodiscard]] std::uint32_t physics_layers_count(const Physics::LayerTable &layers) noexcept;
    /// Union of the named layers' bits (unknown names are ignored).
    [[nodiscard]] std::uint64_t physics_layers_mask_of(const Physics::LayerTable &layers, const std::vector<std::string> &names);
    [[nodiscard]] std::uint64_t physics_layers_all(const Physics::LayerTable &layers) noexcept;
    /// Symmetric. Unregistered indices are ignored.
    void physics_layers_set_collides(Physics::LayerTable &layers, std::uint32_t a, std::uint32_t b, bool collides) noexcept;
    [[nodiscard]] bool physics_layers_collides(const Physics::LayerTable &layers, std::uint32_t a, std::uint32_t b) noexcept;
    /// The layers `layer` collides with, ready to hand to a solver as a filter mask (0 for an unregistered layer).
    [[nodiscard]] std::uint64_t physics_layers_collision_mask(const Physics::LayerTable &layers, std::uint32_t layer) noexcept;

    // ---- collision world + character ----

    [[nodiscard]] std::unique_ptr<Physics::SimpleWorld> physics_world_new();
    /// Adds a box and returns its index for `physics_world_move_box`.
    [[nodiscard]] std::uint32_t physics_world_add_box(Physics::SimpleWorld &world, const PhysicsBox &shape);
    void physics_world_add_plane(Physics::SimpleWorld &world, const PhysicsPlane &plane);
    [[nodiscard]] std::uint32_t physics_world_box_count(const Physics::SimpleWorld &world) noexcept;
    /// Moves a box (a platform) by `delta`; an unknown index is ignored.
    void physics_world_move_box(Physics::SimpleWorld &world, std::uint32_t index, std::array<float, 3> delta) noexcept;
    [[nodiscard]] bool physics_world_overlaps_capsule(const Physics::SimpleWorld &world, std::array<float, 3> feet, float radius, float height);
    /// Sweeps a vertical capsule (feet at `feet`) by `delta`; false when nothing is hit (`hit` untouched).
    [[nodiscard]] bool physics_world_sweep_capsule(const Physics::SimpleWorld &world, std::array<float, 3> feet, float radius, float height,
                                                   std::array<float, 3> delta, PhysicsSweepHit &hit);
    [[nodiscard]] std::unique_ptr<Physics::CharacterController> physics_character_new(const PhysicsCharacterSettings &settings, std::array<float, 3> feet);
    /// Moves for `dt` seconds toward `desired_velocity` (horizontal, world space). `jump` launches when grounded.
    [[nodiscard]] PhysicsMoveResult physics_character_move(Physics::CharacterController &character, const Physics::SimpleWorld &world,
                                                           std::array<float, 3> desired_velocity, float dt, bool jump);
    /// Crouches at once; standing up succeeds only with headroom. Returns the resulting crouch state.
    [[nodiscard]] bool physics_character_set_crouching(Physics::CharacterController &character, const Physics::SimpleWorld &world, bool crouching);
    void physics_character_teleport(Physics::CharacterController &character, std::array<float, 3> feet) noexcept;
    void physics_character_add_impulse(Physics::CharacterController &character, std::array<float, 3> velocity_change) noexcept;
    [[nodiscard]] PhysicsCharacterState physics_character_state(const Physics::CharacterController &character) noexcept;

    // ---- contact events ----

    [[nodiscard]] std::unique_ptr<Physics::ContactEventFilter> physics_contacts_new(const PhysicsContactConfig &config);
    void physics_contacts_set_config(Physics::ContactEventFilter &filter, const PhysicsContactConfig &config);
    /// Clears last frame's events and sets the clock used for cooldowns.
    void physics_contacts_begin_frame(Physics::ContactEventFilter &filter, double time_seconds);
    /// Offers a contact; true if it became (or replaced) an event.
    [[nodiscard]] bool physics_contacts_submit(Physics::ContactEventFilter &filter, const PhysicsContactPoint &contact);
    [[nodiscard]] std::unique_ptr<std::vector<PhysicsContactEvent>> physics_contacts_events(const Physics::ContactEventFilter &filter);

    // ---- surfaces + ballistics ----

    [[nodiscard]] std::unique_ptr<Physics::SurfaceTable> physics_surfaces_new();
    /// Appends a material and returns its id.
    [[nodiscard]] std::uint32_t physics_surfaces_add(Physics::SurfaceTable &surfaces, const std::string &name, const PhysicsSurfaceMaterial &material);
    [[nodiscard]] bool physics_surfaces_find(const Physics::SurfaceTable &surfaces, const std::string &name, std::uint32_t &id);
    [[nodiscard]] std::uint32_t physics_surfaces_count(const Physics::SurfaceTable &surfaces) noexcept;
    /// False for an unknown id (outputs untouched).
    [[nodiscard]] bool physics_surfaces_get(const Physics::SurfaceTable &surfaces, std::uint32_t id, PhysicsSurfaceMaterial &material);
    [[nodiscard]] std::unique_ptr<std::string> physics_surfaces_name(const Physics::SurfaceTable &surfaces, std::uint32_t id);
    /// Resolves one entry into one surface: `normal` faces the projectile, `thickness` is the distance the straight ray spends inside (very large
    /// for terrain), `random` is two uniform values in [0, 1) for scatter (pass fixed values for deterministic replays).
    [[nodiscard]] PhysicsImpactResult physics_resolve_impact(const PhysicsProjectile &projectile, const PhysicsSurfaceMaterial &material,
                                                             std::array<float, 3> entry_point, std::array<float, 3> normal, float thickness,
                                                             std::array<float, 2> random);

    /// A ray query for `physics_trace_shot`: report the nearest surface along the segment (write `hit`, return true) or return false.
    class PhysicsRayQuery {
      public:
        virtual ~PhysicsRayQuery() = default;
        virtual bool cast(std::array<float, 3> origin, std::array<float, 3> direction, float max_distance, PhysicsSurfaceHit &hit) = 0;
    };

    /// Traces a projectile through the world (ray, impact, continue from the exit or ricochet) until it stops, leaves range or runs out of impacts.
    [[nodiscard]] std::unique_ptr<std::vector<PhysicsShotImpact>> physics_trace_shot(const Physics::SurfaceTable &surfaces, const PhysicsProjectile &projectile,
                                                                                     const PhysicsShotOptions &options, PhysicsRayQuery &query);

    // ---- cloth ----

    /// A `columns x rows` sheet `spacing` apart. Throws `Error` for a grid under 2x2, a non-positive spacing or mass, or an enormous grid.
    [[nodiscard]] std::unique_ptr<Physics::Cloth> physics_cloth_grid(std::uint32_t columns, std::uint32_t rows, float spacing, std::array<float, 3> origin,
                                                                     std::array<float, 3> right, std::array<float, 3> down, float total_mass);
    /// `positions_xyz` is x, y, z triples; `indices` triangles. Throws `Error` for empty or malformed input or an index past the vertices.
    [[nodiscard]] std::unique_ptr<Physics::Cloth> physics_cloth_from_triangles(const std::vector<float> &positions_xyz, const std::vector<std::uint32_t> &indices,
                                                                                float total_mass);
    [[nodiscard]] std::uint32_t physics_cloth_particle_count(const Physics::Cloth &cloth) noexcept;
    [[nodiscard]] std::uint32_t physics_cloth_stretch_constraints(const Physics::Cloth &cloth) noexcept;
    [[nodiscard]] std::uint32_t physics_cloth_bend_constraints(const Physics::Cloth &cloth) noexcept;
    /// x, y, z per particle.
    [[nodiscard]] std::unique_ptr<std::vector<float>> physics_cloth_positions(const Physics::Cloth &cloth);
    [[nodiscard]] std::unique_ptr<std::vector<std::uint32_t>> physics_cloth_indices(const Physics::Cloth &cloth);
    [[nodiscard]] std::unique_ptr<std::vector<float>> physics_cloth_normals(const Physics::Cloth &cloth);
    /// An unknown particle is ignored.
    void physics_cloth_pin(Physics::Cloth &cloth, std::uint32_t particle, bool pinned) noexcept;
    [[nodiscard]] bool physics_cloth_is_pinned(const Physics::Cloth &cloth, std::uint32_t particle) noexcept;
    void physics_cloth_move_pinned(Physics::Cloth &cloth, std::uint32_t particle, std::array<float, 3> position) noexcept;
    /// Advances by `dt` seconds with no colliders.
    void physics_cloth_step(Physics::Cloth &cloth, float dt, const PhysicsClothParams &params);
    /// Advances by `dt` seconds against spheres, capsules and planes (a flat list each).
    void physics_cloth_step_colliders(Physics::Cloth &cloth, float dt, const PhysicsClothParams &params, const std::vector<PhysicsClothSphere> &spheres,
                                      const std::vector<PhysicsClothCapsule> &capsules, const std::vector<PhysicsClothPlane> &planes);

    // ---- swing / twist ----

    [[nodiscard]] PhysicsSwingTwist physics_decompose_swing_twist(std::array<float, 4> rotation, std::array<float, 3> axis);
    [[nodiscard]] float physics_twist_angle(std::array<float, 4> rotation, std::array<float, 3> axis);
    [[nodiscard]] bool physics_within_limits(std::array<float, 4> rotation, const PhysicsSwingTwistLimits &limits);
    [[nodiscard]] std::array<float, 4> physics_clamp_to_limits(std::array<float, 4> rotation, const PhysicsSwingTwistLimits &limits);
    [[nodiscard]] std::array<float, 3> physics_pd_drive(std::array<float, 4> current, std::array<float, 4> target, std::array<float, 3> angular_velocity,
                                                        float stiffness, float damping);

    // ---- defaults (the records above carry the engine defaults; these exist for bindings that need them as a call) ----

    [[nodiscard]] PhysicsCharacterSettings physics_character_settings_defaults() noexcept;
    [[nodiscard]] PhysicsContactConfig physics_contact_config_defaults() noexcept;
    [[nodiscard]] PhysicsSurfaceMaterial physics_surface_material_defaults() noexcept;
    [[nodiscard]] PhysicsShotOptions physics_shot_options_defaults() noexcept;
    [[nodiscard]] PhysicsClothParams physics_cloth_params_defaults() noexcept;
    [[nodiscard]] PhysicsSwingTwistLimits physics_swing_twist_limits_defaults() noexcept;

} // namespace SFT::CxxApi
