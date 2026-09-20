/// Coverage for `ArrayReflection.hpp`'s `java.lang.reflect.Array`-style ergonomic layer:
/// `ReflectedSequence`/`ReflectedMap`/`ReflectedSet`/`ReflectedOptional` wrapping real
/// `std::vector`/`std::array`/`std::unordered_map`/`std::set`/`std::optional` fields, plus
/// `dispatch_reflected_field` routing each field to the right wrapper (and to none for a plain
/// scalar field).

#include <array>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

#include <Reflection/ArrayReflection.hpp>
#include <Reflection/Reflection.hpp>

namespace {

    int failures = 0;

    /// Records a failed expectation. Deliberately not `assert` — see `TypeRegistryTest.cpp`'s
    /// identical helper's doc comment: these checks must hold under `NDEBUG` too.
    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "ArrayReflectionTest: %s\n", description);
            ++failures;
        }
    }

    struct Loadout {
        std::vector<int> ammo_counts;
        std::array<int, 4> hotbar{};
        std::unordered_map<int, int> upgrade_levels;
        std::set<int> unlocked_perks;
        std::optional<int> equipped_charm;
        int plain_score = 0;
    };

} // namespace

SFT_REFLECT_TYPE(Loadout, "test.reflection.array_reflection.loadout");
SFT_REFLECT_FIELD(ammo_counts);
SFT_REFLECT_FIELD(hotbar);
SFT_REFLECT_FIELD(upgrade_levels);
SFT_REFLECT_FIELD(unlocked_perks);
SFT_REFLECT_FIELD(equipped_charm);
SFT_REFLECT_FIELD(plain_score);
SFT_REFLECT_END();

int main() {
    using namespace SFT::Reflection;

    TypeRegistry &registry = TypeRegistry::instance();
    const TypeInfo &loadout_type = registry.type<Loadout>();

    const FieldInfo *ammo_field = loadout_type.find_field("ammo_counts");
    const FieldInfo *hotbar_field = loadout_type.find_field("hotbar");
    const FieldInfo *upgrades_field = loadout_type.find_field("upgrade_levels");
    const FieldInfo *perks_field = loadout_type.find_field("unlocked_perks");
    const FieldInfo *charm_field = loadout_type.find_field("equipped_charm");
    const FieldInfo *score_field = loadout_type.find_field("plain_score");

    check(ammo_field != nullptr && ammo_field->container != nullptr, "ammo_counts must carry a ContainerInfo");
    check(hotbar_field != nullptr && hotbar_field->container != nullptr, "hotbar must carry a ContainerInfo");
    check(upgrades_field != nullptr && upgrades_field->map != nullptr, "upgrade_levels must carry a MapInfo");
    check(perks_field != nullptr && perks_field->set != nullptr, "unlocked_perks must carry a SetInfo");
    check(charm_field != nullptr && charm_field->optional != nullptr, "equipped_charm must carry an OptionalInfo");
    check(score_field != nullptr, "plain_score must be found");

    Loadout loadout{};

    // ── ReflectedSequence: std::vector<int> ───────────────────────────────────────────────────
    {
        ReflectedSequence ammo{*ammo_field->container, &loadout.ammo_counts};
        check(!ammo.is_fixed_size(), "std::vector must not report fixed_size");
        check(ammo.length() == 0, "a freshly constructed vector field must start empty");
        check(ammo.resize(3), "resizing a std::vector must succeed");
        check(ammo.length() == 3, "resize must actually change the reported length");

        const int first_value = 11;
        check(ammo.set(0, static_cast<const void *>(&first_value)), "raw set must succeed for a valid index");
        int read_back = 0;
        check(ammo.get(0, static_cast<void *>(&read_back)), "raw get must succeed for a valid index");
        check(read_back == 11, "raw get must observe the value raw set wrote");

        check(!ammo.get(3, static_cast<void *>(&read_back)), "raw get must fail for an out-of-range index");
        check(!ammo.set(99, static_cast<const void *>(&first_value)), "raw set must fail for an out-of-range index");

        check(ammo.set<int>(1, 42), "typed set<int> must succeed for the real element type");
        const std::optional<int> typed_value = ammo.get<int>(1);
        check(typed_value.has_value() && *typed_value == 42, "typed get<int> must observe the value typed set wrote");

        check(!ammo.get<float>(1).has_value(), "typed get<float> must fail against an int-element container");
        check(!ammo.set<float>(1, 1.0f), "typed set<float> must fail against an int-element container");
        check(!ammo.get<int>(50).has_value(), "typed get<int> must fail for an out-of-range index");

        const std::span<const int> view = ammo.view<int>();
        check(view.size() == ammo.length(), "view<int> must span the whole container");
        check(!view.empty() && view[1] == 42, "view<int> must observe live container contents");
    }

    // ── ReflectedSequence: std::array<int, 4> (fixed size) ────────────────────────────────────
    {
        ReflectedSequence hotbar{*hotbar_field->container, &loadout.hotbar};
        check(hotbar.is_fixed_size(), "std::array must report fixed_size");
        check(hotbar.length() == 4, "std::array's length must be its fixed extent");
        check(hotbar.resize(4), "resizing a std::array to its own size must succeed");
        check(!hotbar.resize(5), "resizing a std::array to a different size must fail");
        check(!hotbar.resize(0), "resizing a std::array to zero must fail");

        check(hotbar.set<int>(2, 7), "typed set<int> must succeed on a std::array element");
        const std::optional<int> slot = hotbar.get<int>(2);
        check(slot.has_value() && *slot == 7, "typed get<int> must observe the value written into a std::array");
        check(loadout.hotbar[2] == 7, "the underlying std::array must actually be mutated");
    }

    // ── ReflectedMap: std::unordered_map<int, int> ────────────────────────────────────────────
    {
        ReflectedMap upgrades{*upgrades_field->map, &loadout.upgrade_levels};
        check(upgrades.size() == 0, "a freshly constructed map field must start empty");

        check(upgrades.insert_or_assign<int, int>(1, 10), "typed insert_or_assign must succeed for matching key/value types");
        check(upgrades.insert_or_assign<int, int>(2, 20), "typed insert_or_assign must succeed for a second entry");
        check(upgrades.size() == 2, "size must reflect both inserted entries");

        const std::optional<int> found = upgrades.find<int, int>(1);
        check(found.has_value() && *found == 10, "typed find must return the value for a present key");
        check(!upgrades.find<int, int>(999).has_value(), "typed find must fail for an absent key");

        check(!upgrades.insert_or_assign<float, int>(1.0f, 5), "typed insert_or_assign must fail on a key-type mismatch");
        check(!upgrades.find<int, float>(1).has_value(), "typed find must fail on a value-type mismatch");

        int visited_count = 0;
        int value_sum = 0;
        upgrades.for_each<int, int>([&](int key, int value) {
            ++visited_count;
            value_sum += key + value;
        });
        check(visited_count == 2, "typed for_each must visit every entry exactly once");
        check(value_sum == (1 + 10) + (2 + 20), "typed for_each must observe real key/value pairs");

        check(upgrades.erase<int>(1), "typed erase must succeed for a present key");
        check(upgrades.size() == 1, "erase must actually remove the entry");
        check(!upgrades.erase<int>(1), "typed erase must fail for an already-absent key");

        check(upgrades.clear(), "clear must succeed");
        check(upgrades.size() == 0, "clear must actually empty the map");
    }

    // ── ReflectedSet: std::set<int> ────────────────────────────────────────────────────────────
    {
        ReflectedSet perks{*perks_field->set, &loadout.unlocked_perks};
        check(perks.size() == 0, "a freshly constructed set field must start empty");

        check(perks.insert<int>(5), "typed insert must succeed");
        check(perks.insert<int>(9), "typed insert must succeed for a second element");
        check(perks.size() == 2, "size must reflect both inserted elements");
        check(perks.contains<int>(5), "typed contains must find an inserted element");
        check(!perks.contains<int>(42), "typed contains must not find an absent element");
        check(!perks.insert<float>(1.0f), "typed insert must fail on a type mismatch");

        int visited_count = 0;
        int element_sum = 0;
        perks.for_each<int>([&](int element) {
            ++visited_count;
            element_sum += element;
        });
        check(visited_count == 2, "typed for_each must visit every element exactly once");
        check(element_sum == 5 + 9, "typed for_each must observe real elements");

        check(perks.erase<int>(5), "typed erase must succeed for a present element");
        check(perks.size() == 1, "erase must actually remove the element");
        check(!perks.erase<int>(5), "typed erase must fail for an already-absent element");
    }

    // ── ReflectedOptional: std::optional<int> ─────────────────────────────────────────────────
    {
        ReflectedOptional charm{*charm_field->optional, &loadout.equipped_charm};
        check(!charm.has_value(), "a default-constructed std::optional field must start empty");
        check(!charm.get<int>().has_value(), "typed get must fail while empty");

        check(charm.set<int>(77), "typed set must succeed");
        check(charm.has_value(), "has_value must be true after a typed set");
        const std::optional<int> value = charm.get<int>();
        check(value.has_value() && *value == 77, "typed get must observe the value typed set wrote");
        check(loadout.equipped_charm.has_value() && *loadout.equipped_charm == 77, "the underlying std::optional must actually hold the value");

        check(!charm.set<float>(1.0f), "typed set must fail on a type mismatch");

        int *mutable_ptr = charm.mutable_data_as<int>();
        check(mutable_ptr != nullptr, "mutable_data_as<int> must succeed for a populated, matching-type optional");
        *mutable_ptr = 88;
        check(loadout.equipped_charm.has_value() && *loadout.equipped_charm == 88, "mutating through mutable_data_as must reach the real storage");

        check(charm.reset(), "reset must succeed");
        check(!charm.has_value(), "has_value must be false after reset");
        check(charm.mutable_data_as<int>() == nullptr, "mutable_data_as<int> must return null once empty");
    }

    // ── dispatch_reflected_field ──────────────────────────────────────────────────────────────
    {
        int sequence_hits = 0;
        int map_hits = 0;
        int set_hits = 0;
        int optional_hits = 0;

        auto visitor = [&](auto &&reflected) {
            using Reflected = std::decay_t<decltype(reflected)>;
            if constexpr (std::is_same_v<Reflected, ReflectedSequence>) {
                ++sequence_hits;
            } else if constexpr (std::is_same_v<Reflected, ReflectedMap>) {
                ++map_hits;
            } else if constexpr (std::is_same_v<Reflected, ReflectedSet>) {
                ++set_hits;
            } else if constexpr (std::is_same_v<Reflected, ReflectedOptional>) {
                ++optional_hits;
            }
        };

        check(dispatch_reflected_field(*ammo_field, &loadout, visitor), "dispatch must route a std::vector field to a wrapper");
        check(dispatch_reflected_field(*hotbar_field, &loadout, visitor), "dispatch must route a std::array field to a wrapper");
        check(dispatch_reflected_field(*upgrades_field, &loadout, visitor), "dispatch must route a std::unordered_map field to a wrapper");
        check(dispatch_reflected_field(*perks_field, &loadout, visitor), "dispatch must route a std::set field to a wrapper");
        check(dispatch_reflected_field(*charm_field, &loadout, visitor), "dispatch must route a std::optional field to a wrapper");
        check(!dispatch_reflected_field(*score_field, &loadout, visitor), "dispatch must decline a plain scalar field");

        check(sequence_hits == 2, "dispatch must route exactly the two sequence-shaped fields (vector + array) to ReflectedSequence");
        check(map_hits == 1, "dispatch must route exactly one field to ReflectedMap");
        check(set_hits == 1, "dispatch must route exactly one field to ReflectedSet");
        check(optional_hits == 1, "dispatch must route exactly one field to ReflectedOptional");

        check(reflected_field_shape(*ammo_field) == ReflectedFieldShape::Sequence, "reflected_field_shape must classify the vector field as Sequence");
        check(reflected_field_shape(*upgrades_field) == ReflectedFieldShape::Map, "reflected_field_shape must classify the map field as Map");
        check(reflected_field_shape(*perks_field) == ReflectedFieldShape::Set, "reflected_field_shape must classify the set field as Set");
        check(reflected_field_shape(*charm_field) == ReflectedFieldShape::Optional, "reflected_field_shape must classify the optional field as Optional");
        check(reflected_field_shape(*score_field) == ReflectedFieldShape::None, "reflected_field_shape must classify a plain scalar field as None");

        check(as_reflected_sequence(*ammo_field, &loadout).has_value(), "as_reflected_sequence must succeed for a sequence-shaped field");
        check(!as_reflected_sequence(*score_field, &loadout).has_value(), "as_reflected_sequence must fail for a plain scalar field");
        check(as_reflected_map(*upgrades_field, &loadout).has_value(), "as_reflected_map must succeed for a map-shaped field");
        check(as_reflected_set(*perks_field, &loadout).has_value(), "as_reflected_set must succeed for a set-shaped field");
        check(as_reflected_optional(*charm_field, &loadout).has_value(), "as_reflected_optional must succeed for an optional-shaped field");
    }

    if (failures != 0) {
        (void)std::fprintf(stderr, "ArrayReflectionTest: %d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
