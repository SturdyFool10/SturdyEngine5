#include <Physics/Ballistics.hpp>
#include <Physics/CharacterController.hpp>
#include <Physics/Cloth.hpp>
#include <Physics/ContactEvents.hpp>
#include <Physics/FixedStepper.hpp>
#include <Physics/Layers.hpp>
#include <Physics/Ragdoll.hpp>
#include <Physics/SimpleWorld.hpp>
#include <Physics/SwingTwist.hpp>

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <iostream>

using namespace SFT::Physics;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    }
    bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
} // namespace

int main() {
    // ---- fixed stepper --------------------------------------------------------------------------
    {
        FixedStepper s(0.01, 5);
        check(s.advance(0.025) == 2 && near(static_cast<float>(s.alpha()), 0.5f), "two steps and half a step left over");
        check(s.advance(0.005) == 1 && near(static_cast<float>(s.alpha()), 0.0f, 1e-3f), "leftover completes a step");
        check(s.advance(10.0) == 5 && s.dropped_seconds() > 9.0, "a huge frame is clamped to the step budget");
        check(s.advance(-1.0) == 0, "negative delta adds nothing");
        const BodyPose a{{0, 0, 0}, glm::quat(1, 0, 0, 0)};
        const BodyPose b{{2, 0, 0}, glm::angleAxis(glm::pi<float>() / 2, glm::vec3(0, 1, 0))};
        const BodyPose mid = interpolate(a, b, 0.5f);
        check(near(mid.position.x, 1.0f) && near(glm::degrees(glm::angle(mid.rotation)), 45.0f, 0.1f), "pose interpolation");
    }

    // ---- layers ---------------------------------------------------------------------------------
    {
        LayerTable t;
        const auto world = t.add("World"), player = t.add("Player"), debris = t.add("Debris");
        check(world && player && debris && *world == 0 && *debris == 2, "layers registered in order");
        check(t.add("Player") == player, "adding an existing name is idempotent");
        check(t.collides(*player, *debris), "new layers collide with everything");
        t.set_collides(*player, *debris, false);
        check(!t.collides(*player, *debris) && !t.collides(*debris, *player), "collision matrix is symmetric");
        check((t.collision_mask(*player) & LayerTable::bit(*debris)) == 0 && (t.collision_mask(*player) & LayerTable::bit(*world)) != 0,
              "collision mask reflects the matrix");
    }

    // ---- ballistics -----------------------------------------------------------------------------
    {
        SurfaceMaterial wood{.name = "wood", .penetration_resistance = 2000.0f, .ricochet_angle_degrees = 8.0f};
        const Projectile bullet{{0, 0, 0}, {0, 0, -1}, 3000.0f};
        const ImpactResult through = resolve_impact(bullet, wood, {0, 0, -5}, {0, 0, 1}, 0.5f, {0.5f, 0.5f});
        check(through.outcome == ImpactOutcome::Penetrated && near(through.remaining_energy, 3000.0f - 1000.0f, 1.0f),
              "thin wood is penetrated and costs resistance * thickness");
        const ImpactResult stopped = resolve_impact(bullet, wood, {0, 0, -5}, {0, 0, 1}, 2.0f, {0.5f, 0.5f});
        check(stopped.outcome == ImpactOutcome::Stopped && near(stopped.depth, 1.5f), "thick wood stops the round at its maximum depth");
        const Projectile grazing{{0, 0, 0}, glm::normalize(glm::vec3(0.0f, -0.05f, -1.0f)), 3000.0f};
        const ImpactResult skip = resolve_impact(grazing, wood, {0, 0, -5}, {0, 1, 0}, 0.5f, {0.2f, 0.7f});
        check(skip.outcome == ImpactOutcome::Ricocheted && skip.direction.y > 0.0f && skip.remaining_energy < 3000.0f,
              "a grazing hit ricochets away from the surface and loses energy");

        SurfaceTable table;
        const MaterialId wood_id = table.add(wood);
        check(table.find("wood") == wood_id && table.get(wood_id) != nullptr, "surface table lookup");
        // Two wooden walls one metre apart, each 0.2 m thick; the shot goes through both.
        const RayQuery query = [&](const glm::vec3 &origin, const glm::vec3 &direction, float max_distance, SurfaceHit &hit) {
            for (float wall_z : {-5.0f, -6.0f}) {
                const float t = (wall_z - origin.z) / direction.z;
                if (t > 0.0f && t <= max_distance) {
                    hit = SurfaceHit{origin + direction * t, {0, 0, 1}, t, 0.2f, wood_id, 1};
                    return true;
                }
            }
            return false;
        };
        const auto shot = trace_shot(bullet, table, query);
        check(shot.size() == 2 && shot[0].result.outcome == ImpactOutcome::Penetrated && shot[1].result.outcome == ImpactOutcome::Penetrated,
              "a shot penetrates two walls in sequence");
        check(shot.size() == 2 && shot[1].result.remaining_energy < shot[0].result.remaining_energy, "energy falls with each wall");
    }

    // ---- swing / twist --------------------------------------------------------------------------
    {
        const glm::vec3 x(1, 0, 0);
        const glm::quat twist = glm::angleAxis(1.0f, x);
        const glm::quat swing = glm::angleAxis(0.4f, glm::vec3(0, 1, 0));
        const SwingTwist split = decompose_swing_twist(swing * twist, x);
        check(near(twist_angle(split.twist, x), 1.0f, 1e-3f), "twist angle recovered");
        SwingTwistLimits limits;
        limits.axis = x;
        limits.twist_min = -0.5f;
        limits.twist_max = 0.5f;
        limits.swing_y = 0.8f;
        limits.swing_z = 0.8f;
        check(!within_limits(twist, limits), "a 1 rad twist violates +/-0.5");
        check(near(twist_angle(clamp_to_limits(twist, limits), x), 0.5f, 1e-3f), "twist clamps to the limit");
        check(within_limits(glm::angleAxis(0.2f, glm::vec3(0, 1, 0)), limits), "a small swing is inside the cone");
        const glm::quat big = glm::angleAxis(1.5f, glm::vec3(0, 1, 0));
        check(!within_limits(big, limits) && glm::angle(clamp_to_limits(big, limits)) <= 0.8f + 1e-2f, "a large swing is pulled back to the cone edge");
        const glm::vec3 drive = pd_drive(glm::quat(1, 0, 0, 0), glm::angleAxis(0.5f, glm::vec3(0, 0, 1)), glm::vec3(0.0f), 10.0f, 1.0f);
        check(drive.z > 4.9f && drive.z < 5.1f, "PD drive is stiffness times the angle error");
    }

    // ---- cloth ----------------------------------------------------------------------------------
    {
        Cloth cloth = Cloth::grid(8, 8, 0.1f, {0, 2, 0}, {1, 0, 0}, {0, -1, 0}, 1.0f);
        for (SFT::u32 x = 0; x < 8; ++x) cloth.pin(x, true);
        check(cloth.stretch_constraint_count() > 0 && cloth.bend_constraint_count() > 0, "constraints generated");
        ClothParams params;
        for (int i = 0; i < 120; ++i) cloth.step(1.0f / 60.0f, params);
        const auto positions = cloth.positions();
        check(near(positions[0].y, 2.0f), "pinned particles stay put");
        const float bottom = positions[7 * 8].y;
        check(bottom < 1.6f && bottom > 2.0f - 0.7f * 1.15f, "the hanging sheet falls to about its rest length without stretching much");
        ClothPlane floor{{0, 1, 0}, 1.9f};
        const ClothColliders colliders{{}, {}, std::span<const ClothPlane>(&floor, 1)};
        Cloth sheet = Cloth::grid(6, 6, 0.1f, {0, 2.5f, 0}, {1, 0, 0}, {0, 0, 1}, 0.5f);
        for (int i = 0; i < 240; ++i) sheet.step(1.0f / 60.0f, params, colliders);
        float lowest = 10.0f;
        for (const auto &p : sheet.positions()) lowest = std::min(lowest, p.y);
        check(lowest > 1.9f - 0.01f, "a falling sheet rests on the plane instead of passing through");
    }

    // ---- character controller ---------------------------------------------------------------------
    {
        SimpleWorld world;
        world.add_plane({{0, 1, 0}, 0.0f, 1});
        world.add_box({{2.0f, 0.0f, -2.0f}, {3.0f, 2.0f, 2.0f}, 2}); // a tall wall
        world.add_box({{-3.0f, 0.0f, -2.0f}, {-2.0f, 0.2f, 2.0f}, 3}); // a 0.2 m curb (below the step height)
        CharacterSettings settings;
        CharacterController c(settings, {0.0f, 1.0f, 0.0f});
        for (int i = 0; i < 60; ++i) c.move(world, {0, 0, 0}, 1.0f / 60.0f);
        check(c.grounded() && near(c.position().y, 0.0f, 0.03f), "falls and lands on the floor");
        for (int i = 0; i < 90; ++i) c.move(world, {2.0f, 0, 0}, 1.0f / 60.0f);
        check(c.position().x > 1.0f && c.position().x < 1.75f, "walks forward and is stopped by the wall");
        MoveResult r{};
        for (int i = 0; i < 60; ++i) r = c.move(world, {1.0f, 0, 1.0f}, 1.0f / 60.0f);
        check(c.position().z > 0.5f && c.position().x < 1.75f, "slides along the wall");
        c.teleport({-1.0f, 0.0f, 0.0f});
        bool stepped = false;
        for (int i = 0; i < 120; ++i) {
            r = c.move(world, {-2.0f, 0, 0}, 1.0f / 60.0f);
            stepped = stepped || r.stepped_up;
        }
        check(stepped && c.position().x < -2.5f, "steps up onto a low curb");
        c.teleport({0.0f, 0.0f, 0.0f});
        for (int i = 0; i < 5; ++i) c.move(world, {0, 0, 0}, 1.0f / 60.0f);
        r = c.move(world, {0, 0, 0}, 1.0f / 60.0f, true);
        check(r.jumped && c.position().y > 0.0f, "jump leaves the ground");
        check(c.set_crouching(world, true) && c.height() < settings.standing_height, "crouching shortens the capsule");
        check(!c.set_crouching(world, false), "standing up succeeds with headroom");
    }

    // ---- contact events ---------------------------------------------------------------------------
    {
        ContactEventFilter filter;
        filter.begin_frame(0.0);
        ContactPoint hit;
        hit.body_a = 1;
        hit.body_b = 2;
        hit.normal_impulse = 50.0f;
        hit.approach_speed = 3.0f;
        check(filter.submit(hit) && filter.events().size() == 1, "a real impact is accepted");
        hit.normal_impulse = 80.0f;
        check(filter.submit(hit) && filter.events().size() == 1 && filter.events()[0].contact.normal_impulse == 80.0f,
              "a stronger duplicate in the same spot replaces the first");
        ContactPoint tap;
        tap.body_a = 3;
        tap.body_b = 4;
        tap.normal_impulse = 0.01f;
        check(!filter.submit(tap), "contacts below the thresholds are dropped");
        filter.begin_frame(0.05);
        check(!filter.submit(hit), "the pair cooldown suppresses a repeat");
        filter.begin_frame(0.5);
        check(filter.submit(hit), "after the cooldown the pair can raise events again");
        check(filter.events()[0].intensity > 0.0f && filter.events()[0].intensity <= 1.0f, "intensity is normalised");
    }

    return failures == 0 ? 0 : 1;
}
