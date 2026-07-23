#include <Ecs/src/System.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace TestTypes {

    struct Position {
        std::uint32_t id = 0;
        std::uint32_t updates = 0;
    };

    struct Marker00 {};
    struct Marker01 {};
    struct Marker02 {};
    struct Marker03 {};
    struct Marker04 {};
    struct Marker05 {};
    struct Marker06 {};
    struct Marker07 {};
    struct Marker08 {};
    struct Marker09 {};
    struct Marker10 {};
    struct Marker11 {};
    struct Marker12 {};
    struct Marker13 {};
    struct Marker14 {};
    struct Marker15 {};

} // namespace TestTypes

SFT_ECS_COMPONENT(TestTypes::Position, "sturdy.test.position");
SFT_ECS_COMPONENT(TestTypes::Marker00, "sturdy.test.marker00");
SFT_ECS_COMPONENT(TestTypes::Marker01, "sturdy.test.marker01");
SFT_ECS_COMPONENT(TestTypes::Marker02, "sturdy.test.marker02");
SFT_ECS_COMPONENT(TestTypes::Marker03, "sturdy.test.marker03");
SFT_ECS_COMPONENT(TestTypes::Marker04, "sturdy.test.marker04");
SFT_ECS_COMPONENT(TestTypes::Marker05, "sturdy.test.marker05");
SFT_ECS_COMPONENT(TestTypes::Marker06, "sturdy.test.marker06");
SFT_ECS_COMPONENT(TestTypes::Marker07, "sturdy.test.marker07");
SFT_ECS_COMPONENT(TestTypes::Marker08, "sturdy.test.marker08");
SFT_ECS_COMPONENT(TestTypes::Marker09, "sturdy.test.marker09");
SFT_ECS_COMPONENT(TestTypes::Marker10, "sturdy.test.marker10");
SFT_ECS_COMPONENT(TestTypes::Marker11, "sturdy.test.marker11");
SFT_ECS_COMPONENT(TestTypes::Marker12, "sturdy.test.marker12");
SFT_ECS_COMPONENT(TestTypes::Marker13, "sturdy.test.marker13");
SFT_ECS_COMPONENT(TestTypes::Marker14, "sturdy.test.marker14");
SFT_ECS_COMPONENT(TestTypes::Marker15, "sturdy.test.marker15");

namespace {

    using SFT::Ecs::ComponentRegistry;
    using SFT::Ecs::Entity;
    using SFT::Ecs::Schedule;
    using SFT::Ecs::ScheduleConfig;
    using SFT::Ecs::World;
    using TestTypes::Position;

    bool check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }

    template <class Marker>
    void spawn_rows(World &world, std::uint32_t count, std::uint32_t &next_value) {
        for (std::uint32_t row = 0; row < count; ++row) {
            (void)world.spawn(Position{.id = next_value++}, Marker{});
        }
    }

    void spawn_fragmented_rows(World &world, std::uint32_t rows_per_archetype) {
        std::uint32_t next_value = 0;
        spawn_rows<TestTypes::Marker00>(world, rows_per_archetype, next_value);
        spawn_rows<TestTypes::Marker01>(world, rows_per_archetype, next_value);
        spawn_rows<TestTypes::Marker02>(world, rows_per_archetype, next_value);
        spawn_rows<TestTypes::Marker03>(world, rows_per_archetype, next_value);
        spawn_rows<TestTypes::Marker04>(world, rows_per_archetype, next_value);
        spawn_rows<TestTypes::Marker05>(world, rows_per_archetype, next_value);
        spawn_rows<TestTypes::Marker06>(world, rows_per_archetype, next_value);
        spawn_rows<TestTypes::Marker07>(world, rows_per_archetype, next_value);
        spawn_rows<TestTypes::Marker08>(world, rows_per_archetype, next_value);
        spawn_rows<TestTypes::Marker09>(world, rows_per_archetype, next_value);
        spawn_rows<TestTypes::Marker10>(world, rows_per_archetype, next_value);
        spawn_rows<TestTypes::Marker11>(world, rows_per_archetype, next_value);
        spawn_rows<TestTypes::Marker12>(world, rows_per_archetype, next_value);
        spawn_rows<TestTypes::Marker13>(world, rows_per_archetype, next_value);
        spawn_rows<TestTypes::Marker14>(world, rows_per_archetype, next_value);
        spawn_rows<TestTypes::Marker15>(world, rows_per_archetype, next_value);
    }

    bool fragmented_query_uses_global_budget() {
        constexpr std::uint32_t archetype_count = 16;
        constexpr std::uint32_t rows_per_archetype = 4096;
        constexpr std::uint32_t total_rows = archetype_count * rows_per_archetype;
        constexpr std::size_t old_parallel_rows = rows_per_archetype / 32 +
                                                  static_cast<std::size_t>(rows_per_archetype % 32 != 0);
        constexpr std::size_t old_rows_per_chunk = std::max<std::size_t>(128, old_parallel_rows);
        constexpr std::size_t old_per_archetype_chunk_count =
            archetype_count * (rows_per_archetype / old_rows_per_chunk +
                               static_cast<std::size_t>(rows_per_archetype % old_rows_per_chunk != 0));

        ComponentRegistry registry;
        World world{registry};
        spawn_fragmented_rows(world, rows_per_archetype);

        bool passed = true;
        {
            std::vector<SFT::Ecs::Query<Position>::Chunk> chunks;
            {
                auto query = world.query<Position>();
                chunks = query.chunks(128, 32);
            }
            passed &= check(old_per_archetype_chunk_count == 512, "derived legacy task count is incorrect");
            passed &= check(chunks.size() == 32, "fragmented query did not target the global parallelism budget");

            std::vector<std::uint8_t> visits(total_rows, 0);
            std::size_t visited_rows = 0;
            bool values_in_range = true;
            for (const auto &chunk : chunks) {
                visited_rows += chunk.size();
                chunk.each([&](Entity, Position &position) noexcept {
                    if (position.id >= visits.size()) {
                        values_in_range = false;
                        return;
                    }
                    ++visits[position.id];
                });
            }

            passed &= check(visited_rows == total_rows, "fragmented chunks did not cover every row");
            passed &= check(values_in_range, "fragmented query returned a corrupt position value");
            passed &= check(
                std::all_of(visits.begin(), visits.end(), [](std::uint8_t visits_for_row) { return visits_for_row == 1; }),
                "fragmented chunks did not visit every row exactly once");
        }

        SFT::Async::Scheduler::initialize(4);
        Schedule schedule{ScheduleConfig{.minimum_rows_per_task = 128, .tasks_per_worker = 8}};
        schedule.add_system([](Entity, Position &position) noexcept { ++position.updates; });
        schedule.run(world);
        SFT::Async::Scheduler::shutdown();

        {
            auto query = world.query<const Position>();
            std::vector<std::uint8_t> updated_ids(total_rows, 0);
            std::size_t updated_rows = 0;
            bool values_updated_once = true;
            for (auto [entity, position] : query) {
                (void)entity;
                if (position.id >= updated_ids.size()) {
                    values_updated_once = false;
                } else {
                    ++updated_ids[position.id];
                }
                values_updated_once &= position.updates == 1;
                ++updated_rows;
            }
            values_updated_once &= std::all_of(
                updated_ids.begin(), updated_ids.end(), [](std::uint8_t visits_for_row) { return visits_for_row == 1; });
            passed &= check(updated_rows == total_rows, "scheduled system did not process every fragmented row");
            passed &= check(values_updated_once, "scheduled system did not update every fragmented row exactly once");
        }

        if (passed) {
            std::cout << "Fragmented query chunks/tasks: " << old_per_archetype_chunk_count << " -> 32\n";
        }
        return passed;
    }

    bool single_archetype_and_normalized_inputs() {
        constexpr std::uint32_t row_count = 4096;
        ComponentRegistry registry;
        World world{registry};
        for (std::uint32_t row = 0; row < row_count; ++row) {
            (void)world.spawn(Position{.id = row});
        }

        auto query = world.query<Position>();
        auto parallel_chunks = query.chunks(128, 32);
        auto normalized_chunks = query.chunks(0, 0);
        return check(parallel_chunks.size() == 32, "single-archetype partition changed unexpectedly") &&
               check(normalized_chunks.size() == 1, "zero chunk arguments were not normalized to one");
    }

    bool uneven_archetypes_preserve_boundaries() {
        constexpr std::size_t total_rows = 1 + 129 + 257;
        ComponentRegistry registry;
        World world{registry};
        std::uint32_t next_value = 0;
        spawn_rows<TestTypes::Marker00>(world, 1, next_value);
        spawn_rows<TestTypes::Marker01>(world, 129, next_value);
        spawn_rows<TestTypes::Marker02>(world, 257, next_value);

        auto query = world.query<Position>();
        auto chunks = query.chunks(64, 4);
        std::size_t covered_rows = 0;
        bool chunk_sizes_valid = true;
        for (const auto &chunk : chunks) {
            covered_rows += chunk.size();
            chunk_sizes_valid &= chunk.size() > 0 && chunk.size() <= 97;
        }

        return check(chunks.size() == 6, "uneven archetypes produced an unexpected chunk count") &&
               check(chunks.size() <= 4 + 3 - 1, "uneven archetypes exceeded the boundary overhead limit") &&
               check(covered_rows == total_rows, "uneven archetype chunks did not cover every row") &&
               check(chunk_sizes_valid, "uneven archetype chunk exceeded the global chunk size");
    }

    bool empty_query_has_no_chunks() {
        ComponentRegistry registry;
        World world{registry};
        auto query = world.query<Position>();
        return check(query.chunks(128, 32).empty(), "empty query produced work chunks");
    }

} // namespace

int main() {
    bool passed = true;
    passed &= fragmented_query_uses_global_budget();
    passed &= single_archetype_and_normalized_inputs();
    passed &= uneven_archetypes_preserve_boundaries();
    passed &= empty_query_has_no_chunks();

    if (passed) {
        std::cout << "ECS global query chunking tests passed.\n";
        return 0;
    }
    return 1;
}
