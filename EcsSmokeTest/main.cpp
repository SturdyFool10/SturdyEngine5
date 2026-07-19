#include <Foundation/src/Foundation.hpp>

#include <Async/src/Scheduler.hpp>
#include <Ecs/src/Component.hpp>
#include <Ecs/src/Event.hpp>
#include <Ecs/src/Module.hpp>
#include <Ecs/src/Resource.hpp>
#include <Ecs/src/System.hpp>
#include <Ecs/src/World.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using SFT::Ecs::ComponentRegistry;
using SFT::Ecs::Entity;
using SFT::Ecs::EventReader;
using SFT::Ecs::EventWriter;
using SFT::Ecs::Events;
using SFT::Ecs::Module;
using SFT::Ecs::ReadResource;
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

    struct PingEvent {
        i32 value = 0;
    };

    // Stands in for the real Engine::InputState (plans/ecs-engine-subsystem-access.md, not yet
    // implemented) — just enough surface to prove a module can map input to hotkey events.
    struct StubInputState {
        std::vector<i32> just_pressed_keys;

        [[nodiscard]] bool just_pressed(i32 key) const noexcept {
            return std::find(just_pressed_keys.begin(), just_pressed_keys.end(), key) != just_pressed_keys.end();
        }
    };

    struct HotkeyPressed {
        i32 hotkey_id = 0;
    };

} // namespace

SFT_ECS_COMPONENT(Position, "test.position");
SFT_ECS_COMPONENT(Velocity, "test.velocity");
SFT_ECS_EVENT(PingEvent, "test.ping_event");
SFT_ECS_RESOURCE(StubInputState, "test.stub_input_state");
SFT_ECS_EVENT(HotkeyPressed, "test.hotkey_pressed");

namespace {

    // Ecs::Module worked example from plans/ecs-events-and-modules.md: owns its own resource +
    // event buffer, registers one resource-only ("global") system as a single build() call rather
    // than requiring the consumer to hand-copy bind_resource()/add_system() calls.
    class HotkeyModule final : public Module {
      public:
        explicit HotkeyModule(i32 key, i32 hotkey_id) noexcept : key_(key), hotkey_id_(hotkey_id) {}

        void build(World &world, Schedule &schedule) override {
            world.bind_resource(input_);
            world.bind_resource(events_);

            const i32 key = key_;
            const i32 hotkey_id = hotkey_id_;
            schedule.add_system([key, hotkey_id](ReadResource<StubInputState> input,
                                                  EventWriter<HotkeyPressed> events) noexcept {
                if (input->just_pressed(key)) {
                    events.send(HotkeyPressed{hotkey_id});
                }
            });
        }

        [[nodiscard]] StubInputState &input() noexcept { return input_; }

      private:
        i32 key_ = 0;
        i32 hotkey_id_ = 0;
        StubInputState input_{};
        Events<HotkeyPressed> events_{};
    };

    // Deliberately registers an EventReader before any EventWriter for the same event type. Must
    // trip Schedule::validate_event_ordering()'s contract_violation (Foundation::log_error + flush +
    // std::terminate) instead of silently losing every event that tick. Run in its own process via
    // `--expect-event-order-violation` (see main()) so it never corrupts the positive-path exit code.
    [[noreturn]] void run_deliberate_ordering_violation() {
        ComponentRegistry registry;
        World world{registry};
        Events<PingEvent> events;
        world.bind_resource(events);

        Schedule schedule;
        schedule.add_system([](EventReader<PingEvent> reader) noexcept { (void)reader.read(); });
        schedule.add_system([](EventWriter<PingEvent> writer) noexcept { writer.send(PingEvent{1}); });

        schedule.run(world); // Must never return: contract_violation() calls std::terminate().
        std::fprintf(stderr, "expected the misordered schedule to trip the ordering contract.\n");
        std::abort();
    }

} // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--expect-event-order-violation") == 0) {
        run_deliberate_ordering_violation();
    }

    SFT::Async::Scheduler::initialize(2);

    ComponentRegistry registry;
    World world{registry};

    const Entity a = world.spawn(Position{0.0f, 0.0f}, Velocity{1.0f, 2.0f});
    [[maybe_unused]] const Entity b = world.spawn(Position{10.0f, 10.0f}, Velocity{-1.0f, 0.0f});

    Events<PingEvent> ping_events;
    world.bind_resource(ping_events);

    Schedule schedule;

    // Ordinary entity-based system — still works after generalizing resource resolution for events.
    i32 entity_updates = 0;
    schedule.add_system([&entity_updates](Entity /*entity*/, Position &position, const Velocity &velocity) noexcept {
        position.x += velocity.x;
        position.y += velocity.y;
        ++entity_updates;
    });

    // Resource-only ("global") system: an event producer with no entities to iterate at all.
    auto ping_hits = std::make_shared<std::atomic<i32>>(0);
    auto ping_sum = std::make_shared<std::atomic<i32>>(0);

    schedule.add_system([](EventWriter<PingEvent> pings) noexcept {
        pings.send(PingEvent{1});
        pings.send(PingEvent{2});
    });
    // Registered after the producer, per the producer-before-consumer ordering contract.
    schedule.add_system([ping_hits, ping_sum](EventReader<PingEvent> pings) noexcept {
        for (const PingEvent &event : pings.read()) {
            ping_sum->fetch_add(event.value, std::memory_order_relaxed);
        }
        ping_hits->fetch_add(1, std::memory_order_relaxed);
    });

    // Ecs::Module: bundles its own resource + event + system as one registration step, and can be
    // "wired in as a listener" by any other system declaring EventReader<HotkeyPressed>.
    HotkeyModule hotkeys{/*key=*/42, /*hotkey_id=*/7};
    hotkeys.build(world, schedule);

    auto hotkey_hits = std::make_shared<std::vector<i32>>();
    schedule.add_system([hotkey_hits](EventReader<HotkeyPressed> events) noexcept {
        for (const HotkeyPressed &event : events.read()) {
            hotkey_hits->push_back(event.hotkey_id);
        }
    });

    hotkeys.input().just_pressed_keys = {42};
    schedule.run(world);

    assert(entity_updates == 2); // once per matching entity — 2 entities, one tick.
    assert(ping_hits->load() == 1);
    assert(ping_sum->load() == 3); // 1 + 2, exactly one tick's events.
    assert(hotkey_hits->size() == 1 && (*hotkey_hits)[0] == 7);

    {
        auto pos_a = world.get_component<Position>(a);
        assert(pos_a != nullptr && pos_a->x == 1.0f && pos_a->y == 2.0f);
    }

    // Second tick, no new hotkey press: proves Events<T> auto-clears between runs rather than
    // accumulating — if clearing were broken, ping_sum would jump by 6 (stale [1,2] plus fresh
    // [1,2]) instead of 3, and hotkey_hits would stay stuck at whatever leaked from tick one.
    hotkeys.input().just_pressed_keys.clear();
    schedule.run(world);

    assert(entity_updates == 4); // 2 entities, two ticks.
    assert(ping_hits->load() == 2);
    assert(ping_sum->load() == 6);
    assert(hotkey_hits->size() == 1); // no new press this tick

    SFT::Async::Scheduler::shutdown();

    std::printf("Ecs smoke test passed (%d ping hits, sum=%d; %zu hotkey events).\n",
                ping_hits->load(),
                ping_sum->load(),
                hotkey_hits->size());
    return 0;
}
