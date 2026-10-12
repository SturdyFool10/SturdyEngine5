// Per-entity vs per-chunk erased system dispatch over the same data and the same work. Prints ns per entity for each; asserts only that
// both produce identical results (timing assertions would be flaky). Run it to see what a foreign language pays for crossing the
// boundary once per entity instead of once per archetype.

#include <Ecs/System.hpp>
#include <Ecs/World.hpp>

#include <chrono>
#include <cstdio>

namespace {

    struct Vec2 {
        float x = 0.0f;
        float y = 0.0f;
    };
    struct Position : Vec2 {};
    struct Velocity : Vec2 {};

    // Written so the compiler cannot hoist the call: the function pointer is opaque to it, as a foreign callback would be.
    void per_entity(SFT::Ecs::Entity, void **components, void *, void *) noexcept {
        auto *p = static_cast<Position *>(components[0]);
        const auto *v = static_cast<const Velocity *>(components[1]);
        p->x += v->x;
        p->y += v->y;
    }

    void per_chunk(const SFT::Ecs::Entity *, SFT::usize count, void *const *columns, void *, void *) noexcept {
        auto *p = static_cast<Position *>(columns[0]);
        const auto *v = static_cast<const Velocity *>(columns[1]);
        for (SFT::usize i = 0; i < count; ++i) {
            p[i].x += v[i].x;
            p[i].y += v[i].y;
        }
    }

} // namespace

SFT_ECS_COMPONENT(Position, "bench.position");
SFT_ECS_COMPONENT(Velocity, "bench.velocity");

int main() {
    using namespace SFT::Ecs;
    constexpr int entities = 200000;
    constexpr int runs = 20;

    auto build = [&](ComponentRegistry &registry, World &world) {
        for (int i = 0; i < entities; ++i) {
            (void)world.spawn(Position{{static_cast<float>(i), 0.0f}}, Velocity{{1.0f, 2.0f}});
        }
        (void)registry;
    };

    double per_entity_ns = 0.0;
    double per_chunk_ns = 0.0;
    float per_entity_sum = 0.0f;
    float per_chunk_sum = 0.0f;

    for (int mode = 0; mode < 2; ++mode) {
        ComponentRegistry registry;
        World world{registry};
        build(registry, world);
        const ComponentId position = registry.component<Position>();
        const ComponentId velocity = registry.component<Velocity>();
        Schedule schedule{ScheduleConfig{.executor = ExecutorPolicy::Synchronous}};
        SystemAccess access;
        access.writes.push_back(registry.info(position)->key);
        access.reads.push_back(registry.info(velocity)->key);
        if (mode == 0) {
            (void)schedule.add_erased_system(access, {position, velocity}, per_entity, nullptr);
        } else {
            (void)schedule.add_erased_chunk_system(access, {position, velocity}, per_chunk, nullptr);
        }

        schedule.run(world); // warm up
        const auto start = std::chrono::steady_clock::now();
        for (int run = 0; run < runs; ++run) {
            schedule.run(world);
        }
        const auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
        const double ns_per_entity = elapsed / (static_cast<double>(runs) * entities);

        float sum = 0.0f;
        const ComponentId ids[]{position};
        struct Acc {
            float *sum;
        } acc{&sum};
        (void)world.for_each_erased(
            ids, [](Entity, void **components, void *user) noexcept { *static_cast<Acc *>(user)->sum += static_cast<const Position *>(components[0])->x; }, &acc);
        (mode == 0 ? per_entity_ns : per_chunk_ns) = ns_per_entity;
        (mode == 0 ? per_entity_sum : per_chunk_sum) = sum;
    }

    std::printf("ErasedDispatchBenchmark: %d entities x %d runs\n", entities, runs);
    std::printf("  per-entity erased system: %.2f ns/entity\n", per_entity_ns);
    std::printf("  per-chunk  erased system: %.2f ns/entity (%.1fx)\n", per_chunk_ns, per_chunk_ns > 0 ? per_entity_ns / per_chunk_ns : 0.0);

    if (per_entity_sum != per_chunk_sum) {
        std::fprintf(stderr, "ErasedDispatchBenchmark: the two dispatch styles disagree (%f vs %f)\n", per_entity_sum, per_chunk_sum);
        return 1;
    }
    return 0;
}
