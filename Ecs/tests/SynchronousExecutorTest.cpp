#include <Ecs/System.hpp>

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

    /// Checks the supplied condition and reports the accompanying diagnostic message when it is false.
    ///
    /// @param condition Condition controlling whether the operation proceeds.
    /// @param message Text consumed by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }


    /// Returns the current or globally available synchronous schedule starts no worker threads value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool synchronous_schedule_starts_no_worker_threads() {
        ComponentRegistry registry;
        World world{registry};
        Counter counter;
        world.bind_resource(counter);

        constexpr std::uint32_t row_count = 4096;
        for (std::uint32_t row = 0; row < row_count; ++row) {
            (void)world.spawn(Position{.id = row});
        }

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


    /// Returns the current or globally available async schedule still starts scheduler value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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
