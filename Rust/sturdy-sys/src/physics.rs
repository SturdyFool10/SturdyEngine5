//! Raw `cxx` bindings for the engine's solver-agnostic physics package (`Physics/*`): the fixed-step accumulator, collision layers, the analytic
//! collision world with its character controller, contact-event filtering, surface materials with ballistics, XPBD cloth, and swing/twist joint
//! maths. Everything here is CPU-side and independent of any `Engine`.
//!
//! Vectors are `[x, y, z]`, quaternions `[x, y, z, w]`, in metres and a right-handed world. The safe wrappers live in `sturdy::physics`.

#[cxx::bridge(namespace = "sturdy_rs::physics")]
pub mod ffi {
    // ---- plain data ----------------------------------------------------------------------------

    #[derive(Debug, Clone, Copy, PartialEq)]
    struct PoseDesc {
        position: [f32; 3],
        rotation: [f32; 4],
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    struct BoxDesc {
        min: [f32; 3],
        max: [f32; 3],
        object: u64,
        /// Reported to a character so it can ride the box.
        velocity: [f32; 3],
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    struct PlaneDesc {
        normal: [f32; 3],
        offset: f32,
        object: u64,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Default)]
    struct SweepHitDesc {
        fraction: f32,
        normal: [f32; 3],
        point: [f32; 3],
        object: u64,
        object_velocity: [f32; 3],
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    struct CharacterSettingsDesc {
        radius: f32,
        standing_height: f32,
        crouching_height: f32,
        up: [f32; 3],
        gravity: f32,
        slope_limit_degrees: f32,
        step_height: f32,
        snap_distance: f32,
        skin_width: f32,
        max_slides: u32,
        jump_speed: f32,
        terminal_speed: f32,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Default)]
    struct MoveResultDesc {
        displacement: [f32; 3],
        grounded: bool,
        hit_ceiling: bool,
        stepped_up: bool,
        jumped: bool,
        landed: bool,
        landing_speed: f32,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Default)]
    struct CharacterStateDesc {
        position: [f32; 3],
        velocity: [f32; 3],
        ground_normal: [f32; 3],
        ground_object: u64,
        height: f32,
        grounded: bool,
        crouching: bool,
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    struct ContactPointDesc {
        body_a: u64,
        body_b: u64,
        point: [f32; 3],
        normal: [f32; 3],
        normal_impulse: f32,
        approach_speed: f32,
        slide_speed: f32,
        /// `0xFFFFFFFF` = none.
        material_a: u32,
        material_b: u32,
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    struct ContactEventDesc {
        contact: ContactPointDesc,
        /// `false` = impact, `true` = scrape.
        scrape: bool,
        intensity: f32,
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    struct ContactConfigDesc {
        min_impulse: f32,
        min_speed: f32,
        pair_cooldown_seconds: f32,
        merge_radius: f32,
        max_events_per_frame: u32,
        full_intensity_impulse: f32,
    }

    #[derive(Debug, Clone, PartialEq)]
    struct SurfaceMaterialDesc {
        name: String,
        penetration_resistance: f32,
        ricochet_angle_degrees: f32,
        ricochet_energy_retained: f32,
        ricochet_scatter: f32,
        exit_deflection: f32,
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    struct ProjectileDesc {
        position: [f32; 3],
        direction: [f32; 3],
        energy: f32,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Default)]
    struct ImpactResultDesc {
        /// 0 = stopped, 1 = penetrated, 2 = ricocheted.
        outcome: u8,
        point: [f32; 3],
        direction: [f32; 3],
        remaining_energy: f32,
        depth: f32,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Default)]
    struct SurfaceHitDesc {
        point: [f32; 3],
        normal: [f32; 3],
        distance: f32,
        thickness: f32,
        material: u32,
        object: u64,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Default)]
    struct ShotImpactDesc {
        hit: SurfaceHitDesc,
        result: ImpactResultDesc,
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    struct ShotOptionsDesc {
        max_distance: f32,
        max_impacts: u32,
        minimum_energy: f32,
        seed: u64,
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    struct ClothParamsDesc {
        gravity: [f32; 3],
        stretch_compliance: f32,
        bend_compliance: f32,
        damping: f32,
        substeps: u32,
        collision_thickness: f32,
        friction: f32,
        wind: [f32; 3],
        drag: f32,
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    struct ClothSphereDesc {
        center: [f32; 3],
        radius: f32,
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    struct ClothCapsuleDesc {
        a: [f32; 3],
        b: [f32; 3],
        radius: f32,
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    struct ClothPlaneDesc {
        normal: [f32; 3],
        offset: f32,
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    struct SwingTwistLimitsDesc {
        axis: [f32; 3],
        swing_y: f32,
        swing_z: f32,
        twist_min: f32,
        twist_max: f32,
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    struct SwingTwistDesc {
        swing: [f32; 4],
        twist: [f32; 4],
    }

    // ---- ray query callback (Rust side) ------------------------------------------------------------

    extern "Rust" {
        /// A boxed ray query for `trace_shot`: reports the nearest surface along a segment.
        type RayQuery;
        fn call(self: &mut RayQuery, origin: [f32; 3], direction: [f32; 3], max_distance: f32, hit: &mut SurfaceHitDesc) -> bool;
    }

    // ---- C++ side ---------------------------------------------------------------------------------

    unsafe extern "C++" {
        include!("sturdy_rs/physics.hpp");

        /// `SFT::Physics::FixedStepper`.
        type StepperBox;
        /// `SFT::Physics::LayerTable`.
        type LayersBox;
        /// `SFT::Physics::SimpleWorld`.
        type WorldBox;
        /// `SFT::Physics::CharacterController`.
        type CharacterBox;
        /// `SFT::Physics::ContactEventFilter`.
        type ContactFilterBox;
        /// `SFT::Physics::SurfaceTable`.
        type SurfacesBox;
        /// `SFT::Physics::Cloth`.
        type ClothBox;

        // -- fixed stepper --
        fn stepper_new(step_seconds: f64, max_steps_per_frame: u32) -> UniquePtr<StepperBox>;
        fn stepper_advance(stepper: Pin<&mut StepperBox>, delta_seconds: f64) -> u32;
        fn stepper_alpha(stepper: &StepperBox) -> f64;
        fn stepper_step(stepper: &StepperBox) -> f64;
        fn stepper_total_steps(stepper: &StepperBox) -> u64;
        fn stepper_simulated_time(stepper: &StepperBox) -> f64;
        fn stepper_dropped_seconds(stepper: &StepperBox) -> f64;
        fn stepper_set_step(stepper: Pin<&mut StepperBox>, step_seconds: f64);
        fn stepper_reset(stepper: Pin<&mut StepperBox>);
        fn interpolate_pose(previous: &PoseDesc, current: &PoseDesc, alpha: f32) -> PoseDesc;

        // -- layers --
        fn layers_new() -> UniquePtr<LayersBox>;
        /// The layer's index; `found` is false when all 64 are taken.
        fn layers_add(layers: Pin<&mut LayersBox>, name: &str, found: &mut bool) -> u32;
        fn layers_find(layers: &LayersBox, name: &str, found: &mut bool) -> u32;
        fn layers_name(layers: &LayersBox, index: u32) -> String;
        fn layers_count(layers: &LayersBox) -> u32;
        fn layers_mask_of(layers: &LayersBox, names: &Vec<String>) -> u64;
        fn layers_all(layers: &LayersBox) -> u64;
        fn layers_set_collides(layers: Pin<&mut LayersBox>, a: u32, b: u32, collides: bool);
        fn layers_collides(layers: &LayersBox, a: u32, b: u32) -> bool;
        fn layers_collision_mask(layers: &LayersBox, layer: u32) -> u64;

        // -- simple world + character --
        fn world_new() -> UniquePtr<WorldBox>;
        fn world_add_box(world: Pin<&mut WorldBox>, desc: &BoxDesc) -> u32;
        fn world_add_plane(world: Pin<&mut WorldBox>, desc: &PlaneDesc);
        fn world_box_count(world: &WorldBox) -> u32;
        fn world_move_box(world: Pin<&mut WorldBox>, index: u32, delta: [f32; 3]);
        fn world_overlaps_capsule(world: &WorldBox, feet: [f32; 3], radius: f32, height: f32) -> bool;
        fn world_sweep_capsule(world: &WorldBox, feet: [f32; 3], radius: f32, height: f32, delta: [f32; 3], hit: &mut SweepHitDesc) -> bool;
        fn character_settings_default() -> CharacterSettingsDesc;
        fn character_new(settings: &CharacterSettingsDesc, feet: [f32; 3]) -> UniquePtr<CharacterBox>;
        fn character_move(character: Pin<&mut CharacterBox>, world: &WorldBox, desired_velocity: [f32; 3], dt: f32, jump: bool) -> MoveResultDesc;
        fn character_set_crouching(character: Pin<&mut CharacterBox>, world: &WorldBox, crouching: bool) -> bool;
        fn character_teleport(character: Pin<&mut CharacterBox>, feet: [f32; 3]);
        fn character_add_impulse(character: Pin<&mut CharacterBox>, velocity_change: [f32; 3]);
        fn character_state(character: &CharacterBox) -> CharacterStateDesc;

        // -- contact events --
        fn contact_config_default() -> ContactConfigDesc;
        fn contact_filter_new(config: &ContactConfigDesc) -> UniquePtr<ContactFilterBox>;
        fn contact_filter_set_config(filter: Pin<&mut ContactFilterBox>, config: &ContactConfigDesc);
        fn contact_filter_begin_frame(filter: Pin<&mut ContactFilterBox>, time_seconds: f64);
        fn contact_filter_submit(filter: Pin<&mut ContactFilterBox>, contact: &ContactPointDesc) -> bool;
        fn contact_filter_events(filter: &ContactFilterBox) -> Vec<ContactEventDesc>;

        // -- surfaces + ballistics --
        fn surface_material_default() -> SurfaceMaterialDesc;
        fn surfaces_new() -> UniquePtr<SurfacesBox>;
        fn surfaces_add(surfaces: Pin<&mut SurfacesBox>, material: &SurfaceMaterialDesc) -> u32;
        fn surfaces_find(surfaces: &SurfacesBox, name: &str, found: &mut bool) -> u32;
        fn surfaces_count(surfaces: &SurfacesBox) -> u32;
        fn surfaces_get(surfaces: &SurfacesBox, id: u32, out: &mut SurfaceMaterialDesc) -> bool;
        fn resolve_impact(
            projectile: &ProjectileDesc,
            material: &SurfaceMaterialDesc,
            entry_point: [f32; 3],
            normal: [f32; 3],
            thickness: f32,
            random: [f32; 2],
        ) -> ImpactResultDesc;
        fn shot_options_default() -> ShotOptionsDesc;
        fn trace_shot(surfaces: &SurfacesBox, projectile: &ProjectileDesc, options: &ShotOptionsDesc, query: &mut RayQuery) -> Vec<ShotImpactDesc>;

        // -- cloth --
        fn cloth_params_default() -> ClothParamsDesc;
        fn cloth_grid(columns: u32, rows: u32, spacing: f32, origin: [f32; 3], right: [f32; 3], down: [f32; 3], total_mass: f32) -> UniquePtr<ClothBox>;
        /// `positions` is flattened `x, y, z` triples.
        fn cloth_from_triangles(positions: &[f32], indices: &[u32], total_mass: f32) -> UniquePtr<ClothBox>;
        fn cloth_particle_count(cloth: &ClothBox) -> u32;
        fn cloth_constraint_counts(cloth: &ClothBox) -> [u32; 2];
        fn cloth_positions(cloth: &ClothBox) -> Vec<f32>;
        fn cloth_indices(cloth: &ClothBox) -> Vec<u32>;
        fn cloth_pin(cloth: Pin<&mut ClothBox>, particle: u32, pinned: bool);
        fn cloth_is_pinned(cloth: &ClothBox, particle: u32) -> bool;
        fn cloth_move_pinned(cloth: Pin<&mut ClothBox>, particle: u32, position: [f32; 3]);
        fn cloth_step(
            cloth: Pin<&mut ClothBox>,
            dt: f32,
            params: &ClothParamsDesc,
            spheres: &Vec<ClothSphereDesc>,
            capsules: &Vec<ClothCapsuleDesc>,
            planes: &Vec<ClothPlaneDesc>,
        );
        fn cloth_normals(cloth: &ClothBox) -> Vec<f32>;

        // -- swing / twist --
        fn decompose_swing_twist(rotation: [f32; 4], axis: [f32; 3]) -> SwingTwistDesc;
        fn twist_angle(rotation: [f32; 4], axis: [f32; 3]) -> f32;
        fn within_limits(rotation: [f32; 4], limits: &SwingTwistLimitsDesc) -> bool;
        fn clamp_to_limits(rotation: [f32; 4], limits: &SwingTwistLimitsDesc) -> [f32; 4];
        fn pd_drive(current: [f32; 4], target: [f32; 4], angular_velocity: [f32; 3], stiffness: f32, damping: f32) -> [f32; 3];
    }
}

/// A boxed ray query handed to `trace_shot`.
pub struct RayQuery(pub Box<dyn FnMut([f32; 3], [f32; 3], f32) -> Option<ffi::SurfaceHitDesc>>);

impl RayQuery {
    fn call(&mut self, origin: [f32; 3], direction: [f32; 3], max_distance: f32, hit: &mut ffi::SurfaceHitDesc) -> bool {
        match (self.0)(origin, direction, max_distance) {
            Some(found) => {
                *hit = found;
                true
            }
            None => false,
        }
    }
}

// SAFETY: each physics object is plain CPU state with no thread affinity; the safe wrappers take `&mut self` for anything that mutates, so Rust's
// aliasing rules already forbid concurrent use of one object.
unsafe impl Send for ffi::StepperBox {}
unsafe impl Send for ffi::LayersBox {}
unsafe impl Send for ffi::WorldBox {}
unsafe impl Send for ffi::CharacterBox {}
unsafe impl Send for ffi::ContactFilterBox {}
unsafe impl Send for ffi::SurfacesBox {}
unsafe impl Send for ffi::ClothBox {}
