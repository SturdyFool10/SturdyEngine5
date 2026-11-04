// The CxxApi physics surface over the engine's standalone physics classes: defaults agree with the engine's, and each family does what its name says
// (stepper, layers, world + character, contact filter, surfaces + shot tracing through a callback, cloth, swing/twist).

#include <CxxApi/Physics.hpp>

#include <CxxApi/Error.hpp>

#include <cmath>
#include <cstdio>

using namespace SFT;
using namespace SFT::CxxApi;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }

    /// A wall across z = -5, 0.2 m thick.
    class WallQuery final : public PhysicsRayQuery {
      public:
        explicit WallQuery(std::uint32_t material) : material_(material) {}
        bool cast(std::array<float, 3> origin, std::array<float, 3> direction, float max_distance, PhysicsSurfaceHit &hit) override {
            if (direction[2] >= -0.001f) {
                return false;
            }
            const float distance = (origin[2] + 5.0f) / -direction[2];
            if (distance < 0.0f || distance > max_distance) {
                return false;
            }
            hit.point[0] = origin[0] + direction[0] * distance;
            hit.point[1] = origin[1] + direction[1] * distance;
            hit.point[2] = -5.0f;
            hit.normal[0] = 0.0f;
            hit.normal[1] = 0.0f;
            hit.normal[2] = 1.0f;
            hit.distance = distance;
            hit.thickness = 0.2f;
            hit.material = material_;
            hit.object = 42;
            return true;
        }

      private:
        std::uint32_t material_;
    };

    bool near(float a, float b, float tolerance = 1e-3f) { return std::fabs(a - b) <= tolerance; }
} // namespace

int main() {
    // The records carry the engine's own defaults.
    {
        const Physics::CharacterSettings engine;
        const PhysicsCharacterSettings pod;
        check(near(pod.radius, engine.radius) && near(pod.standing_height, engine.standing_height) && near(pod.gravity, engine.gravity) &&
                  near(pod.step_height, engine.step_height) && pod.max_slides == engine.max_slides && near(pod.jump_speed, engine.jump_speed),
              "character settings defaults match the engine");
        const Physics::ContactEventConfig contacts;
        const PhysicsContactConfig contact_pod;
        check(near(contact_pod.min_impulse, contacts.min_impulse) && contact_pod.max_events_per_frame == contacts.max_events_per_frame,
              "contact config defaults match the engine");
        const Physics::ClothParams cloth;
        const PhysicsClothParams cloth_pod;
        check(near(cloth_pod.gravity[1], cloth.gravity.y) && cloth_pod.substeps == cloth.substeps && near(cloth_pod.bend_compliance, cloth.bend_compliance, 1e-9f),
              "cloth params defaults match the engine");
        const Physics::ShotOptions shot;
        const PhysicsShotOptions shot_pod;
        check(near(shot_pod.max_distance, shot.max_distance) && shot_pod.max_impacts == shot.max_impacts && shot_pod.seed == shot.seed, "shot option defaults match the engine");
    }

    // Stepper.
    {
        auto stepper = physics_stepper_new(0.0, 0);
        check(std::fabs(physics_stepper_step(*stepper) - 1.0 / 60.0) < 1e-12, "default step is 1/60");
        check(physics_stepper_advance(*stepper, 0.05) == 3, "0.05 s holds three steps");
        check(physics_stepper_advance(*stepper, 10.0) == 8, "a long frame is clamped");
        check(physics_stepper_dropped_seconds(*stepper) > 9.0, "the clamped time is dropped");
        check(physics_stepper_advance(*stepper, -1.0) == 0, "negative time is ignored");
        physics_stepper_reset(*stepper);
        check(physics_stepper_total_steps(*stepper) == 0, "reset clears the count");
        PhysicsPose a, b;
        b.position[0] = 2.0f;
        check(near(physics_interpolate_pose(a, b, 0.5f).position[0], 1.0f), "interpolation lerps position");
    }

    // Layers.
    {
        auto layers = physics_layers_new();
        std::uint32_t world = 99, player = 99, again = 99;
        check(physics_layers_add(*layers, "world", world) && world == 0 && physics_layers_add(*layers, "player", player) && player == 1, "layers get indices");
        check(physics_layers_add(*layers, "world", again) && again == 0, "adding twice returns the same layer");
        std::uint32_t found = 99;
        check(physics_layers_find(*layers, "player", found) && found == 1 && !physics_layers_find(*layers, "none", found), "find works");
        check(*physics_layers_name(*layers, 1) == "player", "the name reads back");
        physics_layers_set_collides(*layers, world, player, false);
        check(!physics_layers_collides(*layers, player, world), "disabling is symmetric");
        check(physics_layers_mask_of(*layers, {"player", "missing"}) == 2 && physics_layers_all(*layers) == 3, "masks");
        check(physics_layers_collision_mask(*layers, 99) == 0 && !physics_layers_collides(*layers, 0, 99), "unregistered layers are inert");
    }

    // World + character.
    {
        auto world = physics_world_new();
        PhysicsPlane floor;
        floor.object = 7;
        physics_world_add_plane(*world, floor);
        const std::uint32_t box = physics_world_add_box(*world, PhysicsBox{});
        check(box == 0 && physics_world_box_count(*world) == 1, "a box is added");
        physics_world_move_box(*world, 9, {1, 0, 0}); // ignored
        PhysicsSweepHit hit;
        check(physics_world_sweep_capsule(*world, {0, 3, 5}, 0.3f, 1.8f, {0, -5, 0}, hit) && hit.object == 7 && hit.fraction < 1.0f, "a downward sweep hits the floor");
        check(!physics_world_overlaps_capsule(*world, {0, 3, 5}, 0.3f, 1.8f), "nothing overlaps in the air");

        auto character = physics_character_new(PhysicsCharacterSettings{}, {0, 2, 5});
        bool landed = false;
        for (int i = 0; i < 120 && !landed; ++i) {
            landed = physics_character_move(*character, *world, {0, 0, 0}, 1.0f / 60.0f, false).landed;
        }
        check(landed, "the character lands");
        PhysicsCharacterState state = physics_character_state(*character);
        check(state.grounded && state.ground_object == 7 && std::fabs(state.position[1]) < 0.05f, "and stands on the floor");
        physics_character_move(*character, *world, {2, 0, 0}, 0.5f, false);
        check(physics_character_state(*character).position[0] > 0.3f, "walking moves it");
        check(physics_character_move(*character, *world, {0, 0, 0}, 1.0f / 60.0f, true).jumped, "a grounded character jumps");
        physics_character_teleport(*character, {-4, 1, 0});
        check(near(physics_character_state(*character).position[0], -4.0f), "teleport moves it");
        check(physics_character_set_crouching(*character, *world, true), "crouching succeeds");
    }

    // Contacts.
    {
        auto filter = physics_contacts_new(PhysicsContactConfig{});
        physics_contacts_begin_frame(*filter, 1.0);
        PhysicsContactPoint weak;
        weak.body_a = 1;
        weak.body_b = 2;
        weak.normal_impulse = 0.01f;
        weak.approach_speed = 0.01f;
        check(!physics_contacts_submit(*filter, weak), "a feeble contact is dropped");
        PhysicsContactPoint strong = weak;
        strong.normal_impulse = 100.0f;
        strong.approach_speed = 5.0f;
        check(physics_contacts_submit(*filter, strong), "a strong contact becomes an event");
        const auto events = physics_contacts_events(*filter);
        check(events->size() == 1 && !(*events)[0].scrape && (*events)[0].intensity > 0.0f, "the event carries an intensity");
        physics_contacts_begin_frame(*filter, 5.0);
        check(physics_contacts_events(*filter)->empty(), "a new frame clears the events");
    }

    // Surfaces + ballistics through a callback.
    {
        auto surfaces = physics_surfaces_new();
        PhysicsSurfaceMaterial drywall;
        drywall.penetration_resistance = 400.0f;
        PhysicsSurfaceMaterial steel;
        steel.penetration_resistance = 90000.0f;
        const std::uint32_t drywall_id = physics_surfaces_add(*surfaces, "drywall", drywall);
        const std::uint32_t steel_id = physics_surfaces_add(*surfaces, "steel", steel);
        std::uint32_t found = 99;
        check(physics_surfaces_find(*surfaces, "steel", found) && found == steel_id && !physics_surfaces_find(*surfaces, "glass", found), "materials are found by name");
        PhysicsSurfaceMaterial read;
        check(physics_surfaces_get(*surfaces, steel_id, read) && near(read.penetration_resistance, 90000.0f, 1.0f) && !physics_surfaces_get(*surfaces, 99, read),
              "materials read back");
        check(*physics_surfaces_name(*surfaces, drywall_id) == "drywall" && physics_surfaces_count(*surfaces) == 2, "names and count");

        PhysicsProjectile bullet;
        WallQuery through(drywall_id);
        const auto pass = physics_trace_shot(*surfaces, bullet, PhysicsShotOptions{}, through);
        check(!pass->empty() && (*pass)[0].result.outcome == 1 && (*pass)[0].hit.object == 42, "a rifle round goes through drywall");
        WallQuery plate(steel_id);
        const auto stop = physics_trace_shot(*surfaces, bullet, PhysicsShotOptions{}, plate);
        check(!stop->empty() && (*stop)[0].result.outcome == 0, "the same round stops in steel");
        check(physics_resolve_impact(bullet, drywall, {0, 0, -5}, {0, 0, 1}, 0.2f, {0.5f, 0.5f}).outcome == 1, "resolve_impact agrees with the trace");
    }

    // Cloth.
    {
        bool threw = false;
        try {
            (void)physics_cloth_grid(1, 4, 0.1f, {0, 0, 0}, {1, 0, 0}, {0, 0, 1}, 1.0f);
        } catch (const Error &) {
            threw = true;
        }
        check(threw, "a one-column grid throws");
        auto cloth = physics_cloth_grid(5, 5, 0.1f, {0, 1, 0}, {1, 0, 0}, {0, 0, 1}, 1.0f);
        check(physics_cloth_particle_count(*cloth) == 25 && physics_cloth_stretch_constraints(*cloth) > 0 && physics_cloth_bend_constraints(*cloth) > 0, "a 5x5 sheet");
        for (std::uint32_t i = 0; i < 5; ++i) {
            physics_cloth_pin(*cloth, i, true);
        }
        check(physics_cloth_is_pinned(*cloth, 0) && !physics_cloth_is_pinned(*cloth, 999), "pins");
        const auto before = *physics_cloth_positions(*cloth);
        const PhysicsClothParams params;
        for (int i = 0; i < 60; ++i) {
            physics_cloth_step(*cloth, 1.0f / 60.0f, params);
        }
        const auto after = *physics_cloth_positions(*cloth);
        check(near(after[1], before[1]) && after[24 * 3 + 1] < before[24 * 3 + 1] - 0.05f, "pinned particles stay while the free edge sags");
        PhysicsClothPlane plane;
        plane.offset = 0.8f;
        for (int i = 0; i < 120; ++i) {
            physics_cloth_step_colliders(*cloth, 1.0f / 60.0f, params, {}, {}, {plane});
        }
        check((*physics_cloth_positions(*cloth))[24 * 3 + 1] > 0.75f, "a plane holds the cloth up");
        check(physics_cloth_normals(*cloth)->size() == 75 && physics_cloth_indices(*cloth)->size() == 4 * 4 * 6, "normals and indices");
        threw = false;
        try {
            (void)physics_cloth_from_triangles({0, 0, 0, 1, 0, 0, 0, 1, 0}, {0, 1, 3}, 1.0f);
        } catch (const Error &) {
            threw = true;
        }
        check(threw, "an index past the vertices throws");
        check(physics_cloth_particle_count(*physics_cloth_from_triangles({0, 0, 0, 1, 0, 0, 0, 1, 0}, {0, 1, 2}, 1.0f)) == 3, "a triangle mesh becomes cloth");
    }

    // Swing / twist.
    {
        const float half = 0.70710678f;
        const std::array<float, 4> quarter{half, 0, 0, half};
        const auto split = physics_decompose_swing_twist(quarter, {1, 0, 0});
        check(near(std::fabs(split.twist[0]), half) && near(split.swing[3], 1.0f), "a rotation about the axis is all twist");
        check(near(physics_twist_angle(quarter, {1, 0, 0}), 1.5707963f), "twist angle is 90 degrees");
        const PhysicsSwingTwistLimits limits;
        check(!physics_within_limits(quarter, limits) && physics_within_limits(physics_clamp_to_limits(quarter, limits), limits), "clamping brings it inside");
        check(physics_pd_drive({0, 0, 0, 1}, quarter, {0, 0, 0}, 40.0f, 4.0f)[0] > 0.0f, "the drive turns toward the target");
        check(near(physics_twist_angle({0, 0, 0, 0}, {0, 0, 0}), 0.0f), "degenerate input is neutral, not NaN");
    }

    if (failures == 0) {
        std::printf("CxxApiPhysicsTest: passed.\n");
    }
    return failures == 0 ? 0 : 1;
}
