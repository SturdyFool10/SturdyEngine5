/// Verifies the type-erased `World` API, whose whole reason to exist is that it reports the
/// conditions the templated API terminates on.
///
/// Every "must be rejected" case below is one that `World::spawn`/`add_component`/`get_component`
/// would answer with `std::terminate`. Exercising them here is what proves a foreign caller holding
/// a stale entity cannot take the process down.

#include <cstdio>
#include <cstring>
#include <vector>

#include <Ecs/World.hpp>

namespace {

    int failures = 0;

    /// Records a failed expectation.
    ///
    /// Deliberately not `assert`: these checks must hold in optimized configurations too, and
    /// `assert` compiles to nothing once `NDEBUG` is defined.
    ///
    /// @param condition True when the expectation held.
    /// @param description What was expected, reported when it did not hold.
    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "WorldErasedTest: %s\n", description);
            ++failures;
        }
    }

    struct Position {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct Velocity {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    /// A component the erased API must refuse to memcpy, because copying its bytes would duplicate
    /// an owning pointer without running the copy constructor that makes that safe.
    struct NonTrivial {
        std::vector<int> values;
    };

} // namespace

SFT_ECS_COMPONENT(Position, "test.erased.position");
SFT_ECS_COMPONENT(Velocity, "test.erased.velocity");
SFT_ECS_COMPONENT(NonTrivial, "test.erased.non_trivial");

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    using SFT::Ecs::ComponentId;
    using SFT::Ecs::Entity;
    using SFT::Ecs::World;
    using SFT::Ecs::WorldErasedErrorCode;

    SFT::Ecs::ComponentRegistry registry;
    World world{registry};

    const ComponentId position = registry.component<Position>();
    const ComponentId velocity = registry.component<Velocity>();
    const ComponentId non_trivial = registry.component<NonTrivial>();

    // ── Spawn and read back ────────────────────────────────────────────────────────────────────
    const Position initial_position{1.0f, 2.0f, 3.0f};
    const Velocity initial_velocity{4.0f, 5.0f, 6.0f};
    const ComponentId ids[]{position, velocity};
    const void *const data[]{&initial_position, &initial_velocity};

    const auto spawned = world.spawn_erased(ids, data);
    check(spawned.has_value(), "spawning with two components must succeed");
    const Entity entity = spawned.value_or(Entity{});
    check(world.is_alive(entity), "the spawned entity must be alive");
    check(world.has_component_erased(entity, position), "the entity must carry Position");
    check(world.has_component_erased(entity, velocity), "the entity must carry Velocity");

    Position read_back{};
    check(world.read_component_erased(entity, position, &read_back, sizeof(read_back)).has_value(),
          "reading Position must succeed");
    check(read_back.x == 1.0f && read_back.y == 2.0f && read_back.z == 3.0f,
          "the spawned Position must round-trip byte for byte");

    // Cross-check against the templated accessor: the erased path must observe the same storage
    // the rest of the engine does, not a copy that has drifted.
    {
        const auto borrowed = world.get_component<Position>(entity);
        check(static_cast<bool>(borrowed), "the templated accessor must see the same component");
        check(borrowed->x == 1.0f, "both accessors must agree on the value");
    }

    // ── Write in place ─────────────────────────────────────────────────────────────────────────
    const Position updated{7.0f, 8.0f, 9.0f};
    check(world.write_component_erased(entity, position, &updated, sizeof(updated)).has_value(),
          "writing Position must succeed");
    {
        const auto borrowed = world.get_component<Position>(entity);
        check(borrowed->x == 7.0f && borrowed->z == 9.0f,
              "the templated accessor must observe the erased write");
    }

    // ── Size and type guards ───────────────────────────────────────────────────────────────────
    check(world.read_component_erased(entity, position, &read_back, sizeof(read_back) - 1).error().code ==
              WorldErasedErrorCode::SizeMismatch,
          "a short read buffer must be rejected rather than partially filled");
    check(world.write_component_erased(entity, position, &updated, sizeof(updated) + 4).error().code ==
              WorldErasedErrorCode::SizeMismatch,
          "an oversized write must be rejected");

    NonTrivial owning;
    check(world.add_component_erased(entity, non_trivial, &owning).error().code ==
              WorldErasedErrorCode::NotTriviallyCopyable,
          "a non-trivially-copyable component must not be memcpy'd in");

    // ── Structural changes ─────────────────────────────────────────────────────────────────────
    check(world.add_component_erased(entity, position, &updated).error().code ==
              WorldErasedErrorCode::DuplicateComponent,
          "adding a component the entity already has must be rejected");

    check(world.remove_component_erased(entity, velocity).has_value(),
          "removing Velocity must succeed");
    check(!world.has_component_erased(entity, velocity), "Velocity must be gone");
    check(world.has_component_erased(entity, position), "Position must survive the move");
    {
        Position after_move{};
        check(world.read_component_erased(entity, position, &after_move, sizeof(after_move)).has_value(),
              "Position must still be readable after the archetype move");
        check(after_move.x == 7.0f && after_move.z == 9.0f,
              "the archetype move must preserve component bytes");
    }

    check(world.remove_component_erased(entity, velocity).error().code ==
              WorldErasedErrorCode::MissingComponent,
          "removing an absent component must be rejected");
    check(world.remove_component_erased(entity, position).error().code ==
              WorldErasedErrorCode::NoComponents,
          "removing an entity's last component must be rejected rather than emptying it");

    check(world.add_component_erased(entity, velocity, &initial_velocity).has_value(),
          "re-adding Velocity must succeed");

    // ── Iteration ──────────────────────────────────────────────────────────────────────────────
    const Position other_position{10.0f, 0.0f, 0.0f};
    const Velocity other_velocity{20.0f, 0.0f, 0.0f};
    const void *const other_data[]{&other_position, &other_velocity};
    const auto second = world.spawn_erased(ids, other_data);
    check(second.has_value(), "spawning a second entity must succeed");

    // An entity with Position only, which must not match a Position+Velocity query.
    const ComponentId position_only[]{position};
    const void *const position_only_data[]{&other_position};
    const auto third = world.spawn_erased(position_only, position_only_data);
    check(third.has_value(), "spawning a single-component entity must succeed");

    struct VisitState {
        int count = 0;
        float position_sum = 0.0f;
    };
    VisitState state;

    const auto visited = world.for_each_erased(
        ids,
        [](Entity, void **components, void *user_data) noexcept {
            auto *visit_state = static_cast<VisitState *>(user_data);
            const auto *seen = static_cast<const Position *>(components[0]);
            visit_state->position_sum += seen->x;
            ++visit_state->count;
        },
        &state);

    check(visited.has_value(), "iteration must succeed");
    check(visited.value_or(0) == 2, "only the two Position+Velocity entities must be visited");
    check(state.count == 2, "the visitor must run once per matching entity");
    check(state.position_sum == 17.0f, "the visitor must see the live component values");

    // Iterating on Position alone must reach all three.
    VisitState all;
    const auto visited_all = world.for_each_erased(
        position_only,
        [](Entity, void **, void *user_data) noexcept { ++static_cast<VisitState *>(user_data)->count; },
        &all);
    check(visited_all.value_or(0) == 3, "all three entities carry Position");
    check(all.count == 3, "the visitor must run for every Position holder");

    // Writes through visitor pointers must land in real storage, not a copy.
    const auto mutated = world.for_each_erased(
        position_only,
        [](Entity, void **components, void *) noexcept {
            static_cast<Position *>(components[0])->y = 42.0f;
        },
        nullptr);
    check(mutated.has_value(), "mutating iteration must succeed");
    {
        Position after{};
        check(world.read_component_erased(entity, position, &after, sizeof(after)).has_value(),
              "reading after mutating iteration must succeed");
        check(after.y == 42.0f, "a write through a visitor pointer must reach archetype storage");
    }

    check(world.for_each_erased({}, [](Entity, void **, void *) noexcept {}, nullptr).error().code ==
              WorldErasedErrorCode::NoComponents,
          "iterating with no components must be rejected");
    check(world.for_each_erased(ids, nullptr, nullptr).error().code ==
              WorldErasedErrorCode::InvalidArgument,
          "a null visitor must be rejected");

    // ── Dead entities ──────────────────────────────────────────────────────────────────────────
    // The whole point of this API: every one of these terminates the process through the templated
    // equivalent.
    world.destroy(entity);
    check(!world.is_alive(entity), "the destroyed entity must not be alive");
    check(!world.has_component_erased(entity, position), "a dead entity carries nothing");
    check(world.read_component_erased(entity, position, &read_back, sizeof(read_back)).error().code ==
              WorldErasedErrorCode::DeadEntity,
          "reading from a dead entity must be rejected");
    check(world.write_component_erased(entity, position, &updated, sizeof(updated)).error().code ==
              WorldErasedErrorCode::DeadEntity,
          "writing to a dead entity must be rejected");
    check(world.add_component_erased(entity, non_trivial, &owning).error().code ==
              WorldErasedErrorCode::NotTriviallyCopyable,
          "component-type validation runs before entity validation, so the type error wins here");
    check(world.remove_component_erased(entity, position).error().code ==
              WorldErasedErrorCode::DeadEntity,
          "removing from a dead entity must be rejected");

    // A never-valid entity value, the shape a foreign caller gets from a zeroed struct.
    const Entity never_valid{};
    check(!world.is_alive(never_valid), "a default-constructed entity is not alive");
    check(world.read_component_erased(never_valid, position, &read_back, sizeof(read_back)).error().code ==
              WorldErasedErrorCode::DeadEntity,
          "reading from a default-constructed entity must be rejected");

    // Index recycling: spawning after a destroy reuses the freed index with a bumped generation.
    // The old entity value now names a live row, and only the generation distinguishes them — this
    // is the case where a stale handle from a foreign caller would silently alias someone else's
    // entity if the check were index-only.
    const auto recycled = world.spawn_erased(position_only, position_only_data);
    check(recycled.has_value(), "spawning after a destroy must succeed");
    check(recycled->index == entity.index,
          "the freed index must be recycled, otherwise this case is not being tested");
    check(recycled->generation != entity.generation, "the recycled index must carry a new generation");
    check(world.is_alive(*recycled), "the recycled entity must be alive");
    check(world.read_component_erased(entity, position, &read_back, sizeof(read_back)).error().code ==
              WorldErasedErrorCode::DeadEntity,
          "the stale handle must still be rejected even though its index is live again");
    check(world.read_component_erased(*recycled, position, &read_back, sizeof(read_back)).has_value(),
          "the recycled entity itself must be readable");

    // ── Unknown components ─────────────────────────────────────────────────────────────────────
    const ComponentId unknown = 0xFFFFu;
    check(world.read_component_erased(*second, unknown, &read_back, sizeof(read_back)).error().code ==
              WorldErasedErrorCode::UnknownComponent,
          "an unregistered component id must be rejected");
    check(world.add_component_erased(*second, unknown, &updated).error().code ==
              WorldErasedErrorCode::UnknownComponent,
          "adding an unregistered component must be rejected");

    // ── Spawn argument validation ──────────────────────────────────────────────────────────────
    const ComponentId duplicate_ids[]{position, position};
    const void *const duplicate_data[]{&initial_position, &initial_position};
    check(world.spawn_erased(duplicate_ids, duplicate_data).error().code ==
              WorldErasedErrorCode::DuplicateComponent,
          "spawning with a duplicated component must be rejected");
    check(world.spawn_erased({}, {}).error().code == WorldErasedErrorCode::NoComponents,
          "spawning with no components must be rejected");

    const void *const null_data[]{nullptr};
    check(world.spawn_erased(position_only, null_data).error().code ==
              WorldErasedErrorCode::InvalidArgument,
          "spawning with a null data pointer must be rejected");

    if (failures != 0) {
        (void)std::fprintf(stderr, "WorldErasedTest: %d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
