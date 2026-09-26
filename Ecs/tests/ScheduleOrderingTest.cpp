/// SystemHandle removal/disable and explicit order_after ordering.

#include <Ecs/System.hpp>

#include <iostream>
#include <string>

namespace TestTypes {
    struct A {
        int value = 0;
    };
    struct B {
        int value = 0;
    };
} // namespace TestTypes

SFT_ECS_RESOURCE(TestTypes::A, "sturdy.test.ordering.a");
SFT_ECS_RESOURCE(TestTypes::B, "sturdy.test.ordering.b");

namespace {

    using namespace SFT::Ecs;
    using TestTypes::A;
    using TestTypes::B;

    std::string order_log;

    int failures = 0;
    void check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    }

    Schedule make_schedule() { return Schedule{ScheduleConfig{.executor = ExecutorPolicy::Synchronous}}; }

} // namespace

int main() {
    ComponentRegistry registry;
    World world{registry};
    A a;
    B b;
    world.bind_resource(a);
    world.bind_resource(b);

    // Two systems touching disjoint resources share a stage and run in registration order...
    {
        Schedule schedule = make_schedule();
        order_log.clear();
        (void)schedule.add_system([](WriteResource<A>) noexcept { order_log += 'L'; });
        (void)schedule.add_system([](WriteResource<B>) noexcept { order_log += 'E'; });
        schedule.run(world);
        check(order_log == "LE", "without ordering, disjoint systems run in registration order");
    }

    // ...until order_after pushes one behind the other.
    {
        Schedule schedule = make_schedule();
        order_log.clear();
        const SystemHandle late = schedule.add_system([](WriteResource<A>) noexcept { order_log += 'L'; });
        const SystemHandle early = schedule.add_system([](WriteResource<B>) noexcept { order_log += 'E'; });
        check(late && early && late != early, "registration must return distinct, valid handles");
        check(schedule.order_after(late, early), "order_after must accept two live systems");
        check(!schedule.order_after(late, late), "a system cannot be ordered after itself");
        check(!schedule.order_after(late, SystemHandle{}), "an empty handle is not a valid dependency");
        schedule.run(world);
        check(order_log == "EL", "order_after must run the dependency first");
    }

    // Disabling and removing.
    {
        Schedule schedule = make_schedule();
        order_log.clear();
        const SystemHandle first = schedule.add_system([](WriteResource<A>) noexcept { order_log += 'F'; });
        const SystemHandle second = schedule.add_system([](WriteResource<B>) noexcept { order_log += 'S'; });
        check(schedule.system_enabled(first), "a new system is enabled");
        check(schedule.set_system_enabled(first, false), "disabling a live system succeeds");
        check(!schedule.system_enabled(first), "a disabled system reports disabled");
        schedule.run(world);
        check(order_log == "S", "a disabled system must not run");
        check(schedule.set_system_enabled(first, true), "re-enabling succeeds");
        order_log.clear();
        schedule.run(world);
        check(order_log == "FS", "a re-enabled system runs again");

        check(schedule.remove_system(first), "removing a live system succeeds");
        check(!schedule.remove_system(first), "removing a stale handle reports failure");
        check(schedule.system_count() == 1, "one system remains after removal");
        order_log.clear();
        schedule.run(world);
        check(order_log == "S", "a removed system must not run");
        (void)second;
    }

    // Removing the dependency of an ordered system leaves the ordering satisfied.
    {
        Schedule schedule = make_schedule();
        order_log.clear();
        const SystemHandle dependency = schedule.add_system([](WriteResource<B>) noexcept { order_log += 'D'; });
        const SystemHandle dependent = schedule.add_system([](WriteResource<A>) noexcept { order_log += 'X'; });
        check(schedule.order_after(dependent, dependency), "ordering succeeds");
        check(schedule.remove_system(dependency), "removing the dependency succeeds");
        schedule.run(world);
        check(order_log == "X", "a dependent whose dependency was removed still runs");
    }

    return failures == 0 ? 0 : 1;
}
