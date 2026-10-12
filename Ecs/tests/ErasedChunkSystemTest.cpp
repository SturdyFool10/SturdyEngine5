// Chunk-at-a-time erased systems and visits: one callback per matching archetype, contiguous component arrays parallel to the
// entity array, writes through those arrays land in the live components, and only archetypes carrying every requested
// component are visited.

#include <Ecs/System.hpp>
#include <Ecs/World.hpp>

#include <cstdio>
#include <vector>

namespace {

    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "ErasedChunkSystemTest: %s\n", what);
            ++failures;
        }
    }

    struct Position {
        float x = 0.0f;
        float y = 0.0f;
    };
    struct Velocity {
        float x = 0.0f;
        float y = 0.0f;
    };
    struct Tag {
        int value = 0;
    };

    struct Seen {
        std::vector<std::size_t> counts;
        std::vector<SFT::Ecs::Entity> entities;
    };

    /// Integrates every entity in the chunk: position += velocity, written through the column pointers.
    void integrate_chunk(const SFT::Ecs::Entity *entities, SFT::usize count, void *const *columns, void *, void *user_data) noexcept {
        auto *seen = static_cast<Seen *>(user_data);
        seen->counts.push_back(count);
        auto *positions = static_cast<Position *>(columns[0]);
        const auto *velocities = static_cast<const Velocity *>(columns[1]);
        for (SFT::usize i = 0; i < count; ++i) {
            seen->entities.push_back(entities[i]);
            positions[i].x += velocities[i].x;
            positions[i].y += velocities[i].y;
        }
    }

    void visit_chunk(const SFT::Ecs::Entity *, SFT::usize count, void *const *, void *user_data) noexcept {
        static_cast<Seen *>(user_data)->counts.push_back(count);
    }

} // namespace

SFT_ECS_COMPONENT(Position, "test.chunk.position");
SFT_ECS_COMPONENT(Velocity, "test.chunk.velocity");
SFT_ECS_COMPONENT(Tag, "test.chunk.tag");

int main() {
    using namespace SFT::Ecs;

    ComponentRegistry registry;
    World world{registry};
    const ComponentId position = registry.component<Position>();
    const ComponentId velocity = registry.component<Velocity>();
    const ComponentId tag = registry.component<Tag>();

    // Archetype A = {Position, Velocity}: two entities. Archetype B = {Position, Velocity, Tag}: one entity.
    // Archetype C = {Position}: one entity, which must not be visited by a Position+Velocity system.
    const Entity a1 = world.spawn(Position{1.0f, 1.0f}, Velocity{10.0f, 0.0f});
    const Entity a2 = world.spawn(Position{2.0f, 2.0f}, Velocity{20.0f, 0.0f});
    const Entity b1 = world.spawn(Position{3.0f, 3.0f}, Velocity{30.0f, 0.0f}, Tag{1});
    const Entity c1 = world.spawn(Position{4.0f, 4.0f});

    // ---- a direct chunk visit ----
    {
        Seen seen;
        const ComponentId ids[]{position, velocity};
        const auto visited = world.for_each_erased_chunk(ids, visit_chunk, &seen);
        check(visited && *visited == 3, "the visit reports the three entities that carry both components");
        check(seen.counts.size() == 2, "two archetypes match, so the visitor is called twice, not once per entity");
        std::size_t total = 0;
        for (const std::size_t n : seen.counts) {
            total += n;
        }
        check(total == 3, "the chunk counts add up to the matching entities");
    }

    // ---- validation ----
    {
        Seen seen;
        check(!world.for_each_erased_chunk({}, visit_chunk, &seen) &&
                  world.for_each_erased_chunk({}, visit_chunk, &seen).error().code == WorldErasedErrorCode::NoComponents,
              "a chunk visit with no components is rejected");
        const ComponentId ids[]{position};
        check(!world.for_each_erased_chunk(ids, nullptr, &seen), "a null visitor is rejected");
        const ComponentId unknown[]{static_cast<ComponentId>(9999)};
        check(!world.for_each_erased_chunk(unknown, visit_chunk, &seen), "an unknown component id is rejected");
    }

    // ---- a scheduled chunk system ----
    {
        Seen seen;
        Schedule schedule{ScheduleConfig{.executor = ExecutorPolicy::Synchronous}};
        SystemAccess access;
        access.reads.push_back(registry.info(velocity)->key);
        access.writes.push_back(registry.info(position)->key);
        const SystemHandle handle = schedule.add_erased_chunk_system(access, {position, velocity}, integrate_chunk, &seen);
        check(static_cast<bool>(handle), "a chunk system registers");
        check(!schedule.add_erased_chunk_system(access, {}, integrate_chunk, &seen), "a chunk system needs components");
        check(!schedule.add_erased_chunk_system(access, {position}, nullptr, &seen), "a chunk system needs a body");

        schedule.run(world);

        check(seen.counts.size() == 2, "the scheduled system is called once per matching archetype");
        check(seen.entities.size() == 3, "it saw each of the three matching entities exactly once");

        Position read{};
        check(world.read_component_erased(a1, position, &read, sizeof(read)) && read.x == 11.0f && read.y == 1.0f,
              "a write through the column pointer reaches the live component (first archetype)");
        check(world.read_component_erased(a2, position, &read, sizeof(read)) && read.x == 22.0f, "...for every row of the chunk");
        check(world.read_component_erased(b1, position, &read, sizeof(read)) && read.x == 33.0f, "...and in the other archetype");
        check(world.read_component_erased(c1, position, &read, sizeof(read)) && read.x == 4.0f,
              "an entity without Velocity is untouched");

        // Removing the system stops it.
        check(schedule.remove_system(handle), "the chunk system can be removed");
        seen = {};
        schedule.run(world);
        check(seen.counts.empty(), "a removed chunk system no longer runs");
    }

    (void)tag;
    if (failures != 0) {
        std::fprintf(stderr, "ErasedChunkSystemTest: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("ErasedChunkSystemTest: ok\n");
    return 0;
}
