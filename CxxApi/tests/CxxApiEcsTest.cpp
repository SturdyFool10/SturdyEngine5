// The CxxApi generic ECS on a bare engine: components registered by name with a described layout (and rejected when two
// registrations disagree), entities by id, chunk systems that write through column pointers and queue changes, and the system
// handle functions.

#include <Async/Scheduler.hpp>

#include <CxxApi/CxxApi.hpp>
#include <CxxApi/Error.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

    template <typename F> bool throws_error(F &&call, std::string *message = nullptr) {
        try {
            call();
        } catch (const Error &e) {
            if (message != nullptr) {
                *message = e.what();
            }
            return true;
        }
        return false;
    }

    struct Vec2 {
        float x;
        float y;
    };

    template <typename T> std::vector<std::uint8_t> bytes_of(const T &value) {
        std::vector<std::uint8_t> out(sizeof(T));
        std::memcpy(out.data(), &value, sizeof(T));
        return out;
    }
    template <typename T> T value_of(const std::vector<std::uint8_t> &bytes) {
        T out{};
        std::memcpy(&out, bytes.data(), sizeof(T));
        return out;
    }

    /// position += velocity over each chunk, and queues the spawn of one marker entity per chunk it sees.
    class Integrate final : public ChunkSystemCallback {
      public:
        Integrate(std::uint32_t marker_component, int &chunk_calls, int &destroyed) : marker_(marker_component), calls_(chunk_calls), destroyed_(destroyed) {}
        ~Integrate() override { ++destroyed_; }

        void run(const Entity *entities, std::size_t count, void *const *columns, SystemCommands &commands) noexcept override {
            ++calls_;
            auto *positions = static_cast<Vec2 *>(columns[0]);
            const auto *velocities = static_cast<const Vec2 *>(columns[1]);
            for (std::size_t i = 0; i < count; ++i) {
                positions[i].x += velocities[i].x;
                positions[i].y += velocities[i].y;
                (void)entities[i];
            }
            const std::uint32_t one = 1;
            std::vector<std::uint8_t> marker(sizeof(one));
            std::memcpy(marker.data(), &one, sizeof(one));
            try {
                commands_spawn(commands, {marker_}, marker);
            } catch (...) {
            }
        }

      private:
        std::uint32_t marker_;
        int &calls_;
        int &destroyed_;
    };

    /// Sends one event per chunk carrying the chunk's row count.
    class Producer final : public ChunkSystemCallback {
      public:
        Producer(Ecs::World &world, ResourceId channel) : world_(world), channel_(channel) {}
        void run(const Entity *, std::size_t count, void *const *, SystemCommands &) noexcept override {
            const std::uint32_t rows = static_cast<std::uint32_t>(count);
            std::vector<std::uint8_t> payload(sizeof(rows));
            std::memcpy(payload.data(), &rows, sizeof(rows));
            try {
                ecs_send_event(world_, channel_, payload.data(), payload.size());
            } catch (...) {
            }
        }

      private:
        Ecs::World &world_;
        ResourceId channel_;
    };

    /// Reads every event on the channel into `total` and counts them in `events_seen`.
    class Consumer final : public ChunkSystemCallback {
      public:
        Consumer(Ecs::World &world, ResourceId channel, std::uint32_t &total, std::uint32_t &events_seen)
            : world_(world), channel_(channel), total_(total), events_seen_(events_seen) {}
        void run(const Entity *, std::size_t, void *const *, SystemCommands &) noexcept override {
            try {
                const std::uint32_t n = ecs_event_count(world_, channel_);
                for (std::uint32_t i = 0; i < n; ++i) {
                    std::uint32_t rows = 0;
                    std::vector<std::uint8_t> out(sizeof(rows));
                    ecs_read_event(world_, channel_, i, out.data(), out.size());
                    std::memcpy(&rows, out.data(), sizeof(rows));
                    total_ += rows;
                    ++events_seen_;
                }
            } catch (...) {
            }
        }

      private:
        Ecs::World &world_;
        ResourceId channel_;
        std::uint32_t &total_;
        std::uint32_t &events_seen_;
    };

} // namespace

int main() {
    Engine::Engine engine;
    Ecs::World &world = engine.ecs_world();

    // ---- components ----
    const std::vector<ComponentFieldDesc> vec2_fields{{FieldKind::F32, 0, 1}, {FieldKind::F32, 4, 1}};
    const std::uint32_t position = ecs_register_component(world, "test.cxx.position", sizeof(Vec2), alignof(Vec2), vec2_fields, {"x", "y"});
    const std::uint32_t velocity = ecs_register_component(world, "test.cxx.velocity", sizeof(Vec2), alignof(Vec2), vec2_fields, {});
    const std::uint32_t marker = ecs_register_component(world, "test.cxx.marker", sizeof(std::uint32_t), alignof(std::uint32_t), {}, {});
    check(position != velocity && ecs_component_size(world, position) == sizeof(Vec2), "components get distinct ids and their size reads back");

    std::uint32_t found = 0;
    check(ecs_find_component(world, "test.cxx.position", found) && found == position, "a registered component is found by name");
    check(!ecs_find_component(world, "test.cxx.nope", found), "an unknown name is not found");
    check(ecs_register_component(world, "test.cxx.position", sizeof(Vec2), alignof(Vec2), vec2_fields, {"u", "v"}) == position,
          "registering the same layout again returns the same id, whatever the field names");

    const LayoutFingerprint print = ecs_component_layout(world, position);
    check(print.high != 0 || print.low != 0, "a described component has a fingerprint");
    const LayoutFingerprint none = ecs_component_layout(world, marker);
    check(none.high == 0 && none.low == 0, "a component registered without fields has none");

    std::string message;
    const std::vector<ComponentFieldDesc> as_ints{{FieldKind::U32, 0, 1}, {FieldKind::U32, 4, 1}};
    check(throws_error([&] { (void)ecs_register_component(world, "test.cxx.position", sizeof(Vec2), alignof(Vec2), as_ints, {}); }, &message) &&
              message.find("test.cxx.position") != std::string::npos && message.find("u32") != std::string::npos,
          "the same size with different field kinds throws, naming the component and the kind");
    check(throws_error([&] { (void)ecs_register_component(world, "test.cxx.position", 16, 4, {}, {}); }), "a different size throws");
    check(throws_error([&] { (void)ecs_register_component(world, "", 4, 4, {}, {}); }), "an empty name throws");
    check(throws_error([&] { (void)ecs_register_component(world, "test.cxx.zero", 0, 4, {}, {}); }), "a zero size throws");
    check(throws_error([&] { (void)ecs_register_component(world, "test.cxx.align", 4, 3, {}, {}); }), "a bad alignment throws");
    check(throws_error([&] { (void)ecs_register_component(world, "test.cxx.misfit", 4, 4, vec2_fields, {}); }), "fields that do not fit throw");
    check(throws_error([&] { (void)ecs_register_component(world, "test.cxx.names", 8, 4, vec2_fields, {"only_one"}); }), "mismatched field_names throw");
    check(throws_error([&] { (void)ecs_component_layout(world, 99999); }), "an unknown component id throws");

    // ---- entities ----
    std::vector<std::uint8_t> packed = bytes_of(Vec2{1.0f, 2.0f});
    const std::vector<std::uint8_t> v = bytes_of(Vec2{10.0f, 20.0f});
    packed.insert(packed.end(), v.begin(), v.end());
    const Entity moving = ecs_spawn(world, {position, velocity}, packed);
    const Entity still = ecs_spawn(world, {position}, bytes_of(Vec2{5.0f, 5.0f}));
    check(moving.generation != 0 && ecs_has_component(world, moving, velocity) && !ecs_has_component(world, still, velocity),
          "spawned entities carry exactly their components");
    check(throws_error([&] { (void)ecs_spawn(world, {position, velocity}, bytes_of(Vec2{0, 0})); }), "spawn data shorter than the sizes throws");
    check(throws_error([&] { (void)ecs_spawn(world, {}, {}); }), "an entity needs a component");
    check(throws_error([&] { (void)ecs_spawn(world, {position, position}, packed); }), "duplicate components throw");

    std::vector<std::uint8_t> out(sizeof(Vec2));
    check(ecs_get_component(world, moving, position, out.data(), out.size()) && value_of<Vec2>(out).y == 2.0f, "a component reads back");
    ecs_set_component(world, moving, position, bytes_of(Vec2{7.0f, 8.0f}).data(), sizeof(Vec2));
    check(ecs_get_component(world, moving, position, out.data(), out.size()) && value_of<Vec2>(out).x == 7.0f, "a component can be overwritten");
    check(!ecs_get_component(world, still, velocity, out.data(), out.size()), "a missing component reads as absent, not as an error");
    check(throws_error([&] { (void)ecs_get_component(world, moving, position, out.data(), 3); }), "a wrong buffer size throws");
    ecs_add_component(world, still, velocity, bytes_of(Vec2{1.0f, 1.0f}).data(), sizeof(Vec2));
    check(ecs_has_component(world, still, velocity), "a component can be added");
    ecs_remove_component(world, still, velocity);
    check(!ecs_has_component(world, still, velocity), "...and removed");
    check(throws_error([&] { ecs_add_component(world, moving, position, bytes_of(Vec2{0, 0}).data(), sizeof(Vec2)); }), "adding a component twice throws");

    ecs_set_component(world, moving, position, bytes_of(Vec2{0.0f, 0.0f}).data(), sizeof(Vec2));

    // ---- chunk systems ----
    int calls = 0;
    int destroyed = 0;
    const std::uint64_t system = ecs_add_chunk_system(
        engine, {{position, AccessMode::Write}, {velocity, AccessMode::Read}}, {}, std::make_unique<Integrate>(marker, calls, destroyed));
    check(system != 0, "a chunk system registers");
    check(throws_error([&] { (void)ecs_add_chunk_system(engine, {}, {}, std::make_unique<Integrate>(marker, calls, destroyed)); }), "a chunk system needs components");
    check(throws_error([&] { (void)ecs_add_chunk_system(engine, {{position, AccessMode::Read}}, {}, nullptr); }), "a chunk system needs a callback");
    check(throws_error([&] {
              (void)ecs_add_chunk_system(engine, {{position, AccessMode::Read}, {position, AccessMode::Write}}, {}, std::make_unique<Integrate>(marker, calls, destroyed));
          }),
          "declaring a component twice throws");
    destroyed = 0; // the rejected registrations above destroyed their callbacks, which is correct

    engine.update_schedule().run(world);
    check(calls == 1, "one archetype matches {position, velocity}, so the system is called once, not once per entity");
    check(ecs_get_component(world, moving, position, out.data(), out.size()) && value_of<Vec2>(out).x == 10.0f && value_of<Vec2>(out).y == 20.0f,
          "a write through the column pointer reaches the live component");
    check(ecs_get_component(world, still, position, out.data(), out.size()) && value_of<Vec2>(out).x == 5.0f, "an entity outside the archetype is untouched");

    // The marker spawn was queued from inside the system and applied when the stage ended.
    int markers = 0;
    // (count entities with the marker by scanning a few entity slots)
    for (std::uint32_t index = 0; index < 16; ++index) {
        for (std::uint32_t generation = 1; generation < 3; ++generation) {
            if (ecs_has_component(world, Entity{index, generation}, marker)) {
                ++markers;
            }
        }
    }
    check(markers == 1, "an entity queued with commands_spawn exists after the dispatch");

    ecs_set_system_enabled(engine, system, false);
    engine.update_schedule().run(world);
    check(calls == 1, "a disabled system does not run");
    ecs_set_system_enabled(engine, system, true);
    engine.update_schedule().run(world);
    check(calls == 2, "a re-enabled system runs again");

    const std::uint64_t other = ecs_add_chunk_system(engine, {{position, AccessMode::Read}}, {}, std::make_unique<Integrate>(marker, calls, destroyed));
    ecs_order_system_after(engine, other, system);
    check(throws_error([&] { ecs_order_system_after(engine, system, system); }), "a system cannot be ordered after itself");
    check(throws_error([&] { ecs_set_system_enabled(engine, 99999, true); }), "an unknown system handle throws");

    ecs_remove_system(engine, other);
    check(destroyed == 1, "removing a system destroys its callback");
    ecs_remove_system(engine, system);
    check(destroyed == 2, "...for each system");
    check(throws_error([&] { ecs_remove_system(engine, system); }), "removing twice throws");
    const int calls_before = calls;
    engine.update_schedule().run(world);
    check(calls == calls_before, "a removed system no longer runs");

    // ---- resources ----
    const ResourceId by_name = ecs_resource_id("test.cxx.score");
    check(by_name.high == ecs_resource_id("test.cxx.score").high && by_name.low == ecs_resource_id("test.cxx.score").low, "a resource id is derived from its name");
    check(throws_error([&] { (void)ecs_resource_id(""); }), "an empty resource name throws");

    const std::vector<ComponentFieldDesc> score_fields{{FieldKind::U32, 0, 1}, {FieldKind::F32, 4, 1}};
    const ResourceId score = ecs_create_resource(world, "test.cxx.score", 8, {}, score_fields, {"kills", "accuracy"});
    check(score.high == by_name.high && score.low == by_name.low && ecs_has_resource(world, score), "creating a resource binds it under its name's id");
    std::vector<std::uint8_t> score_bytes(8);
    ecs_read_resource(world, score, score_bytes.data(), score_bytes.size());
    check(score_bytes[0] == 0 && score_bytes[7] == 0, "a resource starts zeroed");
    const std::uint32_t kills = 7;
    std::memcpy(score_bytes.data(), &kills, sizeof(kills));
    ecs_write_resource(world, score, score_bytes.data(), score_bytes.size());
    std::vector<std::uint8_t> back(8);
    ecs_read_resource(world, score, back.data(), back.size());
    check(back[0] == 7, "a resource can be written and read back");
    check(throws_error([&] { ecs_read_resource(world, score, back.data(), 4); }), "a wrong resource size throws");
    const ResourceId again = ecs_create_resource(world, "test.cxx.score", 8, {}, score_fields, {});
    check(again.high == score.high, "creating it again with the same layout returns the same id");
    ecs_read_resource(world, score, back.data(), back.size());
    check(back[0] == 7, "...without resetting its value");
    check(throws_error([&] { (void)ecs_create_resource(world, "test.cxx.score", 8, {}, {{FieldKind::F32, 0, 1}, {FieldKind::F32, 4, 1}}, {}); }, &message) &&
              message.find("test.cxx.score") != std::string::npos,
          "the same size with a different layout throws, naming the resource");
    check(throws_error([&] { (void)ecs_create_resource(world, "test.cxx.initial", 8, {1, 2, 3}, {}, {}); }), "initial data of the wrong size throws");
    ecs_destroy_resource(world, score);
    check(!ecs_has_resource(world, score), "a destroyed resource is no longer bound");
    check(throws_error([&] { ecs_destroy_resource(world, score); }), "destroying it twice throws");
    check(throws_error([&] { ecs_destroy_resource(world, ecs_resource_id("sturdy.engine.render_frame_requests")); }),
          "the engine's own resources cannot be destroyed through this API");

    // ---- event channels between systems ----
    const ResourceId hits = ecs_create_event_channel(world, "test.cxx.hits", 4, {{FieldKind::U32, 0, 1}}, {"rows"});
    check(throws_error([&] { (void)ecs_create_event_channel(world, "test.cxx.hits", 4, {{FieldKind::F32, 0, 1}}, {}); }),
          "a second language that reads the 4-byte payload as a float is rejected");
    check(throws_error([&] { ecs_send_event(world, hits, back.data(), 8); }), "an event of the wrong size throws");
    check(throws_error([&] { ecs_send_event(world, ecs_resource_id("test.cxx.nope"), back.data(), 4); }), "sending to an unknown channel throws");

    std::uint32_t total_rows = 0;
    std::uint32_t events_seen = 0;
    const std::uint64_t producer = ecs_add_chunk_system(engine, {{position, AccessMode::Read}}, {{hits, AccessMode::Write}}, std::make_unique<Producer>(world, hits));
    const std::uint64_t consumer =
        ecs_add_chunk_system(engine, {{position, AccessMode::Read}}, {{hits, AccessMode::Read}}, std::make_unique<Consumer>(world, hits, total_rows, events_seen));
    check(throws_error([&] {
              (void)ecs_add_chunk_system(engine, {{position, AccessMode::Read}}, {{hits, AccessMode::Read}, {hits, AccessMode::Write}}, std::make_unique<Producer>(world, hits));
          }),
          "declaring a resource twice throws");

    // Position is carried by three entities in two archetypes ({position}, {position, velocity}) plus the queued marker entity's
    // archetype does not carry it; the producer sends one event per archetype that matches, and the consumer (declared as a
    // reader of the channel) runs after it and sees them all.
    engine.update_schedule().run(world);
    check(events_seen >= 1, "the consumer saw the events the producer sent in the same run");
    check(total_rows >= 2, "the events carried the chunk sizes");

    ecs_remove_system(engine, consumer);
    ecs_remove_system(engine, producer);
    ecs_clear_events(world, hits);
    check(ecs_event_count(world, hits) == 0, "clearing a channel empties it");
    check(throws_error([&] { (void)ecs_event_count(world, ecs_resource_id("test.cxx.nope")); }), "counting an unknown channel throws");
    check(throws_error([&] { ecs_read_event(world, hits, 0, back.data(), 4); }), "reading past the end throws");

    ecs_destroy(world, moving);
    check(!ecs_has_component(world, moving, position), "a destroyed entity has no components");

    // Running a schedule may start the process-wide async scheduler; left running its workers abort during static destruction.
    Async::Scheduler::shutdown();

    if (failures != 0) {
        std::fprintf(stderr, "CxxApiEcsTest: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("CxxApiEcsTest: ok\n");
    return 0;
}
