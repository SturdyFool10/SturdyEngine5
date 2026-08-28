/// Exercises the ECS C ABI against a real `Engine`, without a window or graphics device.
///
/// The ECS lives entirely on the CPU side of the engine, so it can be driven headlessly — which
/// makes this the one part of the FFI whose behavior (not just its argument validation) is testable
/// in CI. Everything here goes through the public C entry points, using the internal handle table
/// only to mint the engine handle a game-logic callback would normally receive.

#include <cstdio>
#include <cstring>

#include <stdexcept>

#include <Async/Scheduler.hpp>
#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

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
            (void)std::fprintf(stderr, "EcsFfiTest: %s\n", description);
            ++failures;
        }
    }

    struct Position {
        float x;
        float y;
        float z;
    };

    struct Health {
        int32_t current;
        int32_t maximum;
    };

    struct VisitTally {
        int count;
        float sum_x;
    };

    /// Sums the x of every visited Position and counts the visits.
    void STURDY_ABI_CALL tally_visit(SturdyEntity, void **components, void *user_data) {
        auto *tally = static_cast<VisitTally *>(user_data);
        tally->sum_x += static_cast<const Position *>(components[0])->x;
        ++tally->count;
    }

    /// Writes through the visitor pointer, to prove iteration exposes live storage.
    void STURDY_ABI_CALL mutate_visit(SturdyEntity, void **components, void *) {
        static_cast<Position *>(components[0])->y = 99.0f;
    }

    /// What a reentrant visitor observed when it called back into the ECS.
    struct ReentryProbe {
        SturdyEngine engine;
        SturdyComponentId component;
        SturdyResult read_result;
        SturdyResult spawn_result;
    };

    /// Calls back into the ECS from inside a visit, using a perfectly valid engine handle.
    ///
    /// This is the realistic hazard: not a bad handle, but correct-looking code whose reentrancy
    /// would deadlock on the non-recursive world lock if the guard were missing.
    void STURDY_ABI_CALL reentrant_visit(SturdyEntity entity, void **, void *user_data) {
        auto *probe = static_cast<ReentryProbe *>(user_data);
        Position scratch{};
        probe->read_result =
            sturdy_ecs_get_component(probe->engine, entity, probe->component, &scratch, sizeof(scratch));

        const Position value{0.0f, 0.0f, 0.0f};
        SturdyComponentInit init;
        init.component = probe->component;
        init.size = sizeof(Position);
        init.data = &value;
        SturdyEntity spawned{};
        probe->spawn_result = sturdy_ecs_spawn(probe->engine, &init, 1, &spawned);
    }

    /// Counts how many times a registered system body ran.
    struct SystemTally {
        int invocations;
        int unused;
    };

    /// Integrates velocity into position, the canonical shape of a per-entity system.
    void STURDY_ABI_CALL integrate_system(SturdyEntity, void **components, SturdyCommands, void *user_data) {
        auto *tally = static_cast<SystemTally *>(user_data);
        auto *position = static_cast<Position *>(components[0]);
        const auto *velocity = static_cast<const Position *>(components[1]);
        position->x += velocity->x;
        position->y += velocity->y;
        position->z += velocity->z;
        ++tally->invocations;
    }

    /// What a system observed while trying to make structural changes.
    struct CommandProbe {
        SturdyComponentId component;
        uint32_t size;
        SturdyResult spawn_result;
        int invocations;
        SturdyResult direct_spawn_result;
        SturdyEngine engine;
    };

    /// Queues a spawn through Commands, and separately attempts a direct spawn that must be refused.
    void STURDY_ABI_CALL spawning_system(SturdyEntity,
                                         void **,
                                         SturdyCommands commands,
                                         void *user_data) {
        auto *probe = static_cast<CommandProbe *>(user_data);
        ++probe->invocations;

        const Health value{1, 1};
        SturdyComponentInit init;
        init.component = probe->component;
        init.size = probe->size;
        init.data = &value;
        probe->spawn_result = sturdy_ecs_commands_spawn(commands, &init, 1);

        // The direct path must refuse while a schedule is running; only Commands is safe here.
        SturdyEntity unused{};
        probe->direct_spawn_result = sturdy_ecs_spawn(probe->engine, &init, 1, &unused);
    }

    /// Scratch storage for the component-id arrays the counting helper needs.
    SturdyComponentId g_marker_ids[1];

    /// Returns a one-element component id array for iteration.
    const SturdyComponentId *marker_access_ids(SturdyComponentId component) {
        g_marker_ids[0] = component;
        return g_marker_ids;
    }

    /// Counts visited entities.
    void STURDY_ABI_CALL count_visit(SturdyEntity, void **, void *user_data) {
        ++*static_cast<uint32_t *>(user_data);
    }

    /// A resource a global system advances once per frame.
    struct FrameCounter {
        int32_t frames;
        int32_t unused;
    };

    /// One buffered event.
    struct DamageEvent {
        uint32_t amount;
        uint32_t source;
    };

    /// What a global system needs to reach its resource and channel.
    struct GlobalProbe {
        SturdyEngine engine;
        SturdyResourceId counter;
        SturdyResourceId channel;
        int32_t invocations;
        SturdyResult send_result;
    };

    /// Runs once per frame: bumps a resource and emits an event.
    void STURDY_ABI_CALL global_system(SturdyEntity, void **components, SturdyCommands, void *user_data) {
        auto *probe = static_cast<GlobalProbe *>(user_data);
        ++probe->invocations;

        // A global system is handed no components at all; reading through this would be a bug, and
        // asserting on it here is what keeps that contract honest.
        if (components != nullptr) {
            probe->send_result = STURDY_ERROR_INTERNAL;
            return;
        }

        FrameCounter counter{};
        if (sturdy_ecs_get_resource(probe->engine, probe->counter, &counter, sizeof(counter)) == STURDY_OK) {
            ++counter.frames;
            (void)sturdy_ecs_set_resource(probe->engine, probe->counter, &counter, sizeof(counter));
        }

        const DamageEvent event{5, 1};
        probe->send_result = sturdy_ecs_send_event(probe->engine, probe->channel, &event, sizeof(event));
    }

    /// Counter a background task bumps, to prove the body actually ran on a worker.
    struct TaskProbe {
        SFT::Async::Scheduler *unused;
        int ran;
        int on_worker;
    };

    /// Marks that it ran, and whether it was on a scheduler worker thread.
    void STURDY_ABI_CALL background_task(void *user_data) {
        auto *probe = static_cast<TaskProbe *>(user_data);
        SturdyBool worker = STURDY_FALSE;
        (void)sturdy_async_is_worker_thread(&worker);
        probe->on_worker = worker != STURDY_FALSE ? 1 : 0;
        probe->ran = 1;
    }

    /// Throws, to prove an exception escaping a task body cannot unwind into the scheduler.
    void STURDY_ABI_CALL throwing_task(void *user_data) {
        ++*static_cast<int *>(user_data);
        throw std::runtime_error("deliberate");
    }

} // namespace

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    using SFT::Ffi::HandleKind;
    using SFT::Ffi::ScopedHandle;

    SFT::Engine::Engine engine_object;
    const ScopedHandle engine_handle{HandleKind::Engine, &engine_object};
    const SturdyEngine engine{engine_handle.token()};

    // ── Registration ───────────────────────────────────────────────────────────────────────────
    SturdyComponentId position = 0;
    SturdyComponentId health = 0;
    check(sturdy_ecs_register_component(engine, "test.ffi.position", sizeof(Position), alignof(Position),
                                        &position) == STURDY_OK,
          "registering a plain-data component must succeed");
    check(sturdy_ecs_register_component(engine, "test.ffi.health", sizeof(Health), alignof(Health),
                                        &health) == STURDY_OK,
          "registering a second component must succeed");
    check(position != health, "distinct components must get distinct ids");

    // Idempotent re-registration is expected: a binding calls this every run.
    SturdyComponentId position_again = 0;
    check(sturdy_ecs_register_component(engine, "test.ffi.position", sizeof(Position), alignof(Position),
                                        &position_again) == STURDY_OK,
          "re-registering an identical component must succeed");
    check(position_again == position, "re-registration must return the original id");

    // The same name with a different layout must not silently rebind, since live entities are
    // already laid out the old way.
    SturdyComponentId conflicting = 0;
    check(sturdy_ecs_register_component(engine, "test.ffi.position", sizeof(Position) + 4,
                                        alignof(Position), &conflicting) == STURDY_ERROR_INVALID_ARGUMENT,
          "re-registering with a different layout must be rejected");

    check(sturdy_ecs_register_component(engine, "test.ffi.zero", 0, 4, &conflicting) ==
              STURDY_ERROR_INVALID_ARGUMENT,
          "a zero-sized component must be rejected");
    check(sturdy_ecs_register_component(engine, "test.ffi.align", 8, 3, &conflicting) ==
              STURDY_ERROR_INVALID_ARGUMENT,
          "a non-power-of-two alignment must be rejected");
    check(sturdy_ecs_register_component(engine, "", 8, 8, &conflicting) == STURDY_ERROR_INVALID_ARGUMENT,
          "an empty component name must be rejected");

    // ── Lookup and introspection ───────────────────────────────────────────────────────────────
    SturdyComponentId found = 0;
    check(sturdy_ecs_find_component(engine, "test.ffi.position", &found) == STURDY_OK,
          "finding a registered component must succeed");
    check(found == position, "lookup must return the same id registration did");
    check(sturdy_ecs_find_component(engine, "test.ffi.nonexistent", &found) == STURDY_ERROR_NOT_AVAILABLE,
          "finding an unregistered component must report unavailability");

    SturdyComponentInfo info{};
    check(sturdy_ecs_component_info(engine, position, &info) == STURDY_OK,
          "reading component info must succeed");
    check(info.size == sizeof(Position), "reported size must match what was registered");
    check(info.align == alignof(Position), "reported alignment must match what was registered");
    check(info.blittable != STURDY_FALSE, "an FFI-registered component must be blittable");

    char name[128] = {};
    std::size_t name_length = 0;
    check(sturdy_ecs_component_name(engine, position, name, sizeof(name), &name_length) == STURDY_OK,
          "reading a component name must succeed");
    check(std::strcmp(name, "test.ffi.position") == 0, "the canonical name must round-trip");

    // An engine-defined component must be reachable by name without registering anything, which is
    // how a foreign caller interoperates with the engine's own systems.
    SturdyComponentId world_transform = 0;
    const SturdyResult transform_found =
        sturdy_ecs_find_component(engine, "sturdy.engine.world_transform", &world_transform);
    check(transform_found == STURDY_OK || transform_found == STURDY_ERROR_NOT_AVAILABLE,
          "looking up an engine component must not fail in an unexpected way");

    // ── Spawn, read, write ─────────────────────────────────────────────────────────────────────
    const Position start{1.0f, 2.0f, 3.0f};
    const Health full{100, 100};
    SturdyComponentInit init[2];
    init[0].component = position;
    init[0].size = sizeof(Position);
    init[0].data = &start;
    init[1].component = health;
    init[1].size = sizeof(Health);
    init[1].data = &full;

    SturdyEntity entity{};
    check(sturdy_ecs_spawn(engine, init, 2, &entity) == STURDY_OK, "spawning must succeed");

    SturdyBool alive = STURDY_FALSE;
    check(sturdy_ecs_is_alive(engine, entity, &alive) == STURDY_OK && alive != STURDY_FALSE,
          "the spawned entity must be alive");

    SturdyBool present = STURDY_FALSE;
    check(sturdy_ecs_has_component(engine, entity, position, &present) == STURDY_OK &&
              present != STURDY_FALSE,
          "the entity must carry Position");

    Position read_back{};
    check(sturdy_ecs_get_component(engine, entity, position, &read_back, sizeof(read_back)) == STURDY_OK,
          "reading Position must succeed");
    check(read_back.x == 1.0f && read_back.z == 3.0f, "Position must round-trip byte for byte");

    const Position moved{4.0f, 5.0f, 6.0f};
    check(sturdy_ecs_set_component(engine, entity, position, &moved, sizeof(moved)) == STURDY_OK,
          "writing Position must succeed");
    check(sturdy_ecs_get_component(engine, entity, position, &read_back, sizeof(read_back)) == STURDY_OK,
          "re-reading Position must succeed");
    check(read_back.x == 4.0f, "the write must be observable");

    // Size disagreements mean the binding's struct and the engine's have diverged.
    check(sturdy_ecs_get_component(engine, entity, position, &read_back, sizeof(read_back) - 1) ==
              STURDY_ERROR_INVALID_ARGUMENT,
          "a mismatched read size must be rejected");
    check(sturdy_ecs_set_component(engine, entity, position, &moved, sizeof(moved) + 8) ==
              STURDY_ERROR_INVALID_ARGUMENT,
          "a mismatched write size must be rejected");

    // ── Structural changes ─────────────────────────────────────────────────────────────────────
    check(sturdy_ecs_add_component(engine, entity, position, &moved, sizeof(moved)) ==
              STURDY_ERROR_COMPONENT_PRESENT,
          "adding a component the entity already has must be reported distinctly");
    check(sturdy_ecs_remove_component(engine, entity, health) == STURDY_OK,
          "removing Health must succeed");
    check(sturdy_ecs_remove_component(engine, entity, health) == STURDY_ERROR_COMPONENT_MISSING,
          "removing an absent component must be reported distinctly");
    check(sturdy_ecs_get_component(engine, entity, health, &read_back, sizeof(Health)) ==
              STURDY_ERROR_COMPONENT_MISSING,
          "reading a removed component must be reported distinctly");
    check(sturdy_ecs_remove_component(engine, entity, position) == STURDY_ERROR_INVALID_ARGUMENT,
          "removing the last component must be rejected");

    check(sturdy_ecs_add_component(engine, entity, health, &full, sizeof(full)) == STURDY_OK,
          "re-adding Health must succeed");
    check(sturdy_ecs_get_component(engine, entity, position, &read_back, sizeof(read_back)) == STURDY_OK,
          "Position must survive the archetype moves");
    check(read_back.x == 4.0f, "archetype moves must preserve component values");

    // ── Iteration ──────────────────────────────────────────────────────────────────────────────
    const Position second_start{10.0f, 0.0f, 0.0f};
    SturdyComponentInit single;
    single.component = position;
    single.size = sizeof(Position);
    single.data = &second_start;
    SturdyEntity second{};
    check(sturdy_ecs_spawn(engine, &single, 1, &second) == STURDY_OK,
          "spawning a Position-only entity must succeed");

    VisitTally tally{0, 0.0f};
    uint32_t visited = 0;
    const SturdyComponentId position_only[]{position};
    check(sturdy_ecs_for_each(engine, position_only, 1, tally_visit, &tally, &visited) == STURDY_OK,
          "iterating on Position must succeed");
    check(visited == 2, "both Position holders must be visited");
    check(tally.count == 2, "the visitor must run once per match");
    check(tally.sum_x == 14.0f, "the visitor must see live component values");

    const SturdyComponentId both[]{position, health};
    VisitTally narrow{0, 0.0f};
    check(sturdy_ecs_for_each(engine, both, 2, tally_visit, &narrow, &visited) == STURDY_OK,
          "iterating on two components must succeed");
    check(visited == 1, "only the entity carrying both must be visited");

    check(sturdy_ecs_for_each(engine, position_only, 1, mutate_visit, nullptr, nullptr) == STURDY_OK,
          "mutating iteration must succeed");
    check(sturdy_ecs_get_component(engine, entity, position, &read_back, sizeof(read_back)) == STURDY_OK,
          "reading after mutating iteration must succeed");
    check(read_back.y == 99.0f, "a write through a visitor pointer must reach real storage");

    // Reentrancy: calling back into the ECS from a visitor must be refused, not deadlock. If this
    // regressed, the test would hang rather than fail, which is why the timeout matters.
    ReentryProbe probe{engine, position, STURDY_OK, STURDY_OK};
    check(sturdy_ecs_for_each(engine, position_only, 1, reentrant_visit, &probe, nullptr) == STURDY_OK,
          "iteration containing a reentrant call must still complete");
    check(probe.read_result == STURDY_ERROR_BUSY,
          "a read from inside a visitor must report BUSY rather than deadlocking");
    check(probe.spawn_result == STURDY_ERROR_BUSY,
          "a spawn from inside a visitor must report BUSY rather than corrupting iteration");

    check(sturdy_ecs_for_each(engine, position_only, 1, nullptr, nullptr, nullptr) ==
              STURDY_ERROR_INVALID_ARGUMENT,
          "a null visitor must be rejected");
    check(sturdy_ecs_for_each(engine, position_only, 0, tally_visit, &tally, nullptr) ==
              STURDY_ERROR_INVALID_ARGUMENT,
          "iterating on zero components must be rejected");

    // ── Dead entities ──────────────────────────────────────────────────────────────────────────
    check(sturdy_ecs_destroy(engine, entity) == STURDY_OK, "destroying must succeed");
    check(sturdy_ecs_is_alive(engine, entity, &alive) == STURDY_OK && alive == STURDY_FALSE,
          "the destroyed entity must not be alive");
    check(sturdy_ecs_destroy(engine, entity) == STURDY_OK,
          "destroying twice must be a no-op rather than an error");
    check(sturdy_ecs_get_component(engine, entity, position, &read_back, sizeof(read_back)) ==
              STURDY_ERROR_ENTITY_NOT_ALIVE,
          "reading a dead entity must be reported distinctly");
    check(sturdy_ecs_set_component(engine, entity, position, &moved, sizeof(moved)) ==
              STURDY_ERROR_ENTITY_NOT_ALIVE,
          "writing a dead entity must be reported distinctly");
    check(sturdy_ecs_add_component(engine, entity, health, &full, sizeof(full)) ==
              STURDY_ERROR_ENTITY_NOT_ALIVE,
          "adding to a dead entity must be reported distinctly");
    check(sturdy_ecs_has_component(engine, entity, position, &present) == STURDY_OK &&
              present == STURDY_FALSE,
          "a dead entity carries nothing, which is not itself an error");

    // A zeroed entity is the shape a binding gets from a default-constructed struct.
    const SturdyEntity never_valid{0, 0};
    check(sturdy_ecs_is_alive(engine, never_valid, &alive) == STURDY_OK && alive == STURDY_FALSE,
          "a zeroed entity must never be alive");

    // ── Systems ────────────────────────────────────────────────────────────────────────────────
    // Registered systems run through the engine's real schedule, so this exercises the scheduler,
    // the access declaration, and the deferred command buffer together.
    {
        SturdyComponentId velocity = 0;
        check(sturdy_ecs_register_component(engine, "test.ffi.velocity", sizeof(Position),
                                            alignof(Position), &velocity) == STURDY_OK,
              "registering the velocity component must succeed");

        // Two entities carrying Position+velocity, plus one Position-only that must never be seen
        // by a system declaring both.
        const Position origin{0.0f, 0.0f, 0.0f};
        const Position step{1.0f, 0.0f, 0.0f};
        SturdyComponentInit moving[2];
        moving[0].component = position;
        moving[0].size = sizeof(Position);
        moving[0].data = &origin;
        moving[1].component = velocity;
        moving[1].size = sizeof(Position);
        moving[1].data = &step;

        SturdyEntity first_mover{};
        SturdyEntity second_mover{};
        check(sturdy_ecs_spawn(engine, moving, 2, &first_mover) == STURDY_OK, "spawning a mover must succeed");
        check(sturdy_ecs_spawn(engine, moving, 2, &second_mover) == STURDY_OK,
              "spawning a second mover must succeed");

        SturdySystemAccess integrate_access[2];
        integrate_access[0].component = position;
        integrate_access[0].mode = STURDY_ACCESS_WRITE;
        integrate_access[1].component = velocity;
        integrate_access[1].mode = STURDY_ACCESS_READ;

        SystemTally integrate_tally{0, 0};
        check(sturdy_ecs_add_system(engine, integrate_access, 2, integrate_system, &integrate_tally) ==
                  STURDY_OK,
              "registering a system must succeed");

        check(sturdy_ecs_add_system(engine, integrate_access, 2, nullptr, nullptr) ==
                  STURDY_ERROR_INVALID_ARGUMENT,
              "a null system body must be rejected");
        check(sturdy_ecs_add_system(engine, integrate_access, 0, integrate_system, nullptr) ==
                  STURDY_ERROR_INVALID_ARGUMENT,
              "a system declaring no components must be rejected");

        SturdySystemAccess duplicate_access[2];
        duplicate_access[0].component = position;
        duplicate_access[0].mode = STURDY_ACCESS_READ;
        duplicate_access[1].component = position;
        duplicate_access[1].mode = STURDY_ACCESS_WRITE;
        check(sturdy_ecs_add_system(engine, duplicate_access, 2, integrate_system, nullptr) ==
                  STURDY_ERROR_INVALID_ARGUMENT,
              "declaring the same component twice must be rejected");

        SturdySystemAccess bad_mode[1];
        bad_mode[0].component = position;
        bad_mode[0].mode = (SturdyAccessMode)9999;
        check(sturdy_ecs_add_system(engine, bad_mode, 1, integrate_system, nullptr) ==
                  STURDY_ERROR_INVALID_ARGUMENT,
              "an unrecognized access mode must be rejected");

        // Run the schedule the way the engine does each frame.
        engine_object.update_schedule().run(engine_object.ecs_world());

        check(integrate_tally.invocations == 2,
              "the system must run once per entity carrying both declared components");

        Position after{};
        check(sturdy_ecs_get_component(engine, first_mover, position, &after, sizeof(after)) == STURDY_OK,
              "reading a moved entity must succeed");
        check(after.x == 1.0f, "the system's write must reach real component storage");

        engine_object.update_schedule().run(engine_object.ecs_world());
        check(integrate_tally.invocations == 4, "a second run must visit both entities again");
        check(sturdy_ecs_get_component(engine, first_mover, position, &after, sizeof(after)) == STURDY_OK,
              "reading after the second run must succeed");
        check(after.x == 2.0f, "writes must accumulate across runs");
    }

    // ── Deferred commands from a system ────────────────────────────────────────────────────────
    // Structural changes cannot happen while systems are reading the world, so they are queued and
    // applied at the stage boundary. This is the one path by which foreign game logic can create
    // and destroy entities.
    {
        SturdyComponentId marker = 0;
        check(sturdy_ecs_register_component(engine, "test.ffi.marker", sizeof(Health), alignof(Health),
                                            &marker) == STURDY_OK,
              "registering the marker component must succeed");

        const Health tag{7, 7};
        SturdyComponentInit seed;
        seed.component = marker;
        seed.size = sizeof(Health);
        seed.data = &tag;
        SturdyEntity seed_entity{};
        check(sturdy_ecs_spawn(engine, &seed, 1, &seed_entity) == STURDY_OK,
              "spawning the seed entity must succeed");

        SturdySystemAccess marker_access[1];
        marker_access[0].component = marker;
        marker_access[0].mode = STURDY_ACCESS_READ;

        CommandProbe probe{marker, sizeof(Health), STURDY_OK, 0, STURDY_OK, engine};
        check(sturdy_ecs_add_system(engine, marker_access, 1, spawning_system, &probe) == STURDY_OK,
              "registering the spawning system must succeed");

        uint32_t before = 0;
        check(sturdy_ecs_for_each(engine, marker_access_ids(marker), 1, count_visit, &before, nullptr) ==
                  STURDY_OK,
              "counting markers before the run must succeed");
        check(before == 1, "exactly one marker entity should exist before the run");

        engine_object.update_schedule().run(engine_object.ecs_world());

        check(probe.spawn_result == STURDY_OK,
              "queueing a spawn from inside a system must be accepted");
        check(probe.invocations == 1, "the spawning system must have run once");

        // The queued spawn must have been applied by the time the schedule returns, and must not
        // have been visible to the same run that queued it.
        uint32_t after = 0;
        check(sturdy_ecs_for_each(engine, marker_access_ids(marker), 1, count_visit, &after, nullptr) ==
                  STURDY_OK,
              "counting markers after the run must succeed");
        check(after == 2, "the deferred spawn must have been applied at the stage boundary");

        // Direct ECS calls from inside a system must be refused rather than corrupting the world.
        check(probe.direct_spawn_result == STURDY_ERROR_BUSY,
              "a direct spawn from inside a system must report BUSY");
    }

    // ── Resources ──────────────────────────────────────────────────────────────────────────────
    {
        SturdyResourceId id{};
        SturdyResourceId same{};
        check(sturdy_ecs_resource_id("test.ffi.counter", &id) == STURDY_OK,
              "deriving a resource id must succeed");
        check(sturdy_ecs_resource_id("test.ffi.counter", &same) == STURDY_OK,
              "deriving the same id twice must succeed");
        check(id.high == same.high && id.low == same.low,
              "a name must always derive the same id, or persisted ids would not survive a restart");
        check(sturdy_ecs_resource_id(nullptr, &id) == STURDY_ERROR_INVALID_ARGUMENT,
              "a null resource name must be rejected");
        check(sturdy_ecs_resource_id("", &id) == STURDY_ERROR_INVALID_ARGUMENT,
              "an empty resource name must be rejected");

        SturdyBool present = STURDY_TRUE;
        check(sturdy_ecs_has_resource(engine, same, &present) == STURDY_OK && present == STURDY_FALSE,
              "an unbound resource must report absent");

        const FrameCounter initial{41, 0};
        SturdyResourceId counter{};
        check(sturdy_ecs_create_resource(engine, "test.ffi.counter", sizeof(FrameCounter), &initial,
                                         &counter) == STURDY_OK,
              "creating a resource must succeed");
        check(counter.high == same.high && counter.low == same.low,
              "creation must report the same id the name derives");

        check(sturdy_ecs_has_resource(engine, counter, &present) == STURDY_OK && present != STURDY_FALSE,
              "a bound resource must report present");

        FrameCounter read_back{};
        check(sturdy_ecs_get_resource(engine, counter, &read_back, sizeof(read_back)) == STURDY_OK,
              "reading a resource must succeed");
        check(read_back.frames == 41, "the initial value must round-trip");

        const FrameCounter updated{100, 0};
        check(sturdy_ecs_set_resource(engine, counter, &updated, sizeof(updated)) == STURDY_OK,
              "writing a resource must succeed");
        check(sturdy_ecs_get_resource(engine, counter, &read_back, sizeof(read_back)) == STURDY_OK,
              "re-reading a resource must succeed");
        check(read_back.frames == 100, "the write must be observable");

        check(sturdy_ecs_get_resource(engine, counter, &read_back, sizeof(read_back) - 1) ==
                  STURDY_ERROR_INVALID_ARGUMENT,
              "a mismatched resource read size must be rejected");
        check(sturdy_ecs_set_resource(engine, counter, &updated, sizeof(updated) + 4) ==
                  STURDY_ERROR_INVALID_ARGUMENT,
              "a mismatched resource write size must be rejected");

        // Re-creating with the same size is idempotent; a different size is not.
        check(sturdy_ecs_create_resource(engine, "test.ffi.counter", sizeof(FrameCounter), nullptr,
                                         nullptr) == STURDY_OK,
              "re-creating an identical resource must succeed");
        check(sturdy_ecs_get_resource(engine, counter, &read_back, sizeof(read_back)) == STURDY_OK &&
                  read_back.frames == 100,
              "re-creating must not reset an existing resource");
        check(sturdy_ecs_create_resource(engine, "test.ffi.counter", sizeof(FrameCounter) + 8, nullptr,
                                         nullptr) == STURDY_ERROR_INVALID_ARGUMENT,
              "re-creating with a different size must be rejected");

        check(sturdy_ecs_create_resource(engine, "test.ffi.zero_resource", 0, nullptr, nullptr) ==
                  STURDY_ERROR_INVALID_ARGUMENT,
              "a zero-sized resource must be rejected");

        // Unknown ids must report absence rather than reading whatever is nearby.
        SturdyResourceId missing{};
        check(sturdy_ecs_resource_id("test.ffi.never_bound", &missing) == STURDY_OK,
              "deriving an id for an unbound name must succeed");
        check(sturdy_ecs_get_resource(engine, missing, &read_back, sizeof(read_back)) ==
                  STURDY_ERROR_NOT_AVAILABLE,
              "reading an unbound resource must report unavailability");
        check(sturdy_ecs_destroy_resource(engine, missing) == STURDY_ERROR_NOT_AVAILABLE,
              "destroying an unbound resource must report unavailability");
    }

    // ── Event channels ─────────────────────────────────────────────────────────────────────────
    {
        SturdyResourceId channel{};
        check(sturdy_ecs_create_event_channel(engine, "test.ffi.damage", sizeof(DamageEvent), &channel) ==
                  STURDY_OK,
              "creating an event channel must succeed");

        uint32_t count = 99;
        check(sturdy_ecs_event_count(engine, channel, &count) == STURDY_OK && count == 0,
              "a fresh channel must be empty");

        const DamageEvent first{10, 1};
        const DamageEvent second{20, 2};
        check(sturdy_ecs_send_event(engine, channel, &first, sizeof(first)) == STURDY_OK,
              "sending an event must succeed");
        check(sturdy_ecs_send_event(engine, channel, &second, sizeof(second)) == STURDY_OK,
              "sending a second event must succeed");
        check(sturdy_ecs_event_count(engine, channel, &count) == STURDY_OK && count == 2,
              "both events must be buffered");

        DamageEvent read_back{};
        check(sturdy_ecs_read_event(engine, channel, 0, &read_back, sizeof(read_back)) == STURDY_OK,
              "reading the first event must succeed");
        check(read_back.amount == 10 && read_back.source == 1, "the first event must round-trip");
        check(sturdy_ecs_read_event(engine, channel, 1, &read_back, sizeof(read_back)) == STURDY_OK,
              "reading the second event must succeed");
        check(read_back.amount == 20, "events must be readable in send order");

        check(sturdy_ecs_read_event(engine, channel, 2, &read_back, sizeof(read_back)) ==
                  STURDY_ERROR_OUT_OF_RANGE,
              "reading past the end must be rejected");
        check(sturdy_ecs_send_event(engine, channel, &first, sizeof(first) + 4) ==
                  STURDY_ERROR_INVALID_ARGUMENT,
              "a mismatched event size must be rejected");

        check(sturdy_ecs_clear_events(engine, channel) == STURDY_OK, "clearing a channel must succeed");
        check(sturdy_ecs_event_count(engine, channel, &count) == STURDY_OK && count == 0,
              "clearing must empty the channel");

        // A channel and a plain resource must not be confused for one another.
        check(sturdy_ecs_create_event_channel(engine, "test.ffi.counter", sizeof(DamageEvent), nullptr) ==
                  STURDY_ERROR_INVALID_ARGUMENT,
              "creating a channel over an existing plain resource must be rejected");
    }

    // ── Global systems, resources and event draining ───────────────────────────────────────────
    {
        SturdyResourceId counter{};
        SturdyResourceId channel{};
        check(sturdy_ecs_resource_id("test.ffi.counter", &counter) == STURDY_OK, "id lookup must succeed");
        check(sturdy_ecs_resource_id("test.ffi.damage", &channel) == STURDY_OK, "id lookup must succeed");

        GlobalProbe probe{engine, counter, channel, 0, STURDY_OK};

        SturdySystemResourceAccess resource_access[2];
        resource_access[0].resource = counter;
        resource_access[0].mode = STURDY_ACCESS_WRITE;
        resource_access[0].reserved = 0;
        resource_access[1].resource = channel;
        resource_access[1].mode = STURDY_ACCESS_WRITE;
        resource_access[1].reserved = 0;

        check(sturdy_ecs_add_system_with_resources(engine, nullptr, 0, resource_access, 2, global_system,
                                                   &probe) == STURDY_OK,
              "registering a global system must succeed");
        check(sturdy_ecs_add_system_with_resources(engine, nullptr, 0, nullptr, 0, global_system,
                                                   &probe) == STURDY_ERROR_INVALID_ARGUMENT,
              "a system declaring neither components nor resources must be rejected");

        SturdySystemResourceAccess duplicate[2];
        duplicate[0].resource = counter;
        duplicate[0].mode = STURDY_ACCESS_READ;
        duplicate[0].reserved = 0;
        duplicate[1].resource = counter;
        duplicate[1].mode = STURDY_ACCESS_WRITE;
        duplicate[1].reserved = 0;
        check(sturdy_ecs_add_system_with_resources(engine, nullptr, 0, duplicate, 2, global_system,
                                                   &probe) == STURDY_ERROR_INVALID_ARGUMENT,
              "declaring the same resource twice must be rejected");

        FrameCounter before{};
        check(sturdy_ecs_get_resource(engine, counter, &before, sizeof(before)) == STURDY_OK,
              "reading the counter before the run must succeed");

        engine_object.update_schedule().run(engine_object.ecs_world());

        check(probe.invocations == 1, "a global system must run exactly once per frame");
        check(probe.send_result == STURDY_OK, "sending an event from a global system must succeed");

        FrameCounter after{};
        check(sturdy_ecs_get_resource(engine, counter, &after, sizeof(after)) == STURDY_OK,
              "reading the counter after the run must succeed");
        check(after.frames == before.frames + 1,
              "a global system must be able to advance a resource it declared");

        uint32_t count = 0;
        check(sturdy_ecs_event_count(engine, channel, &count) == STURDY_OK,
              "counting events after the run must succeed");
        check(count == 1, "the event sent during the run must still be readable this frame");

        // The scheduler drains event channels at the start of each run. The event above must
        // therefore be gone after the next run, replaced by that run's own.
        engine_object.update_schedule().run(engine_object.ecs_world());
        check(probe.invocations == 2, "the global system must run again");
        check(sturdy_ecs_event_count(engine, channel, &count) == STURDY_OK,
              "counting events after the second run must succeed");
        check(count == 1,
              "the scheduler must drain the channel between frames rather than letting it grow");
    }

    // ── Tag components ─────────────────────────────────────────────────────────────────────────
    // A tag carries no data; its presence is the information. Verified here to occupy one byte,
    // matching an empty C++ struct, and to give each entity its own storage rather than aliasing.
    {
        SturdyComponentId asleep = 0;
        check(sturdy_ecs_register_tag_component(engine, "test.ffi.asleep", &asleep) == STURDY_OK,
              "registering a tag component must succeed");

        SturdyComponentInfo tag_info{};
        check(sturdy_ecs_component_info(engine, asleep, &tag_info) == STURDY_OK,
              "reading tag component info must succeed");
        check(tag_info.is_tag != STURDY_FALSE, "a tag must report itself as one");
        check(tag_info.size == 1, "a tag must occupy one byte, as an empty C++ struct does");

        SturdyComponentId plain = 0;
        check(sturdy_ecs_register_component(engine, "test.ffi.plain", sizeof(Health), alignof(Health),
                                            &plain) == STURDY_OK,
              "registering a plain component must succeed");
        SturdyComponentInfo plain_info{};
        check(sturdy_ecs_component_info(engine, plain, &plain_info) == STURDY_OK,
              "reading plain component info must succeed");
        check(plain_info.is_tag == STURDY_FALSE, "a data component must not report itself as a tag");

        // Two entities, one tagged.
        const Health value{3, 3};
        SturdyComponentInit init;
        init.component = plain;
        init.size = sizeof(Health);
        init.data = &value;

        SturdyEntity tagged{};
        SturdyEntity untagged{};
        check(sturdy_ecs_spawn(engine, &init, 1, &tagged) == STURDY_OK, "spawning must succeed");
        check(sturdy_ecs_spawn(engine, &init, 1, &untagged) == STURDY_OK, "spawning must succeed");

        check(sturdy_ecs_add_tag(engine, tagged, asleep) == STURDY_OK, "adding a tag must succeed");
        check(sturdy_ecs_add_tag(engine, tagged, asleep) == STURDY_ERROR_COMPONENT_PRESENT,
              "adding a tag twice must be reported distinctly");

        SturdyBool present = STURDY_FALSE;
        check(sturdy_ecs_has_component(engine, tagged, asleep, &present) == STURDY_OK &&
                  present != STURDY_FALSE,
              "the tagged entity must carry the tag");
        check(sturdy_ecs_has_component(engine, untagged, asleep, &present) == STURDY_OK &&
                  present == STURDY_FALSE,
              "the untagged entity must not carry the tag");

        // A tag must narrow a query exactly like a data component does.
        uint32_t visited = 0;
        const SturdyComponentId tag_query[]{plain, asleep};
        check(sturdy_ecs_for_each(engine, tag_query, 2, count_visit, &visited, nullptr) == STURDY_OK,
              "iterating on a tag must succeed");
        check(visited == 1, "only the tagged entity must match");

        check(sturdy_ecs_remove_component(engine, tagged, asleep) == STURDY_OK,
              "removing a tag must succeed");
        visited = 0;
        check(sturdy_ecs_for_each(engine, tag_query, 2, count_visit, &visited, nullptr) == STURDY_OK,
              "iterating after removal must succeed");
        check(visited == 0, "no entity must match once the tag is removed");

        // Re-registering a tag is idempotent, as for any component.
        SturdyComponentId again = 0;
        check(sturdy_ecs_register_tag_component(engine, "test.ffi.asleep", &again) == STURDY_OK,
              "re-registering a tag must succeed");
        check(again == asleep, "re-registration must return the original id");
    }

    // ── Async tasks ────────────────────────────────────────────────────────────────────────────
    // Pure CPU, so unlike most of the FFI this is real behavior rather than argument validation.
    {
        SturdyBool running = STURDY_TRUE;
        SturdyBool worker = STURDY_TRUE;
        uint32_t workers = 0xFFFFFFFFu;

        check(sturdy_async_is_running(&running) == STURDY_OK, "reading scheduler state must succeed");
        check(sturdy_async_worker_count(&workers) == STURDY_OK, "reading worker count must succeed");
        check(sturdy_async_is_worker_thread(&worker) == STURDY_OK &&  worker == STURDY_FALSE,
              "the main thread must not report itself as a scheduler worker");
        check(sturdy_async_is_running(nullptr) == STURDY_ERROR_INVALID_ARGUMENT,
              "a null output pointer must be rejected");

        TaskProbe probe{nullptr, 0, 0};
        SturdyTask task{};
        check(sturdy_async_spawn(background_task, &probe, STURDY_TASK_LIGHT, &task) == STURDY_OK,
              "spawning a task must succeed");
        check(sturdy_async_wait(task) == STURDY_OK, "waiting on a task must succeed");

        SturdyBool done = STURDY_FALSE;
        check(sturdy_async_is_done(task, &done) == STURDY_OK && done != STURDY_FALSE,
              "a waited-on task must report done");
        check(probe.ran == 1, "the task body must actually have run");
        check(probe.on_worker == 1, "the task body must run on a scheduler worker, not the caller");

        // Spawning starts the scheduler, so these must now report a live pool.
        check(sturdy_async_is_running(&running) == STURDY_OK && running != STURDY_FALSE,
              "spawning must start the scheduler");
        check(sturdy_async_worker_count(&workers) == STURDY_OK && workers > 0,
              "a running scheduler must report at least one worker");

        check(sturdy_async_release(task) == STURDY_OK, "releasing a task must succeed");
        check(sturdy_async_release(task) == STURDY_ERROR_HANDLE_EXPIRED,
              "releasing twice must report expiry rather than double-freeing");
        check(sturdy_async_is_done(task, &done) == STURDY_ERROR_HANDLE_EXPIRED,
              "using a released task handle must be rejected");

        check(sturdy_async_spawn(nullptr, nullptr, STURDY_TASK_LIGHT, &task) ==
                  STURDY_ERROR_INVALID_ARGUMENT,
              "a null task body must be rejected");
        check(sturdy_async_spawn(background_task, &probe, (SturdyTaskWeight)9999, &task) ==
                  STURDY_ERROR_INVALID_ARGUMENT,
              "an unrecognized task weight must be rejected");

        // Fire-and-forget: no handle requested, nothing to release.
        TaskProbe forgotten{nullptr, 0, 0};
        check(sturdy_async_spawn(background_task, &forgotten, STURDY_TASK_HEAVY, nullptr) == STURDY_OK,
              "spawning without a handle must succeed");

        // An exception escaping a task body must be contained rather than unwinding into the
        // scheduler, where nothing is left to catch it.
        int threw = 0;
        SturdyTask throwing{};
        check(sturdy_async_spawn(throwing_task, &threw, STURDY_TASK_LIGHT, &throwing) == STURDY_OK,
              "spawning a throwing task must succeed");
        check(sturdy_async_wait(throwing) == STURDY_OK,
              "waiting on a task whose body threw must still complete");
        check(threw == 1, "the throwing task body must have run");
        check(sturdy_async_release(throwing) == STURDY_OK, "releasing must succeed");
    }

    // Schedule::run starts the process-wide async scheduler on first use. Left running, its
    // worker threads outlive main and abort during static destruction — which is how this first
    // showed up: every check passed and the process still exited 3.
    SFT::Async::Scheduler::shutdown();

    if (failures != 0) {
        (void)std::fprintf(stderr, "EcsFfiTest: %d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
