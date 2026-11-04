/// Exercises the physics C ABI (`sturdy_physics_*`) end to end without an engine: the fixed stepper, layers, the analytic world with
/// a character falling onto a floor, contact filtering, surface materials with a shot traced through a wall, cloth hanging from pins,
/// and swing/twist maths. Explicit checks rather than assert(), which vanishes under NDEBUG.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <FFI/Sturdy.h>

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "PhysicsFfiTest: %s\n", description);
            ++failures;
        }
    }

    bool near(float a, float b, float tolerance = 1e-3f) { return std::fabs(a - b) <= tolerance; }

    void test_stepper() {
        SturdyPhysicsStepper stepper{};
        check(sturdy_physics_stepper_create(0.0, 0, &stepper) == STURDY_OK, "a stepper is created with defaults");
        double step = 0.0;
        check(sturdy_physics_stepper_step(stepper, &step) == STURDY_OK && std::fabs(step - 1.0 / 60.0) < 1e-12, "the default step is 1/60");
        uint32_t steps = 0;
        check(sturdy_physics_stepper_advance(stepper, 0.05, &steps) == STURDY_OK && steps == 3, "0.05 s holds three 1/60 steps");
        double alpha = 0.0;
        check(sturdy_physics_stepper_alpha(stepper, &alpha) == STURDY_OK && alpha >= 0.0 && alpha < 1.0, "alpha is a fraction of a step");
        uint64_t total = 0;
        check(sturdy_physics_stepper_total_steps(stepper, &total) == STURDY_OK && total == 3, "total steps count the due steps");
        check(sturdy_physics_stepper_advance(stepper, 10.0, &steps) == STURDY_OK && steps == 8, "one huge frame is clamped to the step cap");
        double dropped = 0.0;
        check(sturdy_physics_stepper_dropped_seconds(stepper, &dropped) == STURDY_OK && dropped > 9.0, "the clamped time is reported as dropped");
        check(sturdy_physics_stepper_advance(stepper, -1.0, &steps) == STURDY_ERROR_INVALID_ARGUMENT, "negative time is refused");
        check(sturdy_physics_stepper_set_step(stepper, 0.0) == STURDY_ERROR_INVALID_ARGUMENT, "a zero step is refused");
        check(sturdy_physics_stepper_reset(stepper) == STURDY_OK, "reset works");
        check(sturdy_physics_stepper_total_steps(stepper, &total) == STURDY_OK && total == 0, "reset clears the step count");
        check(sturdy_physics_stepper_destroy(stepper) == STURDY_OK, "destroy works");
        check(sturdy_physics_stepper_advance(stepper, 0.01, &steps) != STURDY_OK, "a destroyed stepper no longer resolves");
        check(sturdy_physics_stepper_destroy(stepper) != STURDY_OK, "destroying twice is refused");

        SturdyVec3 position{};
        SturdyQuat rotation{};
        check(sturdy_physics_interpolate_pose({0, 0, 0}, {0, 0, 0, 1}, {2, 0, 0}, {0, 0, 0, 1}, 0.5f, &position, &rotation) == STURDY_OK && near(position.x, 1.0f) &&
                  near(rotation.w, 1.0f),
              "interpolation lerps position and keeps an identity rotation");
    }

    void test_layers() {
        SturdyPhysicsLayers layers{};
        check(sturdy_physics_layers_create(&layers) == STURDY_OK, "layers are created");
        uint32_t world = 99, player = 99, again = 99;
        check(sturdy_physics_layers_add(layers, "world", &world) == STURDY_OK && world == 0, "the first layer is index 0");
        check(sturdy_physics_layers_add(layers, "player", &player) == STURDY_OK && player == 1, "the second layer is index 1");
        check(sturdy_physics_layers_add(layers, "world", &again) == STURDY_OK && again == 0, "adding a name twice returns the same layer");
        uint32_t found = 99;
        check(sturdy_physics_layers_find(layers, "player", &found) == STURDY_OK && found == 1, "find returns the index");
        check(sturdy_physics_layers_find(layers, "nothing", &found) == STURDY_ERROR_NOT_AVAILABLE, "an unknown name is not available");
        char name[16] = {};
        size_t length = 0;
        check(sturdy_physics_layers_name(layers, 1, name, sizeof(name), &length) == STURDY_OK && std::strcmp(name, "player") == 0 && length == 7, "the name is returned");
        uint32_t count = 0;
        check(sturdy_physics_layers_count(layers, &count) == STURDY_OK && count == 2, "two layers are registered");
        SturdyBool collides = STURDY_FALSE;
        check(sturdy_physics_layers_collides(layers, world, player, &collides) == STURDY_OK && collides, "new layers collide with everything");
        check(sturdy_physics_layers_set_collides(layers, world, player, STURDY_FALSE) == STURDY_OK, "a pair can be disabled");
        check(sturdy_physics_layers_collides(layers, player, world, &collides) == STURDY_OK && !collides, "disabling is symmetric");
        uint64_t mask = 0;
        check(sturdy_physics_layers_collision_mask(layers, world, &mask) == STURDY_OK && mask == 1, "the world layer now collides only with itself");
        const char *names[] = {"player", "missing"};
        check(sturdy_physics_layers_mask_of(layers, names, 2, &mask) == STURDY_OK && mask == 2, "mask_of ignores unknown names");
        check(sturdy_physics_layers_all(layers, &mask) == STURDY_OK && mask == 3, "all covers both layers");
        check(sturdy_physics_layers_set_collides(layers, 5, 0, STURDY_TRUE) == STURDY_ERROR_INVALID_ARGUMENT, "an unregistered index is refused");
        check(sturdy_physics_layers_destroy(layers) == STURDY_OK, "layers are destroyed");
    }

    void test_character() {
        SturdyPhysicsWorld world{};
        check(sturdy_physics_world_create(&world) == STURDY_OK, "a world is created");
        const SturdyPhysicsPlane floor{{0, 1, 0}, 0.0f, 7};
        check(sturdy_physics_world_add_plane(world, &floor) == STURDY_OK, "a floor plane is added");
        const SturdyPhysicsBox box{{2, 0, -1}, {3, 1, 1}, 9, {0, 0, 0}};
        uint32_t box_index = 99;
        check(sturdy_physics_world_add_box(world, &box, &box_index) == STURDY_OK && box_index == 0, "a box is added");
        check(sturdy_physics_world_move_box(world, 4, {0, 0, 0}) == STURDY_ERROR_INVALID_ARGUMENT, "a bad box index is refused");

        SturdyBool overlaps = STURDY_TRUE;
        check(sturdy_physics_world_overlaps_capsule(world, {0, 3, 0}, 0.3f, 1.8f, &overlaps) == STURDY_OK && !overlaps, "a capsule in the air overlaps nothing");
        SturdyBool hit = STURDY_FALSE;
        SturdyPhysicsSweepHit sweep{};
        check(sturdy_physics_world_sweep_capsule(world, {0, 3, 0}, 0.3f, 1.8f, {0, -5, 0}, &hit, &sweep) == STURDY_OK && hit && sweep.fraction < 1.0f && sweep.object == 7,
              "a downward sweep hits the floor");

        SturdyPhysicsCharacterSettings settings{};
        check(sturdy_physics_character_settings_init(&settings) == STURDY_OK && settings.struct_size == sizeof(settings), "settings initialise");
        SturdyPhysicsCharacter character{};
        settings.struct_size = 1;
        check(sturdy_physics_character_create(&settings, {0, 2, 0}, &character) == STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "a wrong struct size is refused");
        check(sturdy_physics_character_settings_init(&settings) == STURDY_OK, "settings re-initialise");
        check(sturdy_physics_character_create(&settings, {0, 2, 0}, &character) == STURDY_OK, "a character is created");

        SturdyPhysicsMoveResult moved{};
        bool landed = false;
        for (int i = 0; i < 120 && !landed; ++i) {
            check(sturdy_physics_character_move(character, world, {0, 0, 0}, 1.0f / 60.0f, STURDY_FALSE, &moved) == STURDY_OK, "move succeeds");
            landed = moved.landed != 0;
        }
        check(landed, "the character lands on the floor");
        SturdyPhysicsCharacterState state{};
        check(sturdy_physics_character_state(character, &state) == STURDY_OK && state.grounded && near(state.position.y, 0.0f, 0.05f) && state.ground_object == 7,
              "it stands on the floor plane");
        check(sturdy_physics_character_move(character, world, {2, 0, 0}, 0.5f, STURDY_FALSE, &moved) == STURDY_OK, "walking succeeds");
        check(sturdy_physics_character_state(character, &state) == STURDY_OK && state.position.x > 0.3f, "walking moves the character along x");
        check(sturdy_physics_character_move(character, world, {0, 0, 0}, 1.0f / 60.0f, STURDY_TRUE, &moved) == STURDY_OK && moved.jumped, "a grounded character jumps");
        check(sturdy_physics_character_teleport(character, {-4, 1, 0}) == STURDY_OK, "teleport works");
        check(sturdy_physics_character_state(character, &state) == STURDY_OK && near(state.position.x, -4.0f), "teleport moves the feet");
        SturdyBool crouching = STURDY_FALSE;
        check(sturdy_physics_character_set_crouching(character, world, STURDY_TRUE, &crouching) == STURDY_OK && crouching, "crouching succeeds");
        check(sturdy_physics_character_move(SturdyPhysicsCharacter{0}, world, {0, 0, 0}, 0.1f, STURDY_FALSE, &moved) != STURDY_OK, "a null character handle is refused");
        check(sturdy_physics_character_move(character, SturdyPhysicsWorld{character.token}, {0, 0, 0}, 0.1f, STURDY_FALSE, &moved) == STURDY_ERROR_INVALID_HANDLE,
              "a handle of the wrong kind is refused");
        check(sturdy_physics_character_destroy(character) == STURDY_OK, "the character is destroyed");
        check(sturdy_physics_world_destroy(world) == STURDY_OK, "the world is destroyed");
    }

    void test_contacts() {
        SturdyPhysicsContactConfig config{};
        check(sturdy_physics_contact_config_init(&config) == STURDY_OK, "contact config initialises");
        config.max_events_per_frame = 4;
        SturdyPhysicsContactFilter filter{};
        check(sturdy_physics_contact_filter_create(&config, &filter) == STURDY_OK, "a filter is created");
        check(sturdy_physics_contact_filter_begin_frame(filter, 1.0) == STURDY_OK, "a frame begins");
        SturdyPhysicsContactPoint weak{};
        weak.body_a = 1;
        weak.body_b = 2;
        weak.normal = {0, 1, 0};
        weak.normal_impulse = 0.01f;
        weak.approach_speed = 0.01f;
        weak.material_a = weak.material_b = 0xFFFFFFFFu;
        SturdyBool accepted = STURDY_TRUE;
        check(sturdy_physics_contact_filter_submit(filter, &weak, &accepted) == STURDY_OK && !accepted, "a feeble contact is dropped");
        SturdyPhysicsContactPoint strong = weak;
        strong.normal_impulse = 100.0f;
        strong.approach_speed = 5.0f;
        check(sturdy_physics_contact_filter_submit(filter, &strong, &accepted) == STURDY_OK && accepted, "a strong contact becomes an event");
        uint32_t count = 0;
        check(sturdy_physics_contact_filter_event_count(filter, &count) == STURDY_OK && count == 1, "one event is held");
        SturdyPhysicsContactEvent event{};
        check(sturdy_physics_contact_filter_event(filter, 0, &event) == STURDY_OK && event.kind == STURDY_PHYSICS_CONTACT_IMPACT && event.intensity > 0.0f &&
                  event.contact.body_a == 1,
              "the event carries the contact and an intensity");
        check(sturdy_physics_contact_filter_event(filter, 1, &event) == STURDY_ERROR_INVALID_ARGUMENT, "an event index past the end is refused");
        check(sturdy_physics_contact_filter_submit(filter, &strong, &accepted) == STURDY_OK, "resubmitting works");
        check(sturdy_physics_contact_filter_event_count(filter, &count) == STURDY_OK && count == 1, "the pair cooldown stops a second event");
        check(sturdy_physics_contact_filter_begin_frame(filter, 5.0) == STURDY_OK && sturdy_physics_contact_filter_event_count(filter, &count) == STURDY_OK && count == 0,
              "a new frame clears the events");
        config.struct_size = 3;
        check(sturdy_physics_contact_filter_set_config(filter, &config) == STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "a wrong config size is refused");
        check(sturdy_physics_contact_filter_destroy(filter) == STURDY_OK, "the filter is destroyed");
    }

    /// A wall across z = -5, 0.2 m thick, with the given material.
    SturdyBool wall_query(void *user, SturdyVec3 origin, SturdyVec3 direction, float max_distance, SturdyPhysicsSurfaceHit *out_hit) {
        const uint32_t material = *static_cast<uint32_t *>(user);
        if (direction.z >= -0.001f) {
            return STURDY_FALSE;
        }
        const float distance = (origin.z - (-5.0f)) / -direction.z;
        if (distance < 0.0f || distance > max_distance) {
            return STURDY_FALSE;
        }
        out_hit->point = {origin.x + direction.x * distance, origin.y + direction.y * distance, -5.0f};
        out_hit->normal = {0, 0, 1};
        out_hit->distance = distance;
        out_hit->thickness = 0.2f;
        out_hit->material = material;
        out_hit->object = 42;
        return STURDY_TRUE;
    }

    void test_surfaces() {
        SturdyPhysicsSurfaces surfaces{};
        check(sturdy_physics_surfaces_create(&surfaces) == STURDY_OK, "a surface table is created");
        SturdyPhysicsSurfaceMaterial drywall{};
        check(sturdy_physics_surface_material_init(&drywall) == STURDY_OK && drywall.struct_size == sizeof(drywall), "a material initialises");
        drywall.name = "drywall";
        drywall.penetration_resistance = 400.0f;
        uint32_t id = 99;
        check(sturdy_physics_surfaces_add(surfaces, &drywall, &id) == STURDY_OK && id == 0, "drywall is added");
        SturdyPhysicsSurfaceMaterial steel = drywall;
        steel.name = "steel";
        steel.penetration_resistance = 90000.0f;
        uint32_t steel_id = 99;
        check(sturdy_physics_surfaces_add(surfaces, &steel, &steel_id) == STURDY_OK && steel_id == 1, "steel is added");
        uint32_t found = 99;
        check(sturdy_physics_surfaces_find(surfaces, "steel", &found) == STURDY_OK && found == 1, "a material is found by name");
        check(sturdy_physics_surfaces_find(surfaces, "glass", &found) == STURDY_ERROR_NOT_AVAILABLE, "an unknown material is not available");
        SturdyPhysicsSurfaceMaterial read{};
        check(sturdy_physics_surfaces_get(surfaces, 1, &read) == STURDY_OK && near(read.penetration_resistance, 90000.0f, 1.0f), "a material is read back");
        char name[16] = {};
        check(sturdy_physics_surfaces_name(surfaces, 0, name, sizeof(name), nullptr) == STURDY_OK && std::strcmp(name, "drywall") == 0, "the name is read back");

        SturdyPhysicsProjectile bullet{{0, 0, 0}, {0, 0, -1}, 3000.0f};
        SturdyPhysicsShotOptions options{};
        check(sturdy_physics_shot_options_init(&options) == STURDY_OK, "shot options initialise");
        SturdyPhysicsShotImpact impacts[8] = {};
        uint32_t count = 0;
        check(sturdy_physics_trace_shot(surfaces, &bullet, &options, wall_query, &id, impacts, 8, &count) == STURDY_OK && count >= 1 &&
                  impacts[0].result.outcome == STURDY_PHYSICS_IMPACT_PENETRATED && impacts[0].hit.object == 42,
              "a rifle round goes through drywall");
        check(sturdy_physics_trace_shot(surfaces, &bullet, &options, wall_query, &steel_id, impacts, 8, &count) == STURDY_OK && count >= 1 &&
                  impacts[0].result.outcome == STURDY_PHYSICS_IMPACT_STOPPED,
              "the same round stops in steel plate");
        check(sturdy_physics_trace_shot(surfaces, &bullet, &options, wall_query, &id, nullptr, 0, &count) == STURDY_OK && count >= 1, "a null buffer only counts");

        SturdyPhysicsImpactResult result{};
        check(sturdy_physics_resolve_impact(&bullet, &drywall, {0, 0, -5}, {0, 0, 1}, 0.2f, 0.5f, 0.5f, &result) == STURDY_OK &&
                  result.outcome == STURDY_PHYSICS_IMPACT_PENETRATED,
              "resolve_impact agrees with the trace");
        SturdyPhysicsProjectile bad = bullet;
        bad.direction = {0, 0, 0};
        check(sturdy_physics_resolve_impact(&bad, &drywall, {0, 0, -5}, {0, 0, 1}, 0.2f, 0.5f, 0.5f, &result) == STURDY_ERROR_INVALID_ARGUMENT, "a zero direction is refused");
        check(sturdy_physics_surfaces_destroy(surfaces) == STURDY_OK, "the table is destroyed");
    }

    void test_cloth() {
        SturdyPhysicsCloth cloth{};
        check(sturdy_physics_cloth_create_grid(1, 4, 0.1f, {0, 0, 0}, {1, 0, 0}, {0, -1, 0}, 1.0f, &cloth) == STURDY_ERROR_INVALID_ARGUMENT, "a one-column grid is refused");
        check(sturdy_physics_cloth_create_grid(5, 5, 0.1f, {0, 1, 0}, {1, 0, 0}, {0, 0, 1}, 1.0f, &cloth) == STURDY_OK, "a 5x5 sheet is created");
        uint32_t particles = 0;
        check(sturdy_physics_cloth_particle_count(cloth, &particles) == STURDY_OK && particles == 25, "25 particles");
        uint32_t stretch = 0, bend = 0;
        check(sturdy_physics_cloth_constraint_counts(cloth, &stretch, &bend) == STURDY_OK && stretch > 0 && bend > 0, "constraints exist");
        for (uint32_t i = 0; i < 5; ++i) {
            check(sturdy_physics_cloth_pin(cloth, i, STURDY_TRUE) == STURDY_OK, "the top row is pinned");
        }
        SturdyBool pinned = STURDY_FALSE;
        check(sturdy_physics_cloth_is_pinned(cloth, 0, &pinned) == STURDY_OK && pinned, "a pin reads back");
        check(sturdy_physics_cloth_pin(cloth, 25, STURDY_TRUE) == STURDY_ERROR_INVALID_ARGUMENT, "a pin past the end is refused");

        std::vector<SturdyVec3> before(25), after(25);
        uint32_t count = 0;
        check(sturdy_physics_cloth_positions(cloth, before.data(), 25, &count) == STURDY_OK && count == 25, "positions are copied");
        SturdyPhysicsClothParams params{};
        check(sturdy_physics_cloth_params_init(&params) == STURDY_OK, "cloth params initialise");
        for (int i = 0; i < 60; ++i) {
            check(sturdy_physics_cloth_step(cloth, 1.0f / 60.0f, &params, nullptr) == STURDY_OK, "cloth steps");
        }
        check(sturdy_physics_cloth_positions(cloth, after.data(), 25, &count) == STURDY_OK, "positions are copied again");
        check(near(after[0].y, before[0].y) && after[24].y < before[24].y - 0.05f, "pinned particles stay while the free edge sags");

        // A plane under the pinned edge holds the free edge up.
        const SturdyPhysicsClothPlane plane{{0, 1, 0}, 0.8f};
        SturdyPhysicsClothColliders colliders{};
        colliders.planes = &plane;
        colliders.plane_count = 1;
        for (int i = 0; i < 120; ++i) {
            check(sturdy_physics_cloth_step(cloth, 1.0f / 60.0f, &params, &colliders) == STURDY_OK, "cloth steps against a plane");
        }
        check(sturdy_physics_cloth_positions(cloth, after.data(), 25, &count) == STURDY_OK && after[24].y > 0.8f - 0.05f, "the plane holds the cloth up");

        std::vector<SturdyVec3> normals(25);
        check(sturdy_physics_cloth_normals(cloth, normals.data(), 25, &count) == STURDY_OK && count == 25, "normals are computed");
        SturdyVec3 tiny[2];
        check(sturdy_physics_cloth_positions(cloth, tiny, 2, &count) == STURDY_ERROR_BUFFER_TOO_SMALL && count == 25, "a small buffer reports the full count");
        std::vector<uint32_t> indices(128);
        check(sturdy_physics_cloth_indices(cloth, indices.data(), 128, &count) == STURDY_OK && count == 4 * 4 * 6, "a 5x5 grid has 32 triangles");
        check(sturdy_physics_cloth_destroy(cloth) == STURDY_OK, "the cloth is destroyed");

        const SturdyVec3 triangle[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        const uint32_t good[3] = {0, 1, 2};
        const uint32_t bad_indices[3] = {0, 1, 3};
        check(sturdy_physics_cloth_create_from_triangles(triangle, 3, bad_indices, 3, 1.0f, &cloth) == STURDY_ERROR_INVALID_ARGUMENT, "an index past the vertices is refused");
        check(sturdy_physics_cloth_create_from_triangles(triangle, 3, good, 3, 1.0f, &cloth) == STURDY_OK, "a triangle mesh becomes cloth");
        check(sturdy_physics_cloth_destroy(cloth) == STURDY_OK, "the mesh cloth is destroyed");
    }

    void test_swing_twist() {
        // 90 degrees about the x axis, taken as pure twist about x.
        const float half = 0.70710678f;
        const SturdyQuat quarter_turn{half, 0, 0, half};
        SturdyQuat swing{}, twist{};
        check(sturdy_physics_decompose_swing_twist(quarter_turn, {1, 0, 0}, &swing, &twist) == STURDY_OK && near(std::fabs(twist.x), half) && near(swing.w, 1.0f),
              "a rotation about the axis is all twist");
        float angle = 0.0f;
        check(sturdy_physics_twist_angle(quarter_turn, {1, 0, 0}, &angle) == STURDY_OK && near(angle, 1.5707963f), "twist angle is 90 degrees");
        const SturdyPhysicsSwingTwistLimits limits{{1, 0, 0}, 0.8f, 0.8f, -0.5f, 0.5f};
        SturdyBool within = STURDY_TRUE;
        check(sturdy_physics_within_limits(quarter_turn, &limits, &within) == STURDY_OK && !within, "90 degrees of twist is outside +-0.5 rad");
        SturdyQuat clamped{};
        check(sturdy_physics_clamp_to_limits(quarter_turn, &limits, &clamped) == STURDY_OK, "clamping works");
        check(sturdy_physics_within_limits(clamped, &limits, &within) == STURDY_OK && within, "the clamped rotation is legal");
        check(sturdy_physics_twist_angle(quarter_turn, {0, 0, 0}, &angle) == STURDY_ERROR_INVALID_ARGUMENT, "a zero axis is refused");
        SturdyVec3 velocity{};
        check(sturdy_physics_pd_drive({0, 0, 0, 1}, quarter_turn, {0, 0, 0}, 40.0f, 4.0f, &velocity) == STURDY_OK && velocity.x > 0.0f, "the drive turns toward the target");
    }

} // namespace

int main() {
    check(sturdy_abi_version_minor() >= 43, "the ABI version includes physics");
    test_stepper();
    test_layers();
    test_character();
    test_contacts();
    test_surfaces();
    test_cloth();
    test_swing_twist();
    if (failures == 0) {
        std::printf("PhysicsFfiTest: passed.\n");
    }
    return failures == 0 ? 0 : 1;
}
