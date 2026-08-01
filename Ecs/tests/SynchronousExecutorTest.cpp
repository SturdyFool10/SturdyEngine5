#include <Ecs/src/System.hpp>

#include <cstdint>
#include <iostream>

namespace TestTypes {

    struct Position {
        std::uint32_t id = 0;
        std::uint32_t updates = 0;
    };

    struct Doomed {};

    struct Counter {
        std::uint32_t value = 0;
    };

} // namespace TestTypes

SFT_ECS_COMPONENT(TestTypes::Position, "sturdy.test.sync.position");
SFT_ECS_COMPONENT(TestTypes::Doomed, "sturdy.test.sync.doomed");
SFT_ECS_RESOURCE(TestTypes::Counter, "sturdy.test.sync.counter");

namespace {

    using SFT::Ecs::Commands;
    using SFT::Ecs::ComponentRegistry;
    using SFT::Ecs::Entity;
    using SFT::Ecs::ExecutorPolicy;
    using SFT::Ecs::Schedule;
    using SFT::Ecs::ScheduleConfig;
    using SFT::Ecs::WriteResource;
    using SFT::Ecs::World;
    using TestTypes::Counter;
    using TestTypes::Doomed;
    using TestTypes::Position;

    bool check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }

    // The point of ExecutorPolicy::Synchronous is that it must never touch Async::Scheduler's
    // process-global state. This test deliberately never calls Async::Scheduler::initialize()
    // anywhere, so if Schedule::run() silently starts it (the bug this policy exists to prevent),
    // is_running() below would observe it.
    bool synchronous_schedule_starts_no_worker_threads() {
        ComponentRegistry registry;
        World world{registry};
        Counter counter;
        world.bind_resource(counter);

        constexpr std::uint32_t row_count = 4096;
        for (std::uint32_t row = 0; row < row_count; ++row) {
            (void)world.spawn(Position{.id = row});
        }
        // A second entity carrying Doomed exercises the Commands/command-buffer dispatch path.
        (void)world.spawn(Position{.id = row_count}, Doomed{});

        Schedule schedule{ScheduleConfig{.executor = ExecutorPolicy::Synchronous}};
        schedule.add_system([](Entity, Position &position) noexcept { ++position.updates; });
        schedule.add_system(
            [](Entity entity, Position &, const Doomed &, Commands &commands) noexcept { commands.destroy(entity); });
        schedule.add_system(
            [](WriteResource<Counter> counter) noexcept { ++counter->value; });

        schedule.run(world);

        bool passed = check(!SFT::Async::Scheduler::is_running(),
                            "Synchronous Schedule::run() started the Async::Scheduler worker pool");

        std::uint32_t updated_rows = 0;
        for (auto [entity, position] : world.query<const Position>()) {
            (void)entity;
            passed &= check(position.updates == 1, "synchronous system did not update every row exactly once");
            ++updated_rows;
        }
        passed &= check(updated_rows == row_count, "the Commands-destroyed entity was not removed synchronously");
        passed &= check(counter.value == 1, "synchronous resource-writing system did not run exactly once");

        return passed;
    }

    // The Async default path must keep working unchanged for existing callers (this repo already has
    // no callers passing ScheduleConfig::executor explicitly outside tests).
    bool async_schedule_still_starts_scheduler() {
        ComponentRegistry registry;
        World world{registry};
        (void)world.spawn(Position{.id = 0});

        SFT::Async::Scheduler::initialize(2);
        Schedule schedule{ScheduleConfig{}};
        schedule.add_system([](Entity, Position &position) noexcept { ++position.updates; });
        schedule.run(world);
        bool passed = check(SFT::Async::Scheduler::is_running(), "Async::Scheduler unexpectedly stopped running");
        SFT::Async::Scheduler::shutdown();
        return passed;
    }

} // namespace

int main() {
    bool passed = true;
    passed &= synchronous_schedule_starts_no_worker_threads();
    passed &= async_schedule_still_starts_scheduler();

    if (passed) {
        std::cout << "ECS synchronous executor policy tests passed.\n";
        return 0;
    }
    return 1;
}
