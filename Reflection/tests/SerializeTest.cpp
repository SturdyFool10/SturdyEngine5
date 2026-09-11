/// Coverage for the reflection-based binary serializer: trivial fields, `UString` fields, nested
/// reflected structs (recursing through `FieldInfo::field_type` back into `TypeRegistry`),
/// `std::vector<T>` container fields for both Trivial and non-Trivial (`UString`) element types,
/// and the clean-error path for a field type the serializer has no generic way to walk.

#include <cstdio>
#include <unordered_map>
#include <vector>

#include <Reflection/Reflection.hpp>

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "SerializeTest: %s\n", description);
            ++failures;
        }
    }

    struct Position {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct Player {
        int health = 100;
        Position position;
        UString name;
    };

    /// A field type the serializer cannot walk generically: not Trivial, not UString, and not
    /// itself registered.
    struct Unsupported {
        std::vector<int> values;
    };

    struct HasUnsupportedField {
        Unsupported opaque;
    };

    struct Inventory {
        std::vector<int> scores;
    };

    /// A container of non-trivial (`UString`) elements — fully supported by the serializer.
    struct HasNonTrivialContainer {
        std::vector<UString> tags;
    };

    struct Scoreboard {
        std::unordered_map<int, int> trivial_scores;
        std::unordered_map<UString, UString> name_tags;
    };

    /// A map whose value type has no nested `TypeInfo` at all — deliberately unsupported.
    struct HasUnsupportedMap {
        std::unordered_map<int, Unsupported> opaque_by_id;
    };

} // namespace

SFT_REFLECT_TYPE(Position, "test.reflection.serialize.position");
SFT_REFLECT_FIELD(x);
SFT_REFLECT_FIELD(y);
SFT_REFLECT_END();

SFT_REFLECT_TYPE(Player, "test.reflection.serialize.player");
SFT_REFLECT_FIELD(health);
SFT_REFLECT_FIELD(position);
SFT_REFLECT_FIELD(name);
SFT_REFLECT_END();

// `Unsupported` is deliberately NOT reflected (no SFT_REFLECT_TYPE) — that's what makes it
// unsupported: its field below falls back to a typeid-derived TypeId that TypeRegistry has no
// entry for, rather than colliding with a real registration.

SFT_REFLECT_TYPE(HasUnsupportedField, "test.reflection.serialize.has_unsupported_field");
SFT_REFLECT_FIELD(opaque);
SFT_REFLECT_END();

SFT_REFLECT_TYPE(Inventory, "test.reflection.serialize.inventory");
SFT_REFLECT_FIELD(scores);
SFT_REFLECT_END();

SFT_REFLECT_TYPE(HasNonTrivialContainer, "test.reflection.serialize.has_non_trivial_container");
SFT_REFLECT_FIELD(tags);
SFT_REFLECT_END();

SFT_REFLECT_TYPE(Scoreboard, "test.reflection.serialize.scoreboard");
SFT_REFLECT_FIELD(trivial_scores);
SFT_REFLECT_FIELD(name_tags);
SFT_REFLECT_END();

SFT_REFLECT_TYPE(HasUnsupportedMap, "test.reflection.serialize.has_unsupported_map");
SFT_REFLECT_FIELD(opaque_by_id);
SFT_REFLECT_END();

int main() {
    using namespace SFT::Reflection;

    TypeRegistry &registry = TypeRegistry::instance();
    const TypeInfo &player_type = registry.type<Player>();
    (void)registry.type<Position>();
    const TypeInfo &has_unsupported_type = registry.type<HasUnsupportedField>();

    // ── Round-trip: primitives, a nested reflected struct, and a UString ────────────────────────
    Player original{};
    original.health = 42;
    original.position = Position{.x = 1.5f, .y = -2.5f};
    original.name = UString{"reflected player"};

    std::vector<std::byte> bytes;
    const auto written = serialize_to_bytes(player_type, &original, bytes);
    check(written.has_value(), "serializing a Player must succeed");

    Player restored{};
    usize consumed = 0;
    const auto read = deserialize_from_bytes(player_type, &restored, bytes, consumed);
    check(read.has_value(), "deserializing a Player must succeed");
    check(consumed == bytes.size(), "deserializing must consume exactly what was serialized");

    check(restored.health == 42, "deserializing must restore the trivial field");
    check(restored.position.x == 1.5f && restored.position.y == -2.5f,
          "deserializing must restore the nested reflected struct's fields");
    check(restored.name.cpp_string_view() == "reflected player", "deserializing must restore the UString field");

    // ── Serializing two values back-to-back and reading them back in sequence ───────────────────
    Player second{.health = 7, .position = Position{.x = 0.0f, .y = 0.0f}, .name = UString{"second"}};
    const auto written_second = serialize_to_bytes(player_type, &second, bytes);
    check(written_second.has_value(), "appending a second serialized value must succeed");

    Player restored_first{};
    usize first_consumed = 0;
    check(deserialize_from_bytes(player_type, &restored_first, bytes, first_consumed).has_value(),
          "reading the first value back out of a two-value buffer must succeed");
    check(restored_first.health == 42, "the first value read back must be the first one written");

    Player restored_second{};
    usize second_consumed = 0;
    const std::span<const std::byte> remaining(bytes.data() + first_consumed, bytes.size() - first_consumed);
    check(deserialize_from_bytes(player_type, &restored_second, remaining, second_consumed).has_value(),
          "reading the second value back out of a two-value buffer must succeed");
    check(restored_second.health == 7 && restored_second.name.cpp_string_view() == "second",
          "the second value read back must be the second one written");

    // ── Unsupported field type: clean error, not a crash ─────────────────────────────────────────
    HasUnsupportedField unsupported_instance{};
    std::vector<std::byte> unsupported_bytes;
    const auto unsupported_write = serialize_to_bytes(has_unsupported_type, &unsupported_instance, unsupported_bytes);
    check(!unsupported_write.has_value(), "serializing a field with no generic walk must fail cleanly");
    check(!unsupported_write.has_value() || unsupported_write.error().code == SerializeErrorCode::NestedTypeNotRegistered,
          "an unwalkable field must be reported as NestedTypeNotRegistered");

    // ── Container introspection: a Trivial-element std::vector<int> field ────────────────────────
    const TypeInfo &inventory_type = registry.type<Inventory>();
    const FieldInfo *scores_field = inventory_type.find_field("scores");
    check(scores_field != nullptr, "scores field must be found by name");
    check(scores_field != nullptr && scores_field->container != nullptr,
          "a std::vector<int> field must carry a non-null ContainerInfo");
    check(scores_field != nullptr && scores_field->container != nullptr && scores_field->container->element_trivial,
          "std::vector<int>'s element type must be recognized as Trivial");
    check(scores_field != nullptr && scores_field->container != nullptr &&
              scores_field->container->element_type == type_id_for<int>(),
          "the container's element_type must identify int correctly");

    Inventory inventory{.scores = {3, 1, 4, 1, 5}};
    check(scores_field != nullptr && container_size(scores_field->container, &inventory.scores) == 5,
          "container_size must report the live element count");

    int read_element = 0;
    check(scores_field != nullptr &&
              container_get_element(scores_field->container, &inventory.scores, 2, &read_element),
          "container_get_element must succeed for a valid index");
    check(read_element == 4, "container_get_element must return the live element value");

    const int new_element = 99;
    check(scores_field != nullptr &&
              container_set_element(scores_field->container, &inventory.scores, 0, &new_element),
          "container_set_element must succeed for a valid index");
    check(inventory.scores[0] == 99, "container_set_element must mutate the live vector");

    check(scores_field != nullptr && !container_get_element(scores_field->container, &inventory.scores, 99, &read_element),
          "container_get_element must reject an out-of-range index instead of reading OOB");

    check(scores_field != nullptr && container_resize(scores_field->container, &inventory.scores, 2),
          "container_resize must succeed");
    check(inventory.scores.size() == 2, "container_resize must actually resize the live vector");

    std::vector<std::byte> inventory_bytes;
    Inventory inventory_to_serialize{.scores = {10, 20, 30}};
    check(serialize_to_bytes(inventory_type, &inventory_to_serialize, inventory_bytes).has_value(),
          "serializing a Trivial-element vector field must succeed");

    Inventory restored_inventory{};
    usize inventory_consumed = 0;
    check(deserialize_from_bytes(inventory_type, &restored_inventory, inventory_bytes, inventory_consumed).has_value(),
          "deserializing a Trivial-element vector field must succeed");
    check(restored_inventory.scores == std::vector<int>{10, 20, 30},
          "deserializing a vector field must restore its exact contents and length");

    // ── A container of non-Trivial elements (std::vector<UString>) round-trips too ────────────────
    const TypeInfo &non_trivial_container_type = registry.type<HasNonTrivialContainer>();
    const FieldInfo *tags_field = non_trivial_container_type.find_field("tags");
    check(tags_field != nullptr && tags_field->container != nullptr && !tags_field->container->element_trivial,
          "std::vector<UString>'s element type must be recognized as non-Trivial");

    HasNonTrivialContainer non_trivial_instance{.tags = {UString{"a"}, UString{"bb"}, UString{"ccc"}}};
    std::vector<std::byte> non_trivial_bytes;
    const auto non_trivial_write = serialize_to_bytes(non_trivial_container_type, &non_trivial_instance, non_trivial_bytes);
    check(non_trivial_write.has_value(), "serializing a non-Trivial-element container must succeed");

    HasNonTrivialContainer restored_non_trivial{};
    usize non_trivial_consumed = 0;
    const auto non_trivial_read =
        deserialize_from_bytes(non_trivial_container_type, &restored_non_trivial, non_trivial_bytes, non_trivial_consumed);
    check(non_trivial_read.has_value(), "deserializing a non-Trivial-element container must succeed");
    check(non_trivial_consumed == non_trivial_bytes.size(), "deserializing must consume exactly what was serialized");
    check(restored_non_trivial.tags.size() == 3, "deserializing must restore the exact element count");
    check(restored_non_trivial.tags.size() == 3 && restored_non_trivial.tags[0].cpp_string_view() == "a" &&
              restored_non_trivial.tags[1].cpp_string_view() == "bb" && restored_non_trivial.tags[2].cpp_string_view() == "ccc",
          "deserializing must restore every element's exact value, in order");

    // ── Map introspection ─────────────────────────────────────────────────────────────────────────
    const TypeInfo &scoreboard_type = registry.type<Scoreboard>();
    const FieldInfo *trivial_scores_field = scoreboard_type.find_field("trivial_scores");
    check(trivial_scores_field != nullptr && trivial_scores_field->map != nullptr,
          "a std::unordered_map<int, int> field must carry a non-null MapInfo");
    check(trivial_scores_field != nullptr && trivial_scores_field->map != nullptr &&
              trivial_scores_field->map->key_trivial && trivial_scores_field->map->value_trivial,
          "std::unordered_map<int, int>'s key and value types must both be recognized as Trivial");
    check(trivial_scores_field != nullptr && trivial_scores_field->container == nullptr,
          "a map field must not also carry a ContainerInfo");

    const FieldInfo *name_tags_field = scoreboard_type.find_field("name_tags");
    check(name_tags_field != nullptr && name_tags_field->map != nullptr && !name_tags_field->map->key_trivial &&
              !name_tags_field->map->value_trivial,
          "std::unordered_map<UString, UString>'s key and value types must both be recognized as non-Trivial");

    Scoreboard scoreboard{};
    scoreboard.trivial_scores = {{1, 100}, {2, 200}};
    check(trivial_scores_field != nullptr && map_size(trivial_scores_field->map, &scoreboard.trivial_scores) == 2,
          "map_size must report the live entry count");

    const int inserted_key = 3;
    const int inserted_value = 300;
    check(trivial_scores_field != nullptr &&
              map_insert_or_assign(trivial_scores_field->map, &scoreboard.trivial_scores, &inserted_key, &inserted_value),
          "map_insert_or_assign must succeed");
    check(scoreboard.trivial_scores.size() == 3 && scoreboard.trivial_scores.at(3) == 300,
          "map_insert_or_assign must mutate the live map");

    int found_value = 0;
    const int lookup_key = 1;
    check(trivial_scores_field != nullptr &&
              map_find(trivial_scores_field->map, &scoreboard.trivial_scores, &lookup_key, &found_value),
          "map_find must succeed for a present key");
    check(found_value == 100, "map_find must return the live value");

    const int missing_key = 999;
    check(trivial_scores_field != nullptr &&
              !map_find(trivial_scores_field->map, &scoreboard.trivial_scores, &missing_key, &found_value),
          "map_find must fail cleanly for an absent key");

    check(trivial_scores_field != nullptr &&
              map_erase(trivial_scores_field->map, &scoreboard.trivial_scores, &lookup_key),
          "map_erase must succeed for a present key");
    check(scoreboard.trivial_scores.size() == 2, "map_erase must mutate the live map");
    check(trivial_scores_field != nullptr &&
              !map_erase(trivial_scores_field->map, &scoreboard.trivial_scores, &lookup_key),
          "map_erase must fail cleanly for an already-absent key");

    // ── Map serialization: Trivial key/value ──────────────────────────────────────────────────────
    Scoreboard scoreboard_to_serialize{};
    scoreboard_to_serialize.trivial_scores = {{1, 10}, {2, 20}, {3, 30}};
    std::vector<std::byte> scoreboard_bytes;
    check(serialize_to_bytes(scoreboard_type, &scoreboard_to_serialize, scoreboard_bytes).has_value(),
          "serializing a Trivial-key/value map field must succeed");

    Scoreboard restored_scoreboard{};
    usize scoreboard_consumed = 0;
    check(deserialize_from_bytes(scoreboard_type, &restored_scoreboard, scoreboard_bytes, scoreboard_consumed).has_value(),
          "deserializing a Trivial-key/value map field must succeed");
    check(scoreboard_consumed == scoreboard_bytes.size(), "deserializing must consume exactly what was serialized");
    check(restored_scoreboard.trivial_scores.size() == 3, "deserializing must restore the exact entry count");
    check(restored_scoreboard.trivial_scores == scoreboard_to_serialize.trivial_scores,
          "deserializing must restore every key/value pair exactly");

    // ── Map serialization: non-Trivial (UString) key/value ────────────────────────────────────────
    scoreboard_to_serialize.name_tags = {{UString{"alice"}, UString{"wizard"}}, {UString{"bob"}, UString{"warrior"}}};
    std::vector<std::byte> name_tags_bytes;
    check(serialize_to_bytes(scoreboard_type, &scoreboard_to_serialize, name_tags_bytes).has_value(),
          "serializing a non-Trivial-key/value map field must succeed");

    Scoreboard restored_name_tags{};
    usize name_tags_consumed = 0;
    check(deserialize_from_bytes(scoreboard_type, &restored_name_tags, name_tags_bytes, name_tags_consumed).has_value(),
          "deserializing a non-Trivial-key/value map field must succeed");
    check(restored_name_tags.name_tags.size() == 2, "deserializing must restore the exact entry count for a UString-keyed map");
    check(restored_name_tags.name_tags.size() == 2 && restored_name_tags.name_tags.at(UString{"alice"}).cpp_string_view() == "wizard" &&
              restored_name_tags.name_tags.at(UString{"bob"}).cpp_string_view() == "warrior",
          "deserializing must restore every UString key/value pair exactly");

    // ── A map whose value type has no nested TypeInfo is a clean, reported error ─────────────────
    const TypeInfo &unsupported_map_type = registry.type<HasUnsupportedMap>();
    HasUnsupportedMap unsupported_map_instance{};
    unsupported_map_instance.opaque_by_id.emplace(1, Unsupported{});
    std::vector<std::byte> unsupported_map_bytes;
    const auto unsupported_map_write = serialize_to_bytes(unsupported_map_type, &unsupported_map_instance, unsupported_map_bytes);
    check(!unsupported_map_write.has_value(), "serializing a map with an unwalkable value type must fail cleanly");
    check(!unsupported_map_write.has_value() || unsupported_map_write.error().code == SerializeErrorCode::NestedTypeNotRegistered,
          "an unwalkable map value type must be reported as NestedTypeNotRegistered");

    // ── Truncated buffer: clean error, not a crash ───────────────────────────────────────────────
    Player truncated_target{};
    usize truncated_consumed = 0;
    const std::span<const std::byte> truncated(bytes.data(), sizeof(int) / 2);
    const auto truncated_result = deserialize_from_bytes(player_type, &truncated_target, truncated, truncated_consumed);
    check(!truncated_result.has_value(), "deserializing a truncated buffer must fail cleanly");
    check(!truncated_result.has_value() || truncated_result.error().code == SerializeErrorCode::UnexpectedEndOfData,
          "a truncated buffer must be reported as UnexpectedEndOfData");

    if (failures != 0) {
        (void)std::fprintf(stderr, "SerializeTest: %d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
