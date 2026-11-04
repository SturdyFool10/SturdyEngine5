#include <CxxApi/Physics.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <glm/geometric.hpp>

#include <CxxApi/Conversions.hpp>
#include <CxxApi/Error.hpp>

namespace SFT::CxxApi {

    namespace {

        glm::vec3 v3(const float (&a)[3]) noexcept { return {a[0], a[1], a[2]}; }
        void put(float (&out)[3], const glm::vec3 &v) noexcept {
            out[0] = v.x;
            out[1] = v.y;
            out[2] = v.z;
        }

        bool usable(const glm::vec3 &v) noexcept {
            const float length = glm::length(v);
            return std::isfinite(length) && length > 1e-6f;
        }
        glm::vec3 direction_or(const glm::vec3 &v, const glm::vec3 &fallback) noexcept { return usable(v) ? glm::normalize(v) : fallback; }
        // A zero or non-finite quaternion would turn into NaNs inside the joint maths; the identity is the neutral answer.
        glm::quat unit(std::array<float, 4> a) noexcept {
            const glm::quat q = to_quat(a);
            const float length = glm::length(q);
            return std::isfinite(length) && length > 1e-6f ? q / length : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
        }

        PhysicsContactPoint to_pod(const Physics::ContactPoint &c) noexcept {
            PhysicsContactPoint out;
            out.body_a = c.body_a;
            out.body_b = c.body_b;
            put(out.point, c.point);
            put(out.normal, c.normal);
            out.normal_impulse = c.normal_impulse;
            out.approach_speed = c.approach_speed;
            out.slide_speed = c.slide_speed;
            out.material_a = c.material_a;
            out.material_b = c.material_b;
            return out;
        }

        Physics::ContactEventConfig to_native(const PhysicsContactConfig &c) noexcept {
            Physics::ContactEventConfig out;
            out.min_impulse = c.min_impulse;
            out.min_speed = c.min_speed;
            out.pair_cooldown_seconds = c.pair_cooldown_seconds;
            out.merge_radius = c.merge_radius;
            out.max_events_per_frame = c.max_events_per_frame;
            out.full_intensity_impulse = c.full_intensity_impulse;
            return out;
        }

        Physics::SurfaceMaterial to_native(const std::string &name, const PhysicsSurfaceMaterial &m) {
            Physics::SurfaceMaterial out;
            out.name = to_ustring(name);
            out.penetration_resistance = m.penetration_resistance;
            out.ricochet_angle_degrees = m.ricochet_angle_degrees;
            out.ricochet_energy_retained = m.ricochet_energy_retained;
            out.ricochet_scatter = m.ricochet_scatter;
            out.exit_deflection = m.exit_deflection;
            return out;
        }

        Physics::Projectile to_native(const PhysicsProjectile &p) noexcept {
            return Physics::Projectile{v3(p.position), direction_or(v3(p.direction), glm::vec3{0.0f, 0.0f, -1.0f}), p.energy};
        }

        PhysicsImpactResult to_pod(const Physics::ImpactResult &r) noexcept {
            PhysicsImpactResult out;
            out.outcome = static_cast<std::uint8_t>(r.outcome);
            put(out.point, r.point);
            put(out.direction, r.direction);
            out.remaining_energy = r.remaining_energy;
            out.depth = r.depth;
            return out;
        }

        Physics::SwingTwistLimits to_native(const PhysicsSwingTwistLimits &l) noexcept {
            Physics::SwingTwistLimits out;
            out.axis = direction_or(v3(l.axis), glm::vec3{1.0f, 0.0f, 0.0f});
            out.swing_y = l.swing_y;
            out.swing_z = l.swing_z;
            out.twist_min = l.twist_min;
            out.twist_max = l.twist_max;
            return out;
        }

        Physics::ClothParams to_native(const PhysicsClothParams &p) noexcept {
            Physics::ClothParams out;
            out.gravity = v3(p.gravity);
            out.stretch_compliance = p.stretch_compliance;
            out.bend_compliance = p.bend_compliance;
            out.damping = p.damping;
            out.substeps = p.substeps;
            out.collision_thickness = p.collision_thickness;
            out.friction = p.friction;
            out.wind = v3(p.wind);
            out.drag = p.drag;
            return out;
        }

        // The cloth solver is O(particles); this keeps a bad size from asking for gigabytes.
        constexpr std::uint64_t max_cloth_particles = 1u << 22;

    } // namespace

    // ---- fixed stepper ----

    std::unique_ptr<Physics::FixedStepper> physics_stepper_new(double step_seconds, std::uint32_t max_steps_per_frame) {
        return std::make_unique<Physics::FixedStepper>(step_seconds > 0.0 && std::isfinite(step_seconds) ? step_seconds : 1.0 / 60.0,
                                                       max_steps_per_frame != 0 ? max_steps_per_frame : 8u);
    }
    std::uint32_t physics_stepper_advance(Physics::FixedStepper &stepper, double delta_seconds) noexcept {
        return std::isfinite(delta_seconds) && delta_seconds > 0.0 ? stepper.advance(delta_seconds) : 0u;
    }
    double physics_stepper_alpha(const Physics::FixedStepper &stepper) noexcept { return stepper.alpha(); }
    double physics_stepper_step(const Physics::FixedStepper &stepper) noexcept { return stepper.step(); }
    std::uint64_t physics_stepper_total_steps(const Physics::FixedStepper &stepper) noexcept { return stepper.total_steps(); }
    double physics_stepper_simulated_time(const Physics::FixedStepper &stepper) noexcept { return stepper.simulated_time(); }
    double physics_stepper_dropped_seconds(const Physics::FixedStepper &stepper) noexcept { return stepper.dropped_seconds(); }
    void physics_stepper_set_step(Physics::FixedStepper &stepper, double step_seconds) noexcept {
        if (step_seconds > 0.0 && std::isfinite(step_seconds)) {
            stepper.set_step(step_seconds);
        }
    }
    void physics_stepper_reset(Physics::FixedStepper &stepper) noexcept { stepper.reset(); }
    PhysicsPose physics_interpolate_pose(const PhysicsPose &previous, const PhysicsPose &current, float alpha) noexcept {
        const Physics::BodyPose pose = Physics::interpolate({v3(previous.position), to_quat(previous.rotation)}, {v3(current.position), to_quat(current.rotation)}, alpha);
        PhysicsPose out;
        put(out.position, pose.position);
        store(out.rotation, pose.rotation);
        return out;
    }

    // ---- layers ----

    std::unique_ptr<Physics::LayerTable> physics_layers_new() { return std::make_unique<Physics::LayerTable>(); }
    bool physics_layers_add(Physics::LayerTable &layers, const std::string &name, std::uint32_t &index) {
        const auto added = layers.add(ustr{std::string_view{name}});
        if (added) {
            index = *added;
        }
        return added.has_value();
    }
    bool physics_layers_find(const Physics::LayerTable &layers, const std::string &name, std::uint32_t &index) {
        const auto found = layers.find(ustr{std::string_view{name}});
        if (found) {
            index = *found;
        }
        return found.has_value();
    }
    std::unique_ptr<std::string> physics_layers_name(const Physics::LayerTable &layers, std::uint32_t index) {
        return std::make_unique<std::string>(layers.name(index).cpp_string_view());
    }
    std::uint32_t physics_layers_count(const Physics::LayerTable &layers) noexcept { return layers.count(); }
    std::uint64_t physics_layers_mask_of(const Physics::LayerTable &layers, const std::vector<std::string> &names) {
        std::vector<UString> list;
        list.reserve(names.size());
        for (const std::string &name : names) {
            list.push_back(to_ustring(name));
        }
        return layers.mask_of(list);
    }
    std::uint64_t physics_layers_all(const Physics::LayerTable &layers) noexcept { return layers.all(); }
    void physics_layers_set_collides(Physics::LayerTable &layers, std::uint32_t a, std::uint32_t b, bool collides) noexcept { layers.set_collides(a, b, collides); }
    bool physics_layers_collides(const Physics::LayerTable &layers, std::uint32_t a, std::uint32_t b) noexcept {
        return a < layers.count() && b < layers.count() && layers.collides(a, b);
    }
    std::uint64_t physics_layers_collision_mask(const Physics::LayerTable &layers, std::uint32_t layer) noexcept {
        return layer < layers.count() ? layers.collision_mask(layer) : 0;
    }

    // ---- collision world + character ----

    std::unique_ptr<Physics::SimpleWorld> physics_world_new() { return std::make_unique<Physics::SimpleWorld>(); }
    std::uint32_t physics_world_add_box(Physics::SimpleWorld &world, const PhysicsBox &shape) {
        const auto index = static_cast<std::uint32_t>(world.box_count());
        Physics::SimpleWorld::Box native;
        native.min = v3(shape.min);
        native.max = v3(shape.max);
        native.object = shape.object;
        native.velocity = v3(shape.velocity);
        world.add_box(native);
        return index;
    }
    void physics_world_add_plane(Physics::SimpleWorld &world, const PhysicsPlane &plane) {
        Physics::SimpleWorld::Plane native;
        native.normal = direction_or(v3(plane.normal), glm::vec3{0.0f, 1.0f, 0.0f});
        native.offset = plane.offset;
        native.object = plane.object;
        world.add_plane(native);
    }
    std::uint32_t physics_world_box_count(const Physics::SimpleWorld &world) noexcept { return static_cast<std::uint32_t>(world.box_count()); }
    void physics_world_move_box(Physics::SimpleWorld &world, std::uint32_t index, std::array<float, 3> delta) noexcept { world.move_box(index, to_vec3(delta)); }
    bool physics_world_overlaps_capsule(const Physics::SimpleWorld &world, std::array<float, 3> feet, float radius, float height) {
        return world.overlaps_capsule(to_vec3(feet), radius, height);
    }
    bool physics_world_sweep_capsule(const Physics::SimpleWorld &world, std::array<float, 3> feet, float radius, float height, std::array<float, 3> delta,
                                     PhysicsSweepHit &hit) {
        Physics::SweepHit native;
        if (!world.sweep_capsule(to_vec3(feet), radius, height, to_vec3(delta), native)) {
            return false;
        }
        hit.fraction = native.fraction;
        put(hit.normal, native.normal);
        put(hit.point, native.point);
        hit.object = native.object;
        put(hit.object_velocity, native.object_velocity);
        return true;
    }
    std::unique_ptr<Physics::CharacterController> physics_character_new(const PhysicsCharacterSettings &settings, std::array<float, 3> feet) {
        Physics::CharacterSettings native;
        native.radius = settings.radius;
        native.standing_height = settings.standing_height;
        native.crouching_height = settings.crouching_height;
        native.up = direction_or(v3(settings.up), glm::vec3{0.0f, 1.0f, 0.0f});
        native.gravity = settings.gravity;
        native.slope_limit_degrees = settings.slope_limit_degrees;
        native.step_height = settings.step_height;
        native.snap_distance = settings.snap_distance;
        native.skin_width = settings.skin_width;
        native.max_slides = settings.max_slides;
        native.jump_speed = settings.jump_speed;
        native.terminal_speed = settings.terminal_speed;
        return std::make_unique<Physics::CharacterController>(native, to_vec3(feet));
    }
    PhysicsMoveResult physics_character_move(Physics::CharacterController &character, const Physics::SimpleWorld &world, std::array<float, 3> desired_velocity,
                                             float dt, bool jump) {
        const Physics::MoveResult r = character.move(world, to_vec3(desired_velocity), std::isfinite(dt) && dt > 0.0f ? dt : 0.0f, jump);
        PhysicsMoveResult out;
        put(out.displacement, r.displacement);
        out.grounded = r.grounded;
        out.hit_ceiling = r.hit_ceiling;
        out.stepped_up = r.stepped_up;
        out.jumped = r.jumped;
        out.landed = r.landed;
        out.landing_speed = r.landing_speed;
        return out;
    }
    bool physics_character_set_crouching(Physics::CharacterController &character, const Physics::SimpleWorld &world, bool crouching) {
        return character.set_crouching(world, crouching);
    }
    void physics_character_teleport(Physics::CharacterController &character, std::array<float, 3> feet) noexcept { character.teleport(to_vec3(feet)); }
    void physics_character_add_impulse(Physics::CharacterController &character, std::array<float, 3> velocity_change) noexcept {
        character.add_impulse(to_vec3(velocity_change));
    }
    PhysicsCharacterState physics_character_state(const Physics::CharacterController &c) noexcept {
        PhysicsCharacterState out;
        put(out.position, c.position());
        put(out.velocity, c.velocity());
        put(out.ground_normal, c.ground_normal());
        out.ground_object = c.ground_object();
        out.height = c.height();
        out.grounded = c.grounded();
        out.crouching = c.crouching();
        return out;
    }

    // ---- contact events ----

    std::unique_ptr<Physics::ContactEventFilter> physics_contacts_new(const PhysicsContactConfig &config) {
        return std::make_unique<Physics::ContactEventFilter>(to_native(config));
    }
    void physics_contacts_set_config(Physics::ContactEventFilter &filter, const PhysicsContactConfig &config) { filter.set_config(to_native(config)); }
    void physics_contacts_begin_frame(Physics::ContactEventFilter &filter, double time_seconds) { filter.begin_frame(time_seconds); }
    bool physics_contacts_submit(Physics::ContactEventFilter &filter, const PhysicsContactPoint &c) {
        Physics::ContactPoint native;
        native.body_a = c.body_a;
        native.body_b = c.body_b;
        native.point = v3(c.point);
        native.normal = v3(c.normal);
        native.normal_impulse = c.normal_impulse;
        native.approach_speed = c.approach_speed;
        native.slide_speed = c.slide_speed;
        native.material_a = c.material_a;
        native.material_b = c.material_b;
        return filter.submit(native);
    }
    std::unique_ptr<std::vector<PhysicsContactEvent>> physics_contacts_events(const Physics::ContactEventFilter &filter) {
        auto out = std::make_unique<std::vector<PhysicsContactEvent>>();
        for (const Physics::ContactEvent &event : filter.events()) {
            PhysicsContactEvent pod;
            pod.contact = to_pod(event.contact);
            pod.scrape = event.kind == Physics::ContactKind::Scrape;
            pod.intensity = event.intensity;
            out->push_back(pod);
        }
        return out;
    }

    // ---- surfaces + ballistics ----

    std::unique_ptr<Physics::SurfaceTable> physics_surfaces_new() { return std::make_unique<Physics::SurfaceTable>(); }
    std::uint32_t physics_surfaces_add(Physics::SurfaceTable &surfaces, const std::string &name, const PhysicsSurfaceMaterial &material) {
        return surfaces.add(to_native(name, material));
    }
    bool physics_surfaces_find(const Physics::SurfaceTable &surfaces, const std::string &name, std::uint32_t &id) {
        const Physics::MaterialId found = surfaces.find(ustr{std::string_view{name}});
        if (found == Physics::no_material) {
            return false;
        }
        id = found;
        return true;
    }
    std::uint32_t physics_surfaces_count(const Physics::SurfaceTable &surfaces) noexcept { return static_cast<std::uint32_t>(surfaces.size()); }
    bool physics_surfaces_get(const Physics::SurfaceTable &surfaces, std::uint32_t id, PhysicsSurfaceMaterial &material) {
        const Physics::SurfaceMaterial *native = surfaces.get(id);
        if (native == nullptr) {
            return false;
        }
        material.penetration_resistance = native->penetration_resistance;
        material.ricochet_angle_degrees = native->ricochet_angle_degrees;
        material.ricochet_energy_retained = native->ricochet_energy_retained;
        material.ricochet_scatter = native->ricochet_scatter;
        material.exit_deflection = native->exit_deflection;
        return true;
    }
    std::unique_ptr<std::string> physics_surfaces_name(const Physics::SurfaceTable &surfaces, std::uint32_t id) {
        const Physics::SurfaceMaterial *native = surfaces.get(id);
        return std::make_unique<std::string>(native != nullptr ? std::string{native->name.cpp_string_view()} : std::string{});
    }
    PhysicsImpactResult physics_resolve_impact(const PhysicsProjectile &projectile, const PhysicsSurfaceMaterial &material, std::array<float, 3> entry_point,
                                               std::array<float, 3> normal, float thickness, std::array<float, 2> random) {
        return to_pod(Physics::resolve_impact(to_native(projectile), to_native(std::string{}, material), to_vec3(entry_point),
                                              direction_or(to_vec3(normal), glm::vec3{0.0f, 1.0f, 0.0f}), thickness, to_vec2(random)));
    }

    std::unique_ptr<std::vector<PhysicsShotImpact>> physics_trace_shot(const Physics::SurfaceTable &surfaces, const PhysicsProjectile &projectile,
                                                                       const PhysicsShotOptions &options, PhysicsRayQuery &query) {
        Physics::ShotOptions native;
        native.max_distance = options.max_distance;
        native.max_impacts = options.max_impacts;
        native.minimum_energy = options.minimum_energy;
        native.seed = options.seed;
        const Physics::RayQuery ray = [&query](const glm::vec3 &origin, const glm::vec3 &direction, float max_distance, Physics::SurfaceHit &hit) {
            PhysicsSurfaceHit pod;
            if (!query.cast(to_array(origin), to_array(direction), max_distance, pod)) {
                return false;
            }
            hit.point = v3(pod.point);
            hit.normal = v3(pod.normal);
            hit.distance = pod.distance;
            hit.thickness = pod.thickness;
            hit.material = pod.material;
            hit.object = pod.object;
            return true;
        };
        auto out = std::make_unique<std::vector<PhysicsShotImpact>>();
        for (const Physics::ShotImpact &impact : Physics::trace_shot(to_native(projectile), surfaces, ray, native)) {
            PhysicsShotImpact pod;
            put(pod.hit.point, impact.hit.point);
            put(pod.hit.normal, impact.hit.normal);
            pod.hit.distance = impact.hit.distance;
            pod.hit.thickness = impact.hit.thickness;
            pod.hit.material = impact.hit.material;
            pod.hit.object = impact.hit.object;
            pod.result = to_pod(impact.result);
            out->push_back(pod);
        }
        return out;
    }

    // ---- cloth ----

    std::unique_ptr<Physics::Cloth> physics_cloth_grid(std::uint32_t columns, std::uint32_t rows, float spacing, std::array<float, 3> origin,
                                                       std::array<float, 3> right, std::array<float, 3> down, float total_mass) {
        if (columns < 2 || rows < 2) {
            throw Error("a cloth grid needs at least 2 columns and 2 rows");
        }
        if (static_cast<std::uint64_t>(columns) * rows > max_cloth_particles) {
            throw Error("the cloth grid is too large");
        }
        if (!(spacing > 0.0f) || !std::isfinite(spacing) || !(total_mass > 0.0f) || !std::isfinite(total_mass)) {
            throw Error("spacing and total_mass must be finite and positive");
        }
        return std::make_unique<Physics::Cloth>(Physics::Cloth::grid(columns, rows, spacing, to_vec3(origin), to_vec3(right), to_vec3(down), total_mass));
    }
    std::unique_ptr<Physics::Cloth> physics_cloth_from_triangles(const std::vector<float> &positions_xyz, const std::vector<std::uint32_t> &indices,
                                                                  float total_mass) {
        const std::size_t vertex_count = positions_xyz.size() / 3;
        if (vertex_count == 0 || indices.empty() || indices.size() % 3 != 0) {
            throw Error("a cloth mesh needs vertices and a multiple of 3 indices");
        }
        if (vertex_count > max_cloth_particles) {
            throw Error("the cloth mesh is too large");
        }
        if (!(total_mass > 0.0f) || !std::isfinite(total_mass)) {
            throw Error("total_mass must be finite and positive");
        }
        if (std::any_of(indices.begin(), indices.end(), [vertex_count](std::uint32_t i) { return i >= vertex_count; })) {
            throw Error("an index refers past the last vertex");
        }
        std::vector<glm::vec3> positions;
        positions.reserve(vertex_count);
        for (std::size_t i = 0; i < vertex_count; ++i) {
            positions.emplace_back(positions_xyz[i * 3], positions_xyz[i * 3 + 1], positions_xyz[i * 3 + 2]);
        }
        return std::make_unique<Physics::Cloth>(Physics::Cloth::from_triangles(positions, indices, total_mass));
    }
    std::uint32_t physics_cloth_particle_count(const Physics::Cloth &cloth) noexcept { return static_cast<std::uint32_t>(cloth.particle_count()); }
    std::uint32_t physics_cloth_stretch_constraints(const Physics::Cloth &cloth) noexcept { return static_cast<std::uint32_t>(cloth.stretch_constraint_count()); }
    std::uint32_t physics_cloth_bend_constraints(const Physics::Cloth &cloth) noexcept { return static_cast<std::uint32_t>(cloth.bend_constraint_count()); }
    std::unique_ptr<std::vector<float>> physics_cloth_positions(const Physics::Cloth &cloth) {
        auto out = std::make_unique<std::vector<float>>();
        out->reserve(cloth.particle_count() * 3);
        for (const glm::vec3 &p : cloth.positions()) {
            out->insert(out->end(), {p.x, p.y, p.z});
        }
        return out;
    }
    std::unique_ptr<std::vector<std::uint32_t>> physics_cloth_indices(const Physics::Cloth &cloth) {
        const auto indices = cloth.indices();
        return std::make_unique<std::vector<std::uint32_t>>(indices.begin(), indices.end());
    }
    std::unique_ptr<std::vector<float>> physics_cloth_normals(const Physics::Cloth &cloth) {
        std::vector<glm::vec3> normals;
        cloth.compute_normals(normals);
        auto out = std::make_unique<std::vector<float>>();
        out->reserve(normals.size() * 3);
        for (const glm::vec3 &n : normals) {
            out->insert(out->end(), {n.x, n.y, n.z});
        }
        return out;
    }
    void physics_cloth_pin(Physics::Cloth &cloth, std::uint32_t particle, bool pinned) noexcept {
        if (particle < cloth.particle_count()) {
            cloth.pin(particle, pinned);
        }
    }
    bool physics_cloth_is_pinned(const Physics::Cloth &cloth, std::uint32_t particle) noexcept { return particle < cloth.particle_count() && cloth.is_pinned(particle); }
    void physics_cloth_move_pinned(Physics::Cloth &cloth, std::uint32_t particle, std::array<float, 3> position) noexcept {
        if (particle < cloth.particle_count()) {
            cloth.move_pinned(particle, to_vec3(position));
        }
    }
    void physics_cloth_step(Physics::Cloth &cloth, float dt, const PhysicsClothParams &params) {
        cloth.step(std::isfinite(dt) && dt > 0.0f ? dt : 0.0f, to_native(params));
    }
    void physics_cloth_step_colliders(Physics::Cloth &cloth, float dt, const PhysicsClothParams &params, const std::vector<PhysicsClothSphere> &spheres,
                                      const std::vector<PhysicsClothCapsule> &capsules, const std::vector<PhysicsClothPlane> &planes) {
        std::vector<Physics::ClothSphere> sphere_list;
        std::vector<Physics::ClothCapsule> capsule_list;
        std::vector<Physics::ClothPlane> plane_list;
        for (const PhysicsClothSphere &s : spheres) {
            sphere_list.push_back({v3(s.center), s.radius});
        }
        for (const PhysicsClothCapsule &c : capsules) {
            capsule_list.push_back({v3(c.a), v3(c.b), c.radius});
        }
        for (const PhysicsClothPlane &p : planes) {
            plane_list.push_back({direction_or(v3(p.normal), glm::vec3{0.0f, 1.0f, 0.0f}), p.offset});
        }
        cloth.step(std::isfinite(dt) && dt > 0.0f ? dt : 0.0f, to_native(params), Physics::ClothColliders{sphere_list, capsule_list, plane_list});
    }

    // ---- swing / twist ----

    PhysicsSwingTwist physics_decompose_swing_twist(std::array<float, 4> rotation, std::array<float, 3> axis) {
        const Physics::SwingTwist split = Physics::decompose_swing_twist(unit(rotation), direction_or(to_vec3(axis), glm::vec3{1.0f, 0.0f, 0.0f}));
        PhysicsSwingTwist out;
        store(out.swing, split.swing);
        store(out.twist, split.twist);
        return out;
    }
    float physics_twist_angle(std::array<float, 4> rotation, std::array<float, 3> axis) {
        return Physics::twist_angle(unit(rotation), direction_or(to_vec3(axis), glm::vec3{1.0f, 0.0f, 0.0f}));
    }
    bool physics_within_limits(std::array<float, 4> rotation, const PhysicsSwingTwistLimits &limits) { return Physics::within_limits(unit(rotation), to_native(limits)); }
    std::array<float, 4> physics_clamp_to_limits(std::array<float, 4> rotation, const PhysicsSwingTwistLimits &limits) {
        return to_array(Physics::clamp_to_limits(unit(rotation), to_native(limits)));
    }
    std::array<float, 3> physics_pd_drive(std::array<float, 4> current, std::array<float, 4> target, std::array<float, 3> angular_velocity, float stiffness,
                                          float damping) {
        return to_array(Physics::pd_drive(unit(current), unit(target), to_vec3(angular_velocity), stiffness, damping));
    }

    // ---- defaults ----

    PhysicsCharacterSettings physics_character_settings_defaults() noexcept { return {}; }
    PhysicsContactConfig physics_contact_config_defaults() noexcept { return {}; }
    PhysicsSurfaceMaterial physics_surface_material_defaults() noexcept { return {}; }
    PhysicsShotOptions physics_shot_options_defaults() noexcept { return {}; }
    PhysicsClothParams physics_cloth_params_defaults() noexcept { return {}; }
    PhysicsSwingTwistLimits physics_swing_twist_limits_defaults() noexcept { return {}; }

} // namespace SFT::CxxApi
