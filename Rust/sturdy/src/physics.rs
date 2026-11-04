//! Solver-agnostic physics building blocks (`Physics/*`): a fixed-timestep accumulator, collision layers, an analytic collision world with a
//! kinematic character controller, contact-event filtering, surface materials with penetration/ricochet ballistics, XPBD cloth, and
//! swing/twist joint maths. All CPU-side and independent of [`Engine`](crate::Engine): use them with any solver, or none.
//!
//! ```no_run
//! use sturdy::physics::{CharacterController, CharacterSettings, FixedStepper, Plane, World};
//! use sturdy::glam::Vec3;
//!
//! let mut world = World::new();
//! world.add_plane(Plane { normal: Vec3::Y, offset: 0.0, object: 1 });
//! let mut player = CharacterController::new(&CharacterSettings::default(), Vec3::new(0.0, 2.0, 0.0));
//! let mut clock = FixedStepper::new(1.0 / 60.0, 8);
//! for _ in 0..clock.advance(1.0 / 30.0) {
//!     player.step(&world, Vec3::new(1.0, 0.0, 0.0), clock.step() as f32, false);
//! }
//! ```

use cxx::UniquePtr;
use glam::{Quat, Vec3};
use sturdy_sys::physics::{ffi, RayQuery};

fn v(a: [f32; 3]) -> Vec3 {
    Vec3::from_array(a)
}

fn q(a: [f32; 4]) -> Quat {
    Quat::from_xyzw(a[0], a[1], a[2], a[3])
}

fn qa(q: Quat) -> [f32; 4] {
    q.to_array()
}

// ---- fixed stepper ------------------------------------------------------------------------------------

/// Fixed-timestep accumulator ("Fix Your Timestep"): feed it the frame's variable delta, run the simulation [`advance`](Self::advance) times at
/// [`step`](Self::step) seconds each, then render at [`alpha`](Self::alpha) between the last two states.
pub struct FixedStepper(UniquePtr<ffi::StepperBox>);

impl FixedStepper {
    /// `step_seconds` <= 0 selects 1/60; `max_steps_per_frame` 0 selects 8.
    pub fn new(step_seconds: f64, max_steps_per_frame: u32) -> Self {
        Self(ffi::stepper_new(step_seconds, max_steps_per_frame))
    }

    /// Adds `delta_seconds` and returns how many fixed steps are now due. Time beyond the per-frame cap is dropped, so one long frame (a
    /// breakpoint, a hitch) cannot trigger a spiral of death. Negative or non-finite time counts as zero.
    pub fn advance(&mut self, delta_seconds: f64) -> u32 {
        ffi::stepper_advance(self.0.pin_mut(), delta_seconds)
    }

    /// Fraction of a step the accumulator holds after [`advance`](Self::advance), in `[0, 1)`: the interpolation factor.
    pub fn alpha(&self) -> f64 {
        ffi::stepper_alpha(&self.0)
    }

    pub fn step(&self) -> f64 {
        ffi::stepper_step(&self.0)
    }

    pub fn total_steps(&self) -> u64 {
        ffi::stepper_total_steps(&self.0)
    }

    /// Simulated seconds so far (`total_steps * step`).
    pub fn simulated_time(&self) -> f64 {
        ffi::stepper_simulated_time(&self.0)
    }

    /// Seconds discarded by the spiral-of-death clamp since the last [`reset`](Self::reset).
    pub fn dropped_seconds(&self) -> f64 {
        ffi::stepper_dropped_seconds(&self.0)
    }

    /// Ignored unless positive and finite.
    pub fn set_step(&mut self, step_seconds: f64) {
        ffi::stepper_set_step(self.0.pin_mut(), step_seconds);
    }

    pub fn reset(&mut self) {
        ffi::stepper_reset(self.0.pin_mut());
    }
}

/// Position and orientation of a body at one fixed step, for render interpolation.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct BodyPose {
    pub position: Vec3,
    pub rotation: Quat,
}

impl BodyPose {
    /// The pose to draw: `previous` and `current` are the last two simulation states, `alpha` comes from [`FixedStepper::alpha`]. Rotation takes
    /// the short way round.
    pub fn interpolate(previous: &BodyPose, current: &BodyPose, alpha: f32) -> BodyPose {
        let to_desc = |p: &BodyPose| ffi::PoseDesc { position: p.position.to_array(), rotation: qa(p.rotation) };
        let out = ffi::interpolate_pose(&to_desc(previous), &to_desc(current), alpha);
        BodyPose { position: v(out.position), rotation: q(out.rotation) }
    }
}

// ---- layers -------------------------------------------------------------------------------------------

/// A bit mask of collision layers (one bit per layer, up to 64).
pub type LayerMask = u64;

/// Named collision layers and a symmetric matrix of which collide with which; queries and bodies carry a layer index plus the mask the matrix
/// derives for it.
pub struct LayerTable(UniquePtr<ffi::LayersBox>);

impl Default for LayerTable {
    fn default() -> Self {
        Self::new()
    }
}

impl LayerTable {
    pub fn new() -> Self {
        Self(ffi::layers_new())
    }

    /// Registers `name` (idempotent) and returns its index, or `None` when all 64 are taken. A new layer collides with every layer.
    pub fn add(&mut self, name: &str) -> Option<u32> {
        let mut found = false;
        let index = ffi::layers_add(self.0.pin_mut(), name, &mut found);
        found.then_some(index)
    }

    pub fn find(&self, name: &str) -> Option<u32> {
        let mut found = false;
        let index = ffi::layers_find(&self.0, name, &mut found);
        found.then_some(index)
    }

    /// Empty for an unregistered layer.
    pub fn name(&self, layer: u32) -> String {
        ffi::layers_name(&self.0, layer)
    }

    pub fn count(&self) -> u32 {
        ffi::layers_count(&self.0)
    }

    /// The bit of one layer (`1 << layer`; 0 past 63).
    pub fn bit(layer: u32) -> LayerMask {
        if layer < 64 {
            1u64 << layer
        } else {
            0
        }
    }

    /// Union of the named layers' bits (unknown names are ignored).
    pub fn mask_of<S: AsRef<str>>(&self, names: &[S]) -> LayerMask {
        let list: Vec<String> = names.iter().map(|n| n.as_ref().to_owned()).collect();
        ffi::layers_mask_of(&self.0, &list)
    }

    /// Every registered layer.
    pub fn all(&self) -> LayerMask {
        ffi::layers_all(&self.0)
    }

    /// Symmetric: `set_collides(a, b, false)` also stops `b` from colliding with `a`. Unregistered indices are ignored.
    pub fn set_collides(&mut self, a: u32, b: u32, collides: bool) {
        ffi::layers_set_collides(self.0.pin_mut(), a, b, collides);
    }

    pub fn collides(&self, a: u32, b: u32) -> bool {
        ffi::layers_collides(&self.0, a, b)
    }

    /// The layers `layer` collides with, ready to hand to a solver as its filter mask (0 for an unregistered layer).
    pub fn collision_mask(&self, layer: u32) -> LayerMask {
        ffi::layers_collision_mask(&self.0, layer)
    }
}

// ---- collision world + character ------------------------------------------------------------------------

/// An axis-aligned box in a [`World`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Box3 {
    pub min: Vec3,
    pub max: Vec3,
    /// Whatever the box stands for (entity, body); reported back as [`SweepHit::object`] so platforms can carry the character.
    pub object: u64,
    /// Reported to a character so it can ride the box.
    pub velocity: Vec3,
}

impl Default for Box3 {
    fn default() -> Self {
        Self { min: Vec3::ZERO, max: Vec3::ONE, object: 0, velocity: Vec3::ZERO }
    }
}

/// A plane through `normal * offset` facing `normal`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Plane {
    pub normal: Vec3,
    pub offset: f32,
    pub object: u64,
}

/// A capsule sweep that hit something.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SweepHit {
    /// Fraction of the requested movement completed before contact, in `[0, 1]`.
    pub fraction: f32,
    pub normal: Vec3,
    pub point: Vec3,
    pub object: u64,
    /// Velocity of that object at the contact (zero for static geometry).
    pub object_velocity: Vec3,
}

/// A minimal analytic collision world (boxes and planes) for tools, tests and quick prototypes without a physics solver. Capsules point along +Y.
pub struct World(UniquePtr<ffi::WorldBox>);

impl Default for World {
    fn default() -> Self {
        Self::new()
    }
}

impl World {
    pub fn new() -> Self {
        Self(ffi::world_new())
    }

    /// Adds a box and returns its index for [`move_box`](Self::move_box).
    pub fn add_box(&mut self, b: &Box3) -> u32 {
        ffi::world_add_box(
            self.0.pin_mut(),
            &ffi::BoxDesc { min: b.min.to_array(), max: b.max.to_array(), object: b.object, velocity: b.velocity.to_array() },
        )
    }

    pub fn add_plane(&mut self, plane: &Plane) {
        ffi::world_add_plane(self.0.pin_mut(), &ffi::PlaneDesc { normal: plane.normal.to_array(), offset: plane.offset, object: plane.object });
    }

    pub fn box_count(&self) -> u32 {
        ffi::world_box_count(&self.0)
    }

    /// Moves a box (a platform) by `delta`; an unknown index is ignored.
    pub fn move_box(&mut self, index: u32, delta: Vec3) {
        ffi::world_move_box(self.0.pin_mut(), index, delta.to_array());
    }

    /// True when a vertical capsule whose feet are at `feet` overlaps solid geometry.
    pub fn overlaps_capsule(&self, feet: Vec3, radius: f32, height: f32) -> bool {
        ffi::world_overlaps_capsule(&self.0, feet.to_array(), radius, height)
    }

    /// Sweeps a vertical capsule by `delta`; `None` when nothing is hit.
    pub fn sweep_capsule(&self, feet: Vec3, radius: f32, height: f32, delta: Vec3) -> Option<SweepHit> {
        let mut hit = ffi::SweepHitDesc::default();
        ffi::world_sweep_capsule(&self.0, feet.to_array(), radius, height, delta.to_array(), &mut hit).then(|| SweepHit {
            fraction: hit.fraction,
            normal: v(hit.normal),
            point: v(hit.point),
            object: hit.object,
            object_velocity: v(hit.object_velocity),
        })
    }
}

/// Tuning of a [`CharacterController`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CharacterSettings {
    pub radius: f32,
    pub standing_height: f32,
    pub crouching_height: f32,
    pub up: Vec3,
    /// Gravity magnitude along `-up`.
    pub gravity: f32,
    /// Steepest walkable slope, degrees from horizontal.
    pub slope_limit_degrees: f32,
    /// Tallest ledge walked up without jumping.
    pub step_height: f32,
    /// How far to follow the ground downward so walking down ramps and stairs keeps contact.
    pub snap_distance: f32,
    /// Gap kept between the capsule and surfaces.
    pub skin_width: f32,
    pub max_slides: u32,
    pub jump_speed: f32,
    /// Speed beyond which falling is clamped.
    pub terminal_speed: f32,
}

impl Default for CharacterSettings {
    fn default() -> Self {
        let d = ffi::character_settings_default();
        Self {
            radius: d.radius,
            standing_height: d.standing_height,
            crouching_height: d.crouching_height,
            up: v(d.up),
            gravity: d.gravity,
            slope_limit_degrees: d.slope_limit_degrees,
            step_height: d.step_height,
            snap_distance: d.snap_distance,
            skin_width: d.skin_width,
            max_slides: d.max_slides,
            jump_speed: d.jump_speed,
            terminal_speed: d.terminal_speed,
        }
    }
}

/// What one [`CharacterController::step`] did.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct MoveResult {
    /// Position change this call (feet), including platform carry.
    pub displacement: Vec3,
    pub grounded: bool,
    pub hit_ceiling: bool,
    pub stepped_up: bool,
    pub jumped: bool,
    /// Fell onto the ground this call; `landing_speed` is the speed along `-up` just before contact.
    pub landed: bool,
    pub landing_speed: f32,
}

/// The controller's current state.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CharacterState {
    pub position: Vec3,
    pub velocity: Vec3,
    pub ground_normal: Vec3,
    pub ground_object: u64,
    pub height: f32,
    pub grounded: bool,
    pub crouching: bool,
}

/// Kinematic character mover (collide-and-slide): slides along walls and creases, refuses slopes over the limit, steps up ledges, snaps to the
/// ground on ramps and stairs, rides moving platforms, and checks headroom before standing up from a crouch.
pub struct CharacterController(UniquePtr<ffi::CharacterBox>);

impl CharacterController {
    pub fn new(settings: &CharacterSettings, feet: Vec3) -> Self {
        let desc = ffi::CharacterSettingsDesc {
            radius: settings.radius,
            standing_height: settings.standing_height,
            crouching_height: settings.crouching_height,
            up: settings.up.to_array(),
            gravity: settings.gravity,
            slope_limit_degrees: settings.slope_limit_degrees,
            step_height: settings.step_height,
            snap_distance: settings.snap_distance,
            skin_width: settings.skin_width,
            max_slides: settings.max_slides,
            jump_speed: settings.jump_speed,
            terminal_speed: settings.terminal_speed,
        };
        Self(ffi::character_new(&desc, feet.to_array()))
    }

    /// Moves for `dt` seconds toward `desired_velocity` (horizontal, world space). `jump` launches when grounded.
    pub fn step(&mut self, world: &World, desired_velocity: Vec3, dt: f32, jump: bool) -> MoveResult {
        let r = ffi::character_move(self.0.pin_mut(), &world.0, desired_velocity.to_array(), dt, jump);
        MoveResult {
            displacement: v(r.displacement),
            grounded: r.grounded,
            hit_ceiling: r.hit_ceiling,
            stepped_up: r.stepped_up,
            jumped: r.jumped,
            landed: r.landed,
            landing_speed: r.landing_speed,
        }
    }

    /// Crouches at once; standing up succeeds only with headroom. Returns the resulting crouch state.
    pub fn set_crouching(&mut self, world: &World, crouching: bool) -> bool {
        ffi::character_set_crouching(self.0.pin_mut(), &world.0, crouching)
    }

    pub fn teleport(&mut self, feet: Vec3) {
        ffi::character_teleport(self.0.pin_mut(), feet.to_array());
    }

    pub fn add_impulse(&mut self, velocity_change: Vec3) {
        ffi::character_add_impulse(self.0.pin_mut(), velocity_change.to_array());
    }

    pub fn state(&self) -> CharacterState {
        let s = ffi::character_state(&self.0);
        CharacterState {
            position: v(s.position),
            velocity: v(s.velocity),
            ground_normal: v(s.ground_normal),
            ground_object: s.ground_object,
            height: s.height,
            grounded: s.grounded,
            crouching: s.crouching,
        }
    }
}

// ---- contact events -----------------------------------------------------------------------------------

/// "No material" in a [`ContactPoint`].
pub const NO_MATERIAL: u32 = 0xFFFF_FFFF;

/// A raw contact reported by the solver after a step.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ContactPoint {
    pub body_a: u64,
    pub body_b: u64,
    pub point: Vec3,
    pub normal: Vec3,
    /// Impulse along the normal (N*s).
    pub normal_impulse: f32,
    /// Closing speed along the normal, and relative speed along the surface (m/s).
    pub approach_speed: f32,
    pub slide_speed: f32,
    /// Ids in a [`SurfaceTable`], or [`NO_MATERIAL`].
    pub material_a: u32,
    pub material_b: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ContactKind {
    Impact,
    Scrape,
}

/// A contact worth reacting to: what audio, decals and particle systems consume.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ContactEvent {
    pub contact: ContactPoint,
    pub kind: ContactKind,
    /// 0..1, a perceptual mapping of the impulse (log scale) for volume and effect intensity.
    pub intensity: f32,
}

/// Thresholds of a [`ContactEventFilter`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ContactEventConfig {
    /// Contacts weaker than both `min_impulse` and `min_speed` are dropped.
    pub min_impulse: f32,
    pub min_speed: f32,
    /// A body pair cannot raise another event for this long after one.
    pub pair_cooldown_seconds: f32,
    /// Events of the same pair closer than this within a frame merge into the stronger one.
    pub merge_radius: f32,
    /// Hard cap per frame.
    pub max_events_per_frame: u32,
    /// Impulse that maps to intensity 1.
    pub full_intensity_impulse: f32,
}

impl Default for ContactEventConfig {
    fn default() -> Self {
        let d = ffi::contact_config_default();
        Self {
            min_impulse: d.min_impulse,
            min_speed: d.min_speed,
            pair_cooldown_seconds: d.pair_cooldown_seconds,
            merge_radius: d.merge_radius,
            max_events_per_frame: d.max_events_per_frame,
            full_intensity_impulse: d.full_intensity_impulse,
        }
    }
}

impl ContactEventConfig {
    fn to_ffi(self) -> ffi::ContactConfigDesc {
        ffi::ContactConfigDesc {
            min_impulse: self.min_impulse,
            min_speed: self.min_speed,
            pair_cooldown_seconds: self.pair_cooldown_seconds,
            merge_radius: self.merge_radius,
            max_events_per_frame: self.max_events_per_frame,
            full_intensity_impulse: self.full_intensity_impulse,
        }
    }
}

/// Turns the solver's contact stream into a sparse, budgeted event list: thresholds, per-pair cooldowns, merging of near-duplicate contacts and
/// a per-frame cap, so a rolling crate does not spam the audio mixer.
pub struct ContactEventFilter(UniquePtr<ffi::ContactFilterBox>);

impl ContactEventFilter {
    pub fn new(config: &ContactEventConfig) -> Self {
        Self(ffi::contact_filter_new(&config.to_ffi()))
    }

    pub fn set_config(&mut self, config: &ContactEventConfig) {
        ffi::contact_filter_set_config(self.0.pin_mut(), &config.to_ffi());
    }

    /// Clears last frame's events and sets the clock used for cooldowns.
    pub fn begin_frame(&mut self, time_seconds: f64) {
        ffi::contact_filter_begin_frame(self.0.pin_mut(), time_seconds);
    }

    /// Offers a contact; true if it became (or replaced) an event.
    pub fn submit(&mut self, contact: &ContactPoint) -> bool {
        ffi::contact_filter_submit(
            self.0.pin_mut(),
            &ffi::ContactPointDesc {
                body_a: contact.body_a,
                body_b: contact.body_b,
                point: contact.point.to_array(),
                normal: contact.normal.to_array(),
                normal_impulse: contact.normal_impulse,
                approach_speed: contact.approach_speed,
                slide_speed: contact.slide_speed,
                material_a: contact.material_a,
                material_b: contact.material_b,
            },
        )
    }

    /// This frame's events.
    pub fn events(&self) -> Vec<ContactEvent> {
        ffi::contact_filter_events(&self.0)
            .into_iter()
            .map(|e| ContactEvent {
                contact: ContactPoint {
                    body_a: e.contact.body_a,
                    body_b: e.contact.body_b,
                    point: v(e.contact.point),
                    normal: v(e.contact.normal),
                    normal_impulse: e.contact.normal_impulse,
                    approach_speed: e.contact.approach_speed,
                    slide_speed: e.contact.slide_speed,
                    material_a: e.contact.material_a,
                    material_b: e.contact.material_b,
                },
                kind: if e.scrape { ContactKind::Scrape } else { ContactKind::Impact },
                intensity: e.intensity,
            })
            .collect()
    }
}

// ---- surfaces + ballistics -----------------------------------------------------------------------------

/// How a material treats a projectile.
#[derive(Debug, Clone, PartialEq)]
pub struct SurfaceMaterial {
    pub name: String,
    /// Energy a projectile spends per metre travelled through the material, in joules per metre (drywall ~400, timber ~2500, concrete ~25000,
    /// steel plate ~90000 for a rifle round).
    pub penetration_resistance: f32,
    /// Impact angles measured from the surface (0 = skimming, 90 = square on) below which the projectile skips off instead of entering.
    pub ricochet_angle_degrees: f32,
    /// Fraction of energy kept by a ricochet.
    pub ricochet_energy_retained: f32,
    /// How far a ricochet direction scatters, in radians of cone half-angle at the grazing limit.
    pub ricochet_scatter: f32,
    /// How far a projectile bends coming out the other side, in radians per unit of (depth / max depth).
    pub exit_deflection: f32,
}

impl Default for SurfaceMaterial {
    fn default() -> Self {
        let d = ffi::surface_material_default();
        Self::from_ffi(d)
    }
}

impl SurfaceMaterial {
    fn from_ffi(d: ffi::SurfaceMaterialDesc) -> Self {
        Self {
            name: d.name,
            penetration_resistance: d.penetration_resistance,
            ricochet_angle_degrees: d.ricochet_angle_degrees,
            ricochet_energy_retained: d.ricochet_energy_retained,
            ricochet_scatter: d.ricochet_scatter,
            exit_deflection: d.exit_deflection,
        }
    }

    fn to_ffi(&self) -> ffi::SurfaceMaterialDesc {
        ffi::SurfaceMaterialDesc {
            name: self.name.clone(),
            penetration_resistance: self.penetration_resistance,
            ricochet_angle_degrees: self.ricochet_angle_degrees,
            ricochet_energy_retained: self.ricochet_energy_retained,
            ricochet_scatter: self.ricochet_scatter,
            exit_deflection: self.exit_deflection,
        }
    }
}

/// Indexed material table shared by gameplay, audio and VFX (impact sounds and decals key off the same ids).
pub struct SurfaceTable(UniquePtr<ffi::SurfacesBox>);

impl Default for SurfaceTable {
    fn default() -> Self {
        Self::new()
    }
}

impl SurfaceTable {
    pub fn new() -> Self {
        Self(ffi::surfaces_new())
    }

    /// Appends a material and returns its id.
    pub fn add(&mut self, material: &SurfaceMaterial) -> u32 {
        ffi::surfaces_add(self.0.pin_mut(), &material.to_ffi())
    }

    pub fn find(&self, name: &str) -> Option<u32> {
        let mut found = false;
        let id = ffi::surfaces_find(&self.0, name, &mut found);
        found.then_some(id)
    }

    pub fn len(&self) -> u32 {
        ffi::surfaces_count(&self.0)
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn get(&self, id: u32) -> Option<SurfaceMaterial> {
        let mut out = ffi::surface_material_default();
        ffi::surfaces_get(&self.0, id, &mut out).then(|| SurfaceMaterial::from_ffi(out))
    }
}

/// A projectile in flight.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Projectile {
    pub position: Vec3,
    /// Travel direction (normalised for you).
    pub direction: Vec3,
    /// Kinetic energy in joules.
    pub energy: f32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ImpactOutcome {
    /// Absorbed inside the material.
    Stopped,
    /// Came out the far side.
    Penetrated,
    /// Skipped off the entry surface.
    Ricocheted,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ImpactResult {
    pub outcome: ImpactOutcome,
    /// Where the projectile stopped, left the material, or bounced.
    pub point: Vec3,
    /// Travel direction afterwards (meaningless for `Stopped`).
    pub direction: Vec3,
    pub remaining_energy: f32,
    /// Distance travelled inside the material.
    pub depth: f32,
}

impl ImpactResult {
    fn from_ffi(r: ffi::ImpactResultDesc) -> Self {
        Self {
            outcome: match r.outcome {
                1 => ImpactOutcome::Penetrated,
                2 => ImpactOutcome::Ricocheted,
                _ => ImpactOutcome::Stopped,
            },
            point: v(r.point),
            direction: v(r.direction),
            remaining_energy: r.remaining_energy,
            depth: r.depth,
        }
    }
}

/// Resolves one entry into one surface. `normal` faces the projectile, `thickness` is the distance the straight ray spends inside before the exit
/// point (very large for terrain). `random` is two uniform values in `[0, 1)` used for scatter; pass fixed values for deterministic replays.
pub fn resolve_impact(projectile: &Projectile, material: &SurfaceMaterial, entry_point: Vec3, normal: Vec3, thickness: f32, random: [f32; 2]) -> ImpactResult {
    ImpactResult::from_ffi(ffi::resolve_impact(
        &ffi::ProjectileDesc { position: projectile.position.to_array(), direction: projectile.direction.to_array(), energy: projectile.energy },
        &material.to_ffi(),
        entry_point.to_array(),
        normal.to_array(),
        thickness,
        random,
    ))
}

/// What a ray query reports for the nearest surface along a segment.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SurfaceHit {
    pub point: Vec3,
    pub normal: Vec3,
    /// Distance along the ray, in metres, from its origin.
    pub distance: f32,
    /// Distance the straight ray stays inside the object after this point (from the matching exit hit).
    pub thickness: f32,
    /// Id in a [`SurfaceTable`], or [`NO_MATERIAL`].
    pub material: u32,
    /// Opaque id of whatever was hit, for damage and events.
    pub object: u64,
}

/// One surface a traced shot went through.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ShotImpact {
    pub hit: SurfaceHit,
    pub result: ImpactResult,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ShotOptions {
    pub max_distance: f32,
    pub max_impacts: u32,
    /// Energy below which the projectile is considered spent.
    pub minimum_energy: f32,
    /// The deterministic stream seed for scatter (a per-shot value shared with the server for replay).
    pub seed: u64,
}

impl Default for ShotOptions {
    fn default() -> Self {
        let d = ffi::shot_options_default();
        Self { max_distance: d.max_distance, max_impacts: d.max_impacts, minimum_energy: d.minimum_energy, seed: d.seed }
    }
}

/// Traces a projectile through the world: ray, impact, continue from the exit (or the ricochet) until it stops, leaves range or runs out of
/// impacts. `query(origin, direction, max_distance)` casts a ray and returns the nearest hit within range; the caller owns layer filtering. Hits
/// on materials missing from `surfaces` stop the projectile.
pub fn trace_shot<F>(surfaces: &SurfaceTable, projectile: &Projectile, options: &ShotOptions, mut query: F) -> Vec<ShotImpact>
where
    F: FnMut(Vec3, Vec3, f32) -> Option<SurfaceHit> + 'static,
{
    let mut boxed = RayQuery(Box::new(move |origin, direction, max_distance| {
        query(v(origin), v(direction), max_distance).map(|h| ffi::SurfaceHitDesc {
            point: h.point.to_array(),
            normal: h.normal.to_array(),
            distance: h.distance,
            thickness: h.thickness,
            material: h.material,
            object: h.object,
        })
    }));
    ffi::trace_shot(
        &surfaces.0,
        &ffi::ProjectileDesc { position: projectile.position.to_array(), direction: projectile.direction.to_array(), energy: projectile.energy },
        &ffi::ShotOptionsDesc {
            max_distance: options.max_distance,
            max_impacts: options.max_impacts,
            minimum_energy: options.minimum_energy,
            seed: options.seed,
        },
        &mut boxed,
    )
    .into_iter()
    .map(|i| ShotImpact {
        hit: SurfaceHit {
            point: v(i.hit.point),
            normal: v(i.hit.normal),
            distance: i.hit.distance,
            thickness: i.hit.thickness,
            material: i.hit.material,
            object: i.hit.object,
        },
        result: ImpactResult::from_ffi(i.result),
    })
    .collect()
}

// ---- cloth ----------------------------------------------------------------------------------------------

/// Cloth solver tuning.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ClothParams {
    pub gravity: Vec3,
    /// XPBD compliance (inverse stiffness, m/N): 0 is rigid; stretch should stay near zero, bending is softer.
    pub stretch_compliance: f32,
    pub bend_compliance: f32,
    /// Fraction of velocity lost per second.
    pub damping: f32,
    /// Each substep is a full predict/solve/update, so more substeps trade cost for stiffness and stability.
    pub substeps: u32,
    /// Radius added around colliders so the surface does not sit inside them.
    pub collision_thickness: f32,
    /// Fraction of tangential motion removed on contact.
    pub friction: f32,
    /// Uniform wind velocity and how strongly the cloth catches it.
    pub wind: Vec3,
    pub drag: f32,
}

impl Default for ClothParams {
    fn default() -> Self {
        let d = ffi::cloth_params_default();
        Self {
            gravity: v(d.gravity),
            stretch_compliance: d.stretch_compliance,
            bend_compliance: d.bend_compliance,
            damping: d.damping,
            substeps: d.substeps,
            collision_thickness: d.collision_thickness,
            friction: d.friction,
            wind: v(d.wind),
            drag: d.drag,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ClothSphere {
    pub center: Vec3,
    pub radius: f32,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ClothCapsule {
    pub a: Vec3,
    pub b: Vec3,
    pub radius: f32,
}

/// Plane through `normal * offset` facing `normal`; particles are kept on the positive side.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ClothPlane {
    pub normal: Vec3,
    pub offset: f32,
}

/// Collision proxies for one cloth step, in the cloth's space.
#[derive(Debug, Clone, Default)]
pub struct ClothColliders {
    pub spheres: Vec<ClothSphere>,
    pub capsules: Vec<ClothCapsule>,
    pub planes: Vec<ClothPlane>,
}

/// Position-based dynamics cloth (XPBD): stretch constraints on every edge, bending as distance constraints between the far vertices of adjacent
/// triangles, substepped Gauss-Seidel, collision against spheres, capsules and planes, optional wind. Particles are pinned to hold them in place.
pub struct Cloth(UniquePtr<ffi::ClothBox>);

impl Cloth {
    /// A `columns x rows` sheet of particles `spacing` apart, starting at `origin` and extending along `right` and `down` (unit vectors). `None` for
    /// a grid smaller than 2x2, a non-positive spacing or mass, or more than ~4M particles.
    pub fn grid(columns: u32, rows: u32, spacing: f32, origin: Vec3, right: Vec3, down: Vec3, total_mass: f32) -> Option<Self> {
        if columns < 2 || rows < 2 || u64::from(columns) * u64::from(rows) > (1 << 22) || !(spacing > 0.0) || !(total_mass > 0.0) {
            return None;
        }
        Some(Self(ffi::cloth_grid(columns, rows, spacing, origin.to_array(), right.to_array(), down.to_array(), total_mass)))
    }

    /// Any triangle mesh; the rest lengths are the edge lengths in `positions`. `None` when `indices` is empty, not a multiple of 3, or refers past
    /// `positions`.
    pub fn from_triangles(positions: &[Vec3], indices: &[u32], total_mass: f32) -> Option<Self> {
        if positions.is_empty()
            || indices.is_empty()
            || indices.len() % 3 != 0
            || indices.iter().any(|&i| i as usize >= positions.len())
            || !(total_mass > 0.0)
        {
            return None;
        }
        let flat: Vec<f32> = positions.iter().flat_map(|p| p.to_array()).collect();
        Some(Self(ffi::cloth_from_triangles(&flat, indices, total_mass)))
    }

    pub fn particle_count(&self) -> u32 {
        ffi::cloth_particle_count(&self.0)
    }

    /// `(stretch, bend)` constraint counts.
    pub fn constraint_counts(&self) -> (u32, u32) {
        let c = ffi::cloth_constraint_counts(&self.0);
        (c[0], c[1])
    }

    pub fn positions(&self) -> Vec<Vec3> {
        ffi::cloth_positions(&self.0).chunks_exact(3).map(|c| Vec3::new(c[0], c[1], c[2])).collect()
    }

    /// Triangle indices into [`positions`](Self::positions).
    pub fn indices(&self) -> Vec<u32> {
        ffi::cloth_indices(&self.0)
    }

    /// Pins (or releases) a particle in place; an unknown particle is ignored.
    pub fn pin(&mut self, particle: u32, pinned: bool) {
        ffi::cloth_pin(self.0.pin_mut(), particle, pinned);
    }

    pub fn is_pinned(&self, particle: u32) -> bool {
        ffi::cloth_is_pinned(&self.0, particle)
    }

    /// Moves a pinned particle (attachment points following an animated body).
    pub fn move_pinned(&mut self, particle: u32, position: Vec3) {
        ffi::cloth_move_pinned(self.0.pin_mut(), particle, position.to_array());
    }

    /// Advances by `dt` seconds.
    pub fn step(&mut self, dt: f32, params: &ClothParams, colliders: &ClothColliders) {
        let p = ffi::ClothParamsDesc {
            gravity: params.gravity.to_array(),
            stretch_compliance: params.stretch_compliance,
            bend_compliance: params.bend_compliance,
            damping: params.damping,
            substeps: params.substeps,
            collision_thickness: params.collision_thickness,
            friction: params.friction,
            wind: params.wind.to_array(),
            drag: params.drag,
        };
        let spheres: Vec<_> = colliders.spheres.iter().map(|s| ffi::ClothSphereDesc { center: s.center.to_array(), radius: s.radius }).collect();
        let capsules: Vec<_> =
            colliders.capsules.iter().map(|c| ffi::ClothCapsuleDesc { a: c.a.to_array(), b: c.b.to_array(), radius: c.radius }).collect();
        let planes: Vec<_> = colliders.planes.iter().map(|pl| ffi::ClothPlaneDesc { normal: pl.normal.to_array(), offset: pl.offset }).collect();
        ffi::cloth_step(self.0.pin_mut(), dt, &p, &spheres, &capsules, &planes);
    }

    /// Smooth per-particle normals from the current positions.
    pub fn normals(&self) -> Vec<Vec3> {
        ffi::cloth_normals(&self.0).chunks_exact(3).map(|c| Vec3::new(c[0], c[1], c[2])).collect()
    }
}

// ---- swing / twist ----------------------------------------------------------------------------------------

/// Limits of a ball-and-socket joint (shoulder, hip, spine) in its local frame.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SwingTwistLimits {
    /// Twist axis in the joint's frame (the bone direction); normalised for you.
    pub axis: Vec3,
    /// Half-angles of the elliptical swing cone, in radians. Zero locks that direction.
    pub swing_y: f32,
    pub swing_z: f32,
    /// Allowed twist range, radians, about `axis`.
    pub twist_min: f32,
    pub twist_max: f32,
}

impl SwingTwistLimits {
    fn to_ffi(self) -> ffi::SwingTwistLimitsDesc {
        ffi::SwingTwistLimitsDesc {
            axis: self.axis.to_array(),
            swing_y: self.swing_y,
            swing_z: self.swing_z,
            twist_min: self.twist_min,
            twist_max: self.twist_max,
        }
    }
}

impl Default for SwingTwistLimits {
    fn default() -> Self {
        Self { axis: Vec3::X, swing_y: 0.8, swing_z: 0.8, twist_min: -0.5, twist_max: 0.5 }
    }
}

/// Rotation split around a twist axis: `rotation == swing * twist`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SwingTwist {
    pub swing: Quat,
    pub twist: Quat,
}

/// Splits `rotation` into swing and twist about `axis`.
pub fn decompose_swing_twist(rotation: Quat, axis: Vec3) -> SwingTwist {
    let d = ffi::decompose_swing_twist(qa(rotation), axis.to_array());
    SwingTwist { swing: q(d.swing), twist: q(d.twist) }
}

/// Signed twist angle in `(-pi, pi]` about `axis`.
pub fn twist_angle(rotation: Quat, axis: Vec3) -> f32 {
    ffi::twist_angle(qa(rotation), axis.to_array())
}

/// True when `rotation` is inside the limits (with a small tolerance).
pub fn within_limits(rotation: Quat, limits: &SwingTwistLimits) -> bool {
    ffi::within_limits(qa(rotation), &limits.to_ffi())
}

/// The closest rotation to `rotation` that satisfies `limits`: twist clamped to its range, swing pulled back inside the elliptical cone.
pub fn clamp_to_limits(rotation: Quat, limits: &SwingTwistLimits) -> Quat {
    q(ffi::clamp_to_limits(qa(rotation), &limits.to_ffi()))
}

/// Active-ragdoll drive: the angular velocity (rad/s, in the parent's frame) a motor applies to pull a joint from `current` toward `target`, as a
/// PD controller. `angular_velocity` is the joint's present relative velocity.
pub fn pd_drive(current: Quat, target: Quat, angular_velocity: Vec3, stiffness: f32, damping: f32) -> Vec3 {
    v(ffi::pd_drive(qa(current), qa(target), angular_velocity.to_array(), stiffness, damping))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stepper_clamps_a_long_frame() {
        let mut s = FixedStepper::new(0.0, 0);
        assert!((s.step() - 1.0 / 60.0).abs() < 1e-12);
        assert_eq!(s.advance(0.05), 3);
        assert_eq!(s.advance(10.0), 8);
        assert!(s.dropped_seconds() > 9.0);
        assert_eq!(s.advance(-1.0), 0);
        s.reset();
        assert_eq!(s.total_steps(), 0);
    }

    #[test]
    fn layers_are_symmetric() {
        let mut t = LayerTable::new();
        let world = t.add("world").unwrap();
        let player = t.add("player").unwrap();
        assert_eq!(t.add("world"), Some(world));
        assert_eq!(t.find("player"), Some(player));
        assert!(t.find("nope").is_none());
        t.set_collides(world, player, false);
        assert!(!t.collides(player, world));
        assert_eq!(t.mask_of(&["player", "missing"]), LayerTable::bit(player));
        assert_eq!(t.all(), 3);
        assert_eq!(t.name(player), "player");
        assert_eq!(t.collision_mask(99), 0);
    }

    #[test]
    fn a_character_lands_on_a_floor_and_walks() {
        let mut world = World::new();
        world.add_plane(&Plane { normal: Vec3::Y, offset: 0.0, object: 7 });
        let mut c = CharacterController::new(&CharacterSettings::default(), Vec3::new(0.0, 2.0, 0.0));
        let mut landed = false;
        for _ in 0..120 {
            landed |= c.step(&world, Vec3::ZERO, 1.0 / 60.0, false).landed;
        }
        assert!(landed);
        let s = c.state();
        assert!(s.grounded && s.ground_object == 7 && s.position.y.abs() < 0.05);
        c.step(&world, Vec3::new(2.0, 0.0, 0.0), 0.5, false);
        assert!(c.state().position.x > 0.3);
        let hit = world.sweep_capsule(Vec3::new(0.0, 3.0, 0.0), 0.3, 1.8, Vec3::new(0.0, -5.0, 0.0)).unwrap();
        assert!(hit.fraction < 1.0 && hit.object == 7);
    }

    #[test]
    fn contacts_are_filtered() {
        let mut f = ContactEventFilter::new(&ContactEventConfig::default());
        f.begin_frame(1.0);
        let weak = ContactPoint {
            body_a: 1,
            body_b: 2,
            point: Vec3::ZERO,
            normal: Vec3::Y,
            normal_impulse: 0.01,
            approach_speed: 0.01,
            slide_speed: 0.0,
            material_a: NO_MATERIAL,
            material_b: NO_MATERIAL,
        };
        assert!(!f.submit(&weak));
        let strong = ContactPoint { normal_impulse: 100.0, approach_speed: 5.0, ..weak };
        assert!(f.submit(&strong));
        let events = f.events();
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].kind, ContactKind::Impact);
        assert!(events[0].intensity > 0.0);
        f.begin_frame(5.0);
        assert!(f.events().is_empty());
    }

    #[test]
    fn a_shot_goes_through_drywall_and_stops_in_steel() {
        let mut table = SurfaceTable::new();
        let drywall = table.add(&SurfaceMaterial { name: "drywall".into(), penetration_resistance: 400.0, ..SurfaceMaterial::default() });
        let steel = table.add(&SurfaceMaterial { name: "steel".into(), penetration_resistance: 90000.0, ..SurfaceMaterial::default() });
        assert_eq!(table.find("steel"), Some(steel));
        assert_eq!(table.len(), 2);
        assert!(table.get(steel).unwrap().penetration_resistance > 80000.0);
        let bullet = Projectile { position: Vec3::ZERO, direction: -Vec3::Z, energy: 3000.0 };
        let wall = |material: u32| {
            move |origin: Vec3, direction: Vec3, max_distance: f32| {
                if direction.z >= -0.001 {
                    return None;
                }
                let distance = (origin.z + 5.0) / -direction.z;
                (distance >= 0.0 && distance <= max_distance).then(|| SurfaceHit {
                    point: origin + direction * distance,
                    normal: Vec3::Z,
                    distance,
                    thickness: 0.2,
                    material,
                    object: 42,
                })
            }
        };
        let through = trace_shot(&table, &bullet, &ShotOptions::default(), wall(drywall));
        assert_eq!(through[0].result.outcome, ImpactOutcome::Penetrated);
        assert_eq!(through[0].hit.object, 42);
        let stopped = trace_shot(&table, &bullet, &ShotOptions::default(), wall(steel));
        assert_eq!(stopped[0].result.outcome, ImpactOutcome::Stopped);
        let direct = resolve_impact(&bullet, &table.get(drywall).unwrap(), Vec3::new(0.0, 0.0, -5.0), Vec3::Z, 0.2, [0.5, 0.5]);
        assert_eq!(direct.outcome, ImpactOutcome::Penetrated);
    }

    #[test]
    fn cloth_hangs_from_pins_and_rests_on_a_plane() {
        assert!(Cloth::grid(1, 4, 0.1, Vec3::ZERO, Vec3::X, Vec3::Z, 1.0).is_none());
        let mut cloth = Cloth::grid(5, 5, 0.1, Vec3::new(0.0, 1.0, 0.0), Vec3::X, Vec3::Z, 1.0).unwrap();
        assert_eq!(cloth.particle_count(), 25);
        for i in 0..5 {
            cloth.pin(i, true);
        }
        assert!(cloth.is_pinned(0));
        let before = cloth.positions();
        let params = ClothParams::default();
        for _ in 0..60 {
            cloth.step(1.0 / 60.0, &params, &ClothColliders::default());
        }
        let after = cloth.positions();
        assert!((after[0].y - before[0].y).abs() < 1e-3);
        assert!(after[24].y < before[24].y - 0.05);
        let floor = ClothColliders { planes: vec![ClothPlane { normal: Vec3::Y, offset: 0.8 }], ..ClothColliders::default() };
        for _ in 0..120 {
            cloth.step(1.0 / 60.0, &params, &floor);
        }
        assert!(cloth.positions()[24].y > 0.75);
        assert_eq!(cloth.normals().len(), 25);
        assert_eq!(cloth.indices().len(), 4 * 4 * 6);
        assert!(Cloth::from_triangles(&[Vec3::ZERO, Vec3::X, Vec3::Y], &[0, 1, 3], 1.0).is_none());
        assert!(Cloth::from_triangles(&[Vec3::ZERO, Vec3::X, Vec3::Y], &[0, 1, 2], 1.0).is_some());
    }

    #[test]
    fn swing_twist_clamps_a_quarter_turn() {
        let quarter = Quat::from_axis_angle(Vec3::X, std::f32::consts::FRAC_PI_2);
        let split = decompose_swing_twist(quarter, Vec3::X);
        assert!(split.swing.dot(Quat::IDENTITY).abs() > 0.999);
        assert!((twist_angle(quarter, Vec3::X) - std::f32::consts::FRAC_PI_2).abs() < 1e-3);
        let limits = SwingTwistLimits::default();
        assert!(!within_limits(quarter, &limits));
        assert!(within_limits(clamp_to_limits(quarter, &limits), &limits));
        assert!(pd_drive(Quat::IDENTITY, quarter, Vec3::ZERO, 40.0, 4.0).x > 0.0);
    }

    #[test]
    fn interpolation_lerps_position() {
        let a = BodyPose { position: Vec3::ZERO, rotation: Quat::IDENTITY };
        let b = BodyPose { position: Vec3::new(2.0, 0.0, 0.0), rotation: Quat::IDENTITY };
        assert!((BodyPose::interpolate(&a, &b, 0.5).position.x - 1.0).abs() < 1e-5);
    }
}
