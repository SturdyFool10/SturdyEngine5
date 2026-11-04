#include "sturdy_rs/physics.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

// Generated from src/physics.rs.
#include "sturdy_rs/physics_bridge.h"

namespace sturdy_rs::physics {
    namespace {

        namespace P = SFT::Physics;

        glm::vec3 v3(const std::array<float, 3> &a) noexcept { return {a[0], a[1], a[2]}; }
        std::array<float, 3> a3(const glm::vec3 &v) noexcept { return {v.x, v.y, v.z}; }
        glm::quat q4(const std::array<float, 4> &a) noexcept { return glm::quat{a[3], a[0], a[1], a[2]}; }
        std::array<float, 4> a4(const glm::quat &q) noexcept { return {q.x, q.y, q.z, q.w}; }

        bool usable(const glm::vec3 &v) noexcept {
            const float length = glm::length(v);
            return std::isfinite(length) && length > 1e-6f;
        }
        // A zero or non-finite quaternion would turn into NaNs inside the joint maths; the identity is the neutral answer.
        glm::quat unit(const std::array<float, 4> &a) noexcept {
            const glm::quat q = q4(a);
            const float length = glm::length(q);
            return std::isfinite(length) && length > 1e-6f ? q / length : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
        }

        std::string_view sv(rust::Str s) noexcept { return {s.data(), s.size()}; }
        SFT::UString ustring(rust::Str s) { return SFT::UString::try_from_utf8(sv(s)).value_or(SFT::UString{}); }
        rust::String rs(const SFT::UString &s) {
            const auto view = s.cpp_string_view();
            return rust::String::lossy(view.data(), view.size());
        }

        ContactPointDesc to_desc(const P::ContactPoint &c) noexcept {
            return ContactPointDesc{c.body_a, c.body_b, a3(c.point), a3(c.normal), c.normal_impulse, c.approach_speed, c.slide_speed, c.material_a, c.material_b};
        }
        P::ContactPoint to_native(const ContactPointDesc &c) noexcept {
            P::ContactPoint out;
            out.body_a = c.body_a;
            out.body_b = c.body_b;
            out.point = v3(c.point);
            out.normal = v3(c.normal);
            out.normal_impulse = c.normal_impulse;
            out.approach_speed = c.approach_speed;
            out.slide_speed = c.slide_speed;
            out.material_a = c.material_a;
            out.material_b = c.material_b;
            return out;
        }
        P::ContactEventConfig to_native(const ContactConfigDesc &c) noexcept {
            P::ContactEventConfig out;
            out.min_impulse = c.min_impulse;
            out.min_speed = c.min_speed;
            out.pair_cooldown_seconds = c.pair_cooldown_seconds;
            out.merge_radius = c.merge_radius;
            out.max_events_per_frame = c.max_events_per_frame;
            out.full_intensity_impulse = c.full_intensity_impulse;
            return out;
        }
        P::SurfaceMaterial to_native(const SurfaceMaterialDesc &m) {
            P::SurfaceMaterial out;
            out.name = ustring(rust::Str{m.name.data(), m.name.size()});
            out.penetration_resistance = m.penetration_resistance;
            out.ricochet_angle_degrees = m.ricochet_angle_degrees;
            out.ricochet_energy_retained = m.ricochet_energy_retained;
            out.ricochet_scatter = m.ricochet_scatter;
            out.exit_deflection = m.exit_deflection;
            return out;
        }
        SurfaceMaterialDesc to_desc(const P::SurfaceMaterial &m) {
            return SurfaceMaterialDesc{rs(m.name), m.penetration_resistance, m.ricochet_angle_degrees, m.ricochet_energy_retained, m.ricochet_scatter, m.exit_deflection};
        }
        P::Projectile to_native(const ProjectileDesc &p) noexcept {
            const glm::vec3 direction = v3(p.direction);
            return P::Projectile{v3(p.position), usable(direction) ? glm::normalize(direction) : glm::vec3{0.0f, 0.0f, -1.0f}, p.energy};
        }
        ImpactResultDesc to_desc(const P::ImpactResult &r) noexcept {
            return ImpactResultDesc{static_cast<std::uint8_t>(r.outcome), a3(r.point), a3(r.direction), r.remaining_energy, r.depth};
        }
        P::SwingTwistLimits to_native(const SwingTwistLimitsDesc &l) noexcept {
            P::SwingTwistLimits out;
            const glm::vec3 axis = v3(l.axis);
            out.axis = usable(axis) ? glm::normalize(axis) : glm::vec3{1.0f, 0.0f, 0.0f};
            out.swing_y = l.swing_y;
            out.swing_z = l.swing_z;
            out.twist_min = l.twist_min;
            out.twist_max = l.twist_max;
            return out;
        }
        glm::vec3 axis_of(const std::array<float, 3> &a) noexcept {
            const glm::vec3 axis = v3(a);
            return usable(axis) ? glm::normalize(axis) : glm::vec3{1.0f, 0.0f, 0.0f};
        }

    } // namespace

    // ---- fixed stepper ----------------------------------------------------------------------------

    std::unique_ptr<StepperBox> stepper_new(double step_seconds, std::uint32_t max_steps_per_frame) {
        return std::make_unique<StepperBox>(step_seconds > 0.0 && std::isfinite(step_seconds) ? step_seconds : 1.0 / 60.0, max_steps_per_frame != 0 ? max_steps_per_frame : 8u);
    }
    std::uint32_t stepper_advance(StepperBox &stepper, double delta_seconds) {
        return std::isfinite(delta_seconds) && delta_seconds > 0.0 ? stepper.advance(delta_seconds) : 0u;
    }
    double stepper_alpha(const StepperBox &stepper) { return stepper.alpha(); }
    double stepper_step(const StepperBox &stepper) { return stepper.step(); }
    std::uint64_t stepper_total_steps(const StepperBox &stepper) { return stepper.total_steps(); }
    double stepper_simulated_time(const StepperBox &stepper) { return stepper.simulated_time(); }
    double stepper_dropped_seconds(const StepperBox &stepper) { return stepper.dropped_seconds(); }
    void stepper_set_step(StepperBox &stepper, double step_seconds) {
        if (step_seconds > 0.0 && std::isfinite(step_seconds)) {
            stepper.set_step(step_seconds);
        }
    }
    void stepper_reset(StepperBox &stepper) { stepper.reset(); }
    PoseDesc interpolate_pose(const PoseDesc &previous, const PoseDesc &current, float alpha) {
        const P::BodyPose pose = P::interpolate({v3(previous.position), q4(previous.rotation)}, {v3(current.position), q4(current.rotation)}, alpha);
        return PoseDesc{a3(pose.position), a4(pose.rotation)};
    }

    // ---- layers -----------------------------------------------------------------------------------

    std::unique_ptr<LayersBox> layers_new() { return std::make_unique<LayersBox>(); }
    std::uint32_t layers_add(LayersBox &layers, rust::Str name, bool &found) {
        const auto index = layers.add(SFT::ustr{sv(name)});
        found = index.has_value();
        return index.value_or(0);
    }
    std::uint32_t layers_find(const LayersBox &layers, rust::Str name, bool &found) {
        const auto index = layers.find(SFT::ustr{sv(name)});
        found = index.has_value();
        return index.value_or(0);
    }
    rust::String layers_name(const LayersBox &layers, std::uint32_t index) { return rs(layers.name(index)); }
    std::uint32_t layers_count(const LayersBox &layers) { return layers.count(); }
    std::uint64_t layers_mask_of(const LayersBox &layers, const rust::Vec<rust::String> &names) {
        std::vector<SFT::UString> list;
        list.reserve(names.size());
        for (const rust::String &name : names) {
            list.push_back(ustring(rust::Str{name.data(), name.size()}));
        }
        return layers.mask_of(list);
    }
    std::uint64_t layers_all(const LayersBox &layers) { return layers.all(); }
    void layers_set_collides(LayersBox &layers, std::uint32_t a, std::uint32_t b, bool collides) { layers.set_collides(a, b, collides); }
    bool layers_collides(const LayersBox &layers, std::uint32_t a, std::uint32_t b) { return a < layers.count() && b < layers.count() && layers.collides(a, b); }
    std::uint64_t layers_collision_mask(const LayersBox &layers, std::uint32_t layer) { return layer < layers.count() ? layers.collision_mask(layer) : 0; }

    // ---- simple world + character -----------------------------------------------------------------

    std::unique_ptr<WorldBox> world_new() { return std::make_unique<WorldBox>(); }
    std::uint32_t world_add_box(WorldBox &world, const BoxDesc &desc) {
        const auto index = static_cast<std::uint32_t>(world.box_count());
        P::SimpleWorld::Box box;
        box.min = v3(desc.min);
        box.max = v3(desc.max);
        box.object = desc.object;
        box.velocity = v3(desc.velocity);
        world.add_box(box);
        return index;
    }
    void world_add_plane(WorldBox &world, const PlaneDesc &desc) {
        P::SimpleWorld::Plane plane;
        const glm::vec3 normal = v3(desc.normal);
        plane.normal = usable(normal) ? glm::normalize(normal) : glm::vec3{0.0f, 1.0f, 0.0f};
        plane.offset = desc.offset;
        plane.object = desc.object;
        world.add_plane(plane);
    }
    std::uint32_t world_box_count(const WorldBox &world) { return static_cast<std::uint32_t>(world.box_count()); }
    void world_move_box(WorldBox &world, std::uint32_t index, std::array<float, 3> delta) { world.move_box(index, v3(delta)); }
    bool world_overlaps_capsule(const WorldBox &world, std::array<float, 3> feet, float radius, float height) {
        return world.overlaps_capsule(v3(feet), radius, height);
    }
    bool world_sweep_capsule(const WorldBox &world, std::array<float, 3> feet, float radius, float height, std::array<float, 3> delta, SweepHitDesc &hit) {
        P::SweepHit native;
        if (!world.sweep_capsule(v3(feet), radius, height, v3(delta), native)) {
            return false;
        }
        hit = SweepHitDesc{native.fraction, a3(native.normal), a3(native.point), native.object, a3(native.object_velocity)};
        return true;
    }
    CharacterSettingsDesc character_settings_default() {
        const P::CharacterSettings d;
        return CharacterSettingsDesc{d.radius, d.standing_height, d.crouching_height, a3(d.up), d.gravity, d.slope_limit_degrees,
                                     d.step_height, d.snap_distance, d.skin_width, d.max_slides, d.jump_speed, d.terminal_speed};
    }
    std::unique_ptr<CharacterBox> character_new(const CharacterSettingsDesc &settings, std::array<float, 3> feet) {
        P::CharacterSettings native;
        native.radius = settings.radius;
        native.standing_height = settings.standing_height;
        native.crouching_height = settings.crouching_height;
        const glm::vec3 up = v3(settings.up);
        native.up = usable(up) ? glm::normalize(up) : glm::vec3{0.0f, 1.0f, 0.0f};
        native.gravity = settings.gravity;
        native.slope_limit_degrees = settings.slope_limit_degrees;
        native.step_height = settings.step_height;
        native.snap_distance = settings.snap_distance;
        native.skin_width = settings.skin_width;
        native.max_slides = settings.max_slides;
        native.jump_speed = settings.jump_speed;
        native.terminal_speed = settings.terminal_speed;
        return std::make_unique<CharacterBox>(native, v3(feet));
    }
    MoveResultDesc character_move(CharacterBox &character, const WorldBox &world, std::array<float, 3> desired_velocity, float dt, bool jump) {
        const P::MoveResult r = character.move(world, v3(desired_velocity), std::isfinite(dt) && dt > 0.0f ? dt : 0.0f, jump);
        return MoveResultDesc{a3(r.displacement), r.grounded, r.hit_ceiling, r.stepped_up, r.jumped, r.landed, r.landing_speed};
    }
    bool character_set_crouching(CharacterBox &character, const WorldBox &world, bool crouching) { return character.set_crouching(world, crouching); }
    void character_teleport(CharacterBox &character, std::array<float, 3> feet) { character.teleport(v3(feet)); }
    void character_add_impulse(CharacterBox &character, std::array<float, 3> velocity_change) { character.add_impulse(v3(velocity_change)); }
    CharacterStateDesc character_state(const CharacterBox &c) {
        return CharacterStateDesc{a3(c.position()), a3(c.velocity()), a3(c.ground_normal()), c.ground_object(), c.height(), c.grounded(), c.crouching()};
    }

    // ---- contact events ---------------------------------------------------------------------------

    ContactConfigDesc contact_config_default() {
        const P::ContactEventConfig d;
        return ContactConfigDesc{d.min_impulse, d.min_speed, d.pair_cooldown_seconds, d.merge_radius, d.max_events_per_frame, d.full_intensity_impulse};
    }
    std::unique_ptr<ContactFilterBox> contact_filter_new(const ContactConfigDesc &config) { return std::make_unique<ContactFilterBox>(to_native(config)); }
    void contact_filter_set_config(ContactFilterBox &filter, const ContactConfigDesc &config) { filter.set_config(to_native(config)); }
    void contact_filter_begin_frame(ContactFilterBox &filter, double time_seconds) { filter.begin_frame(time_seconds); }
    bool contact_filter_submit(ContactFilterBox &filter, const ContactPointDesc &contact) { return filter.submit(to_native(contact)); }
    rust::Vec<ContactEventDesc> contact_filter_events(const ContactFilterBox &filter) {
        rust::Vec<ContactEventDesc> out;
        for (const P::ContactEvent &event : filter.events()) {
            out.push_back(ContactEventDesc{to_desc(event.contact), event.kind == P::ContactKind::Scrape, event.intensity});
        }
        return out;
    }

    // ---- surfaces + ballistics --------------------------------------------------------------------

    SurfaceMaterialDesc surface_material_default() { return to_desc(P::SurfaceMaterial{}); }
    std::unique_ptr<SurfacesBox> surfaces_new() { return std::make_unique<SurfacesBox>(); }
    std::uint32_t surfaces_add(SurfacesBox &surfaces, const SurfaceMaterialDesc &material) { return surfaces.add(to_native(material)); }
    std::uint32_t surfaces_find(const SurfacesBox &surfaces, rust::Str name, bool &found) {
        const P::MaterialId id = surfaces.find(SFT::ustr{sv(name)});
        found = id != P::no_material;
        return found ? id : 0;
    }
    std::uint32_t surfaces_count(const SurfacesBox &surfaces) { return static_cast<std::uint32_t>(surfaces.size()); }
    bool surfaces_get(const SurfacesBox &surfaces, std::uint32_t id, SurfaceMaterialDesc &out) {
        const P::SurfaceMaterial *material = surfaces.get(id);
        if (material == nullptr) {
            return false;
        }
        out = to_desc(*material);
        return true;
    }
    ImpactResultDesc resolve_impact(const ProjectileDesc &projectile, const SurfaceMaterialDesc &material, std::array<float, 3> entry_point, std::array<float, 3> normal,
                                    float thickness, std::array<float, 2> random) {
        return to_desc(P::resolve_impact(to_native(projectile), to_native(material), v3(entry_point), axis_of(normal), thickness, glm::vec2{random[0], random[1]}));
    }
    ShotOptionsDesc shot_options_default() {
        const P::ShotOptions d;
        return ShotOptionsDesc{d.max_distance, d.max_impacts, d.minimum_energy, d.seed};
    }
    rust::Vec<ShotImpactDesc> trace_shot(const SurfacesBox &surfaces, const ProjectileDesc &projectile, const ShotOptionsDesc &options, RayQuery &query) {
        P::ShotOptions native;
        native.max_distance = options.max_distance;
        native.max_impacts = options.max_impacts;
        native.minimum_energy = options.minimum_energy;
        native.seed = options.seed;
        const P::RayQuery ray = [&query](const glm::vec3 &origin, const glm::vec3 &direction, float max_distance, P::SurfaceHit &hit) {
            SurfaceHitDesc foreign{};
            if (!query.call(a3(origin), a3(direction), max_distance, foreign)) {
                return false;
            }
            hit.point = v3(foreign.point);
            hit.normal = v3(foreign.normal);
            hit.distance = foreign.distance;
            hit.thickness = foreign.thickness;
            hit.material = foreign.material;
            hit.object = foreign.object;
            return true;
        };
        rust::Vec<ShotImpactDesc> out;
        for (const P::ShotImpact &impact : P::trace_shot(to_native(projectile), surfaces, ray, native)) {
            out.push_back(ShotImpactDesc{SurfaceHitDesc{a3(impact.hit.point), a3(impact.hit.normal), impact.hit.distance, impact.hit.thickness, impact.hit.material,
                                                        impact.hit.object},
                                         to_desc(impact.result)});
        }
        return out;
    }

    // ---- cloth ------------------------------------------------------------------------------------

    ClothParamsDesc cloth_params_default() {
        const P::ClothParams d;
        return ClothParamsDesc{a3(d.gravity), d.stretch_compliance, d.bend_compliance, d.damping, d.substeps, d.collision_thickness, d.friction, a3(d.wind), d.drag};
    }
    std::unique_ptr<ClothBox> cloth_grid(std::uint32_t columns, std::uint32_t rows, float spacing, std::array<float, 3> origin, std::array<float, 3> right,
                                         std::array<float, 3> down, float total_mass) {
        return std::make_unique<ClothBox>(P::Cloth::grid(columns, rows, spacing, v3(origin), v3(right), v3(down), total_mass));
    }
    std::unique_ptr<ClothBox> cloth_from_triangles(rust::Slice<const float> positions, rust::Slice<const std::uint32_t> indices, float total_mass) {
        std::vector<glm::vec3> converted;
        converted.reserve(positions.size() / 3);
        for (std::size_t i = 0; i + 2 < positions.size(); i += 3) {
            converted.emplace_back(positions[i], positions[i + 1], positions[i + 2]);
        }
        return std::make_unique<ClothBox>(P::Cloth::from_triangles(converted, std::span<const std::uint32_t>{indices.data(), indices.size()}, total_mass));
    }
    std::uint32_t cloth_particle_count(const ClothBox &cloth) { return static_cast<std::uint32_t>(cloth.particle_count()); }
    std::array<std::uint32_t, 2> cloth_constraint_counts(const ClothBox &cloth) {
        return {static_cast<std::uint32_t>(cloth.stretch_constraint_count()), static_cast<std::uint32_t>(cloth.bend_constraint_count())};
    }
    rust::Vec<float> cloth_positions(const ClothBox &cloth) {
        rust::Vec<float> out;
        out.reserve(cloth.particle_count() * 3);
        for (const glm::vec3 &p : cloth.positions()) {
            out.push_back(p.x);
            out.push_back(p.y);
            out.push_back(p.z);
        }
        return out;
    }
    rust::Vec<std::uint32_t> cloth_indices(const ClothBox &cloth) {
        rust::Vec<std::uint32_t> out;
        for (const std::uint32_t index : cloth.indices()) {
            out.push_back(index);
        }
        return out;
    }
    void cloth_pin(ClothBox &cloth, std::uint32_t particle, bool pinned) {
        if (particle < cloth.particle_count()) {
            cloth.pin(particle, pinned);
        }
    }
    bool cloth_is_pinned(const ClothBox &cloth, std::uint32_t particle) { return particle < cloth.particle_count() && cloth.is_pinned(particle); }
    void cloth_move_pinned(ClothBox &cloth, std::uint32_t particle, std::array<float, 3> position) {
        if (particle < cloth.particle_count()) {
            cloth.move_pinned(particle, v3(position));
        }
    }
    void cloth_step(ClothBox &cloth, float dt, const ClothParamsDesc &params, const rust::Vec<ClothSphereDesc> &spheres, const rust::Vec<ClothCapsuleDesc> &capsules,
                    const rust::Vec<ClothPlaneDesc> &planes) {
        P::ClothParams native;
        native.gravity = v3(params.gravity);
        native.stretch_compliance = params.stretch_compliance;
        native.bend_compliance = params.bend_compliance;
        native.damping = params.damping;
        native.substeps = params.substeps;
        native.collision_thickness = params.collision_thickness;
        native.friction = params.friction;
        native.wind = v3(params.wind);
        native.drag = params.drag;
        std::vector<P::ClothSphere> sphere_list;
        std::vector<P::ClothCapsule> capsule_list;
        std::vector<P::ClothPlane> plane_list;
        for (const ClothSphereDesc &s : spheres) {
            sphere_list.push_back({v3(s.center), s.radius});
        }
        for (const ClothCapsuleDesc &c : capsules) {
            capsule_list.push_back({v3(c.a), v3(c.b), c.radius});
        }
        for (const ClothPlaneDesc &p : planes) {
            plane_list.push_back({axis_of(p.normal), p.offset});
        }
        cloth.step(std::isfinite(dt) && dt > 0.0f ? dt : 0.0f, native, P::ClothColliders{sphere_list, capsule_list, plane_list});
    }
    rust::Vec<float> cloth_normals(const ClothBox &cloth) {
        std::vector<glm::vec3> normals;
        cloth.compute_normals(normals);
        rust::Vec<float> out;
        out.reserve(normals.size() * 3);
        for (const glm::vec3 &n : normals) {
            out.push_back(n.x);
            out.push_back(n.y);
            out.push_back(n.z);
        }
        return out;
    }

    // ---- swing / twist ----------------------------------------------------------------------------

    SwingTwistDesc decompose_swing_twist(std::array<float, 4> rotation, std::array<float, 3> axis) {
        const P::SwingTwist split = P::decompose_swing_twist(unit(rotation), axis_of(axis));
        return SwingTwistDesc{a4(split.swing), a4(split.twist)};
    }
    float twist_angle(std::array<float, 4> rotation, std::array<float, 3> axis) { return P::twist_angle(unit(rotation), axis_of(axis)); }
    bool within_limits(std::array<float, 4> rotation, const SwingTwistLimitsDesc &limits) { return P::within_limits(unit(rotation), to_native(limits)); }
    std::array<float, 4> clamp_to_limits(std::array<float, 4> rotation, const SwingTwistLimitsDesc &limits) {
        return a4(P::clamp_to_limits(unit(rotation), to_native(limits)));
    }
    std::array<float, 3> pd_drive(std::array<float, 4> current, std::array<float, 4> target, std::array<float, 3> angular_velocity, float stiffness, float damping) {
        return a3(P::pd_drive(unit(current), unit(target), v3(angular_velocity), stiffness, damping));
    }

} // namespace sturdy_rs::physics
