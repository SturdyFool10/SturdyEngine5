// Rust <-> SturdyEngine 5 shim for the physics package (Physics/*). See src/physics.rs.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <rust/cxx.h>

#include <Physics/Ballistics.hpp>
#include <Physics/CharacterController.hpp>
#include <Physics/Cloth.hpp>
#include <Physics/ContactEvents.hpp>
#include <Physics/FixedStepper.hpp>
#include <Physics/Layers.hpp>
#include <Physics/SimpleWorld.hpp>
#include <Physics/SwingTwist.hpp>

namespace sturdy_rs::physics {

    struct PoseDesc;
    struct BoxDesc;
    struct PlaneDesc;
    struct SweepHitDesc;
    struct CharacterSettingsDesc;
    struct MoveResultDesc;
    struct CharacterStateDesc;
    struct ContactPointDesc;
    struct ContactEventDesc;
    struct ContactConfigDesc;
    struct SurfaceMaterialDesc;
    struct ProjectileDesc;
    struct ImpactResultDesc;
    struct SurfaceHitDesc;
    struct ShotImpactDesc;
    struct ShotOptionsDesc;
    struct ClothParamsDesc;
    struct ClothSphereDesc;
    struct ClothCapsuleDesc;
    struct ClothPlaneDesc;
    struct SwingTwistLimitsDesc;
    struct SwingTwistDesc;
    struct RayQuery;

    using StepperBox = SFT::Physics::FixedStepper;
    using LayersBox = SFT::Physics::LayerTable;
    using WorldBox = SFT::Physics::SimpleWorld;
    using CharacterBox = SFT::Physics::CharacterController;
    using ContactFilterBox = SFT::Physics::ContactEventFilter;
    using SurfacesBox = SFT::Physics::SurfaceTable;
    using ClothBox = SFT::Physics::Cloth;

    std::unique_ptr<StepperBox> stepper_new(double step_seconds, std::uint32_t max_steps_per_frame);
    std::uint32_t stepper_advance(StepperBox &stepper, double delta_seconds);
    double stepper_alpha(const StepperBox &stepper);
    double stepper_step(const StepperBox &stepper);
    std::uint64_t stepper_total_steps(const StepperBox &stepper);
    double stepper_simulated_time(const StepperBox &stepper);
    double stepper_dropped_seconds(const StepperBox &stepper);
    void stepper_set_step(StepperBox &stepper, double step_seconds);
    void stepper_reset(StepperBox &stepper);
    PoseDesc interpolate_pose(const PoseDesc &previous, const PoseDesc &current, float alpha);

    std::unique_ptr<LayersBox> layers_new();
    std::uint32_t layers_add(LayersBox &layers, rust::Str name, bool &found);
    std::uint32_t layers_find(const LayersBox &layers, rust::Str name, bool &found);
    rust::String layers_name(const LayersBox &layers, std::uint32_t index);
    std::uint32_t layers_count(const LayersBox &layers);
    std::uint64_t layers_mask_of(const LayersBox &layers, const rust::Vec<rust::String> &names);
    std::uint64_t layers_all(const LayersBox &layers);
    void layers_set_collides(LayersBox &layers, std::uint32_t a, std::uint32_t b, bool collides);
    bool layers_collides(const LayersBox &layers, std::uint32_t a, std::uint32_t b);
    std::uint64_t layers_collision_mask(const LayersBox &layers, std::uint32_t layer);

    std::unique_ptr<WorldBox> world_new();
    std::uint32_t world_add_box(WorldBox &world, const BoxDesc &desc);
    void world_add_plane(WorldBox &world, const PlaneDesc &desc);
    std::uint32_t world_box_count(const WorldBox &world);
    void world_move_box(WorldBox &world, std::uint32_t index, std::array<float, 3> delta);
    bool world_overlaps_capsule(const WorldBox &world, std::array<float, 3> feet, float radius, float height);
    bool world_sweep_capsule(const WorldBox &world, std::array<float, 3> feet, float radius, float height, std::array<float, 3> delta, SweepHitDesc &hit);
    CharacterSettingsDesc character_settings_default();
    std::unique_ptr<CharacterBox> character_new(const CharacterSettingsDesc &settings, std::array<float, 3> feet);
    MoveResultDesc character_move(CharacterBox &character, const WorldBox &world, std::array<float, 3> desired_velocity, float dt, bool jump);
    bool character_set_crouching(CharacterBox &character, const WorldBox &world, bool crouching);
    void character_teleport(CharacterBox &character, std::array<float, 3> feet);
    void character_add_impulse(CharacterBox &character, std::array<float, 3> velocity_change);
    CharacterStateDesc character_state(const CharacterBox &character);

    ContactConfigDesc contact_config_default();
    std::unique_ptr<ContactFilterBox> contact_filter_new(const ContactConfigDesc &config);
    void contact_filter_set_config(ContactFilterBox &filter, const ContactConfigDesc &config);
    void contact_filter_begin_frame(ContactFilterBox &filter, double time_seconds);
    bool contact_filter_submit(ContactFilterBox &filter, const ContactPointDesc &contact);
    rust::Vec<ContactEventDesc> contact_filter_events(const ContactFilterBox &filter);

    SurfaceMaterialDesc surface_material_default();
    std::unique_ptr<SurfacesBox> surfaces_new();
    std::uint32_t surfaces_add(SurfacesBox &surfaces, const SurfaceMaterialDesc &material);
    std::uint32_t surfaces_find(const SurfacesBox &surfaces, rust::Str name, bool &found);
    std::uint32_t surfaces_count(const SurfacesBox &surfaces);
    bool surfaces_get(const SurfacesBox &surfaces, std::uint32_t id, SurfaceMaterialDesc &out);
    ImpactResultDesc resolve_impact(const ProjectileDesc &projectile, const SurfaceMaterialDesc &material, std::array<float, 3> entry_point,
                                    std::array<float, 3> normal, float thickness, std::array<float, 2> random);
    ShotOptionsDesc shot_options_default();
    rust::Vec<ShotImpactDesc> trace_shot(const SurfacesBox &surfaces, const ProjectileDesc &projectile, const ShotOptionsDesc &options, RayQuery &query);

    ClothParamsDesc cloth_params_default();
    std::unique_ptr<ClothBox> cloth_grid(std::uint32_t columns, std::uint32_t rows, float spacing, std::array<float, 3> origin, std::array<float, 3> right,
                                         std::array<float, 3> down, float total_mass);
    std::unique_ptr<ClothBox> cloth_from_triangles(rust::Slice<const float> positions, rust::Slice<const std::uint32_t> indices, float total_mass);
    std::uint32_t cloth_particle_count(const ClothBox &cloth);
    std::array<std::uint32_t, 2> cloth_constraint_counts(const ClothBox &cloth);
    rust::Vec<float> cloth_positions(const ClothBox &cloth);
    rust::Vec<std::uint32_t> cloth_indices(const ClothBox &cloth);
    void cloth_pin(ClothBox &cloth, std::uint32_t particle, bool pinned);
    bool cloth_is_pinned(const ClothBox &cloth, std::uint32_t particle);
    void cloth_move_pinned(ClothBox &cloth, std::uint32_t particle, std::array<float, 3> position);
    void cloth_step(ClothBox &cloth, float dt, const ClothParamsDesc &params, const rust::Vec<ClothSphereDesc> &spheres, const rust::Vec<ClothCapsuleDesc> &capsules,
                    const rust::Vec<ClothPlaneDesc> &planes);
    rust::Vec<float> cloth_normals(const ClothBox &cloth);

    SwingTwistDesc decompose_swing_twist(std::array<float, 4> rotation, std::array<float, 3> axis);
    float twist_angle(std::array<float, 4> rotation, std::array<float, 3> axis);
    bool within_limits(std::array<float, 4> rotation, const SwingTwistLimitsDesc &limits);
    std::array<float, 4> clamp_to_limits(std::array<float, 4> rotation, const SwingTwistLimitsDesc &limits);
    std::array<float, 3> pd_drive(std::array<float, 4> current, std::array<float, 4> target, std::array<float, 3> angular_velocity, float stiffness, float damping);

} // namespace sturdy_rs::physics
