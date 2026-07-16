#include <Foundation/Foundation.hpp>

#include <Async/Scheduler.hpp>
#include <Ecs/System.hpp>
#include <Ecs/World.hpp>

#include <cassert>
#include <cstdio>

using SFT::Ecs::Entity;
using SFT::Ecs::Query;
using SFT::Ecs::Schedule;
using SFT::Ecs::World;

namespace {

    struct Position {
        f32 x = 0.0f;
        f32 y = 0.0f;
    };

    struct Velocity {
        f32 x = 0.0f;
        f32 y = 0.0f;
    };

    struct Health {
        i32 value = 0;
    };

} // namespace

int main() {
    SFT::Async::Scheduler::initialize(2);

    World world;

    const Entity a = world.spawn(Position{0.0f, 0.0f}, Velocity{1.0f, 2.0f});
    const Entity b = world.spawn(Position{10.0f, 10.0f}, Velocity{-1.0f, 0.0f});
    const Entity c = world.spawn(Position{5.0f, 5.0f}, Velocity{0.0f, 0.0f}, Health{100});

    assert(world.is_alive(a));
    assert(world.is_alive(b));
    assert(world.is_alive(c));

    Position *pos_a = world.get_component<Position>(a);
    assert(pos_a != nullptr && pos_a->x == 0.0f && pos_a->y == 0.0f);
    assert(world.get_component<Health>(a) == nullptr); // a's archetype has no Health column
    assert(world.get_component<Health>(c) != nullptr);

    // Destroy b and confirm swap-remove kept a's row intact and generation-checking invalidated b.
    world.destroy(b);
    assert(!world.is_alive(b));
    assert(world.get_component<Position>(b) == nullptr);
    pos_a = world.get_component<Position>(a);
    assert(pos_a != nullptr && pos_a->x == 0.0f && pos_a->y == 0.0f);

    // Respawning must recycle b's freed index with a bumped generation, so the old handle stays dead.
    const Entity d = world.spawn(Position{1.0f, 1.0f}, Velocity{2.0f, 2.0f});
    assert(d.index == b.index);
    assert(d.generation != b.generation);
    assert(!world.is_alive(b));
    assert(world.is_alive(d));

    // Query + mutate: integrate Position by Velocity over every entity that has both.
    for (auto [entity, position, velocity] : world.query<Position, const Velocity>()) {
        (void)entity;
        position.x += velocity.x;
        position.y += velocity.y;
    }
    pos_a = world.get_component<Position>(a);
    assert(pos_a != nullptr && pos_a->x == 1.0f && pos_a->y == 2.0f);

    // Schedule: a Position/Velocity system and a disjoint Health-only system should be able to run
    // concurrently (same stage); Schedule::run must still produce correct results either way.
    Schedule schedule;
    i32 integrate_runs = 0;
    schedule.add_system([&integrate_runs](World &, Query<Position, const Velocity> query) {
        for (auto [entity, position, velocity] : query) {
            (void)entity;
            position.x += velocity.x;
            position.y += velocity.y;
        }
        ++integrate_runs;
    });
    i32 health_runs = 0;
    schedule.add_system([&health_runs](World &, Query<const Health> query) {
        for (auto [entity, health] : query) {
            (void)entity;
            (void)health;
        }
        ++health_runs;
    });

    schedule.run(world);
    assert(integrate_runs == 1);
    assert(health_runs == 1);
    pos_a = world.get_component<Position>(a);
    assert(pos_a != nullptr && pos_a->x == 2.0f && pos_a->y == 4.0f);

    SFT::Async::Scheduler::shutdown();

    std::printf("Ecs smoke test passed.\n");
    return 0;
}
