/// Coverage for `std::optional<T>` field introspection and serialization.

#include <cstdio>
#include <optional>

#include <Reflection/Reflection.hpp>

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "OptionalTest: %s\n", description);
            ++failures;
        }
    }

    struct Widget {
        std::optional<int> power;
        std::optional<UString> nickname;
    };

} // namespace

SFT_REFLECT_TYPE(Widget, "test.reflection.optional.widget");
SFT_REFLECT_FIELD(power);
SFT_REFLECT_FIELD(nickname);
SFT_REFLECT_END();

int main() {
    using namespace SFT::Reflection;

    TypeRegistry &registry = TypeRegistry::instance();
    const TypeInfo &type = registry.type<Widget>();

    // ── Introspection ──────────────────────────────────────────────────────────────────────────
    const FieldInfo *power_field = type.find_field("power");
    check(power_field != nullptr && power_field->optional != nullptr,
          "a std::optional<int> field must carry a non-null OptionalInfo");
    check(power_field != nullptr && power_field->optional != nullptr && power_field->optional->value_trivial,
          "std::optional<int>'s value type must be recognized as Trivial");
    check(power_field != nullptr && power_field->optional != nullptr &&
              power_field->optional->value_primitive_kind == PrimitiveKind::SignedInt,
          "std::optional<int>'s value type must be classified SignedInt");
    check(power_field != nullptr && power_field->container == nullptr && power_field->map == nullptr,
          "an optional field must not also carry a ContainerInfo/MapInfo");

    const FieldInfo *nickname_field = type.find_field("nickname");
    check(nickname_field != nullptr && nickname_field->optional != nullptr && !nickname_field->optional->value_trivial,
          "std::optional<UString>'s value type must be recognized as non-Trivial");

    // ── Free-function access ──────────────────────────────────────────────────────────────────
    Widget widget{};
    check(power_field != nullptr && !optional_has_value(power_field->optional, &widget.power),
          "a default-constructed optional must report no value");

    const int new_power = 42;
    check(power_field != nullptr && optional_set(power_field->optional, &widget.power, &new_power),
          "optional_set must succeed");
    check(widget.power.has_value() && *widget.power == 42, "optional_set must mutate the live optional");
    check(power_field != nullptr && optional_has_value(power_field->optional, &widget.power),
          "optional_has_value must report true after optional_set");

    int read_power = 0;
    check(power_field != nullptr && optional_get(power_field->optional, &widget.power, &read_power),
          "optional_get must succeed for a present Trivial value");
    check(read_power == 42, "optional_get must return the live value");

    check(power_field != nullptr && optional_reset(power_field->optional, &widget.power), "optional_reset must succeed");
    check(!widget.power.has_value(), "optional_reset must clear the live optional");
    check(power_field != nullptr && !optional_get(power_field->optional, &widget.power, &read_power),
          "optional_get must fail cleanly for an absent value");

    // ── Serialization: present and absent, Trivial and non-Trivial ───────────────────────────────
    Widget present_widget{.power = 7, .nickname = UString{"sparky"}};
    std::vector<std::byte> present_bytes;
    check(serialize_to_bytes(type, &present_widget, present_bytes).has_value(),
          "serializing a widget with both optionals present must succeed");

    Widget restored_present{};
    usize present_consumed = 0;
    check(deserialize_from_bytes(type, &restored_present, present_bytes, present_consumed).has_value(),
          "deserializing a widget with both optionals present must succeed");
    check(present_consumed == present_bytes.size(), "deserializing must consume exactly what was serialized");
    check(restored_present.power.has_value() && *restored_present.power == 7,
          "deserializing must restore a present Trivial optional's value");
    check(restored_present.nickname.has_value() && restored_present.nickname->cpp_string_view() == "sparky",
          "deserializing must restore a present non-Trivial optional's value");

    Widget absent_widget{};
    std::vector<std::byte> absent_bytes;
    check(serialize_to_bytes(type, &absent_widget, absent_bytes).has_value(),
          "serializing a widget with both optionals absent must succeed");

    Widget restored_absent{.power = 999, .nickname = UString{"stale"}};
    usize absent_consumed = 0;
    check(deserialize_from_bytes(type, &restored_absent, absent_bytes, absent_consumed).has_value(),
          "deserializing a widget with both optionals absent must succeed");
    check(!restored_absent.power.has_value(), "deserializing an absent Trivial optional must clear a stale value");
    check(!restored_absent.nickname.has_value(), "deserializing an absent non-Trivial optional must clear a stale value");

    if (failures != 0) {
        (void)std::fprintf(stderr, "OptionalTest: %d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
