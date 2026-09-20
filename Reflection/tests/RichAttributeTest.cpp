/// Coverage for the array- and enum-valued attribute kinds added to `Attribute.hpp`
/// (`StaticAttributeKind::Enum`/`Array`, `SFT_ATTR_ENUM`, `SFT_ATTR_INT_ARRAY`). Mirrors
/// `StaticReflectionTest.cpp`'s split: `static_assert`s proving the new kinds are genuinely
/// `consteval`-usable, plus a runtime `main()` cross-checking the same declarations through
/// `TypeRegistry`/`FieldInfo::attributes` the way the older scalar kinds already are.

#include <Reflection/Reflection.hpp>

#include <cstdio>
#include <string_view>
#include <variant>

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
            (void)std::fprintf(stderr, "RichAttributeTest: %s\n", description);
            ++failures;
        }
    }

    enum class DamageType : int {
        Physical = 0,
        Fire = 1,
        Frost = 2,
    };

    struct Weapon {
        int damage = 10;
        int valid_slots = 0;
        int loadout = 0;
    };

} // namespace

// `SFT_ATTR_ENUM` needs `DamageType` to have a stable `TypeId` (`type_id<DamageType>()`), which
// requires it to be `SFT_REFLECT_ENUM`-registered the same way a struct needs `SFT_REFLECT_TYPE` —
// see `StaticTypeId.hpp`'s `HasStableTypeId` concept.
SFT_REFLECT_ENUM(DamageType, "test.reflection.rich_attribute.damage_type");
SFT_REFLECT_ENUM_VALUE(Physical);
SFT_REFLECT_ENUM_VALUE(Fire);
SFT_REFLECT_ENUM_VALUE(Frost);
SFT_REFLECT_ENUM_END();

SFT_REFLECT_TYPE(Weapon, "test.reflection.rich_attribute.weapon");
SFT_REFLECT_FIELD(damage, SFT_ATTR_ENUM("category", DamageType::Fire));
SFT_REFLECT_FIELD(valid_slots, SFT_ATTR_INT_ARRAY("valid_slots", 1, 2, 3));
// Same field, three attribute kinds at once (bool/int-array/enum/string), proving `StaticAttribute`
// with the new members still packs cleanly into `StaticFieldInfo::attribute_storage` and that the
// whole walk stays `consteval` end to end.
SFT_REFLECT_FIELD(loadout,
                   SFT_ATTR_BOOL("modder_visible", true),
                   SFT_ATTR_INT_ARRAY("allowed_slots", 4, 5),
                   SFT_ATTR_ENUM("damage_type", DamageType::Frost),
                   SFT_ATTR_STRING("category", "melee"));
SFT_REFLECT_END();

using namespace SFT::Reflection;

// ── Enum-valued attribute: compile-time ─────────────────────────────────────────────────────────
static_assert(reflect<Weapon>().field<"damage">().attributes().size() == 1);
static_assert(reflect<Weapon>().field<"damage">().find_attribute("category")->kind == StaticAttributeKind::Enum);
static_assert(reflect<Weapon>().field<"damage">().find_attribute("category")->type_value == type_id<DamageType>());
static_assert(reflect<Weapon>().field<"damage">().find_attribute("category")->signed_value
               == static_cast<i64>(DamageType::Fire));

// ── Int-array-valued attribute: compile-time ────────────────────────────────────────────────────
static_assert(reflect<Weapon>().field<"valid_slots">().attributes().size() == 1);
static_assert(reflect<Weapon>().field<"valid_slots">().find_attribute("valid_slots")->kind
               == StaticAttributeKind::Array);
static_assert(reflect<Weapon>().field<"valid_slots">().find_attribute("valid_slots")->array_values().size() == 3);
static_assert(reflect<Weapon>().field<"valid_slots">().find_attribute("valid_slots")->array_values()[0] == 1);
static_assert(reflect<Weapon>().field<"valid_slots">().find_attribute("valid_slots")->array_values()[1] == 2);
static_assert(reflect<Weapon>().field<"valid_slots">().find_attribute("valid_slots")->array_values()[2] == 3);

// ── Mixed kinds on one field: still fully `consteval` ───────────────────────────────────────────
static_assert(reflect<Weapon>().field<"loadout">().attributes().size() == 4);
static_assert(reflect<Weapon>().field<"loadout">().find_attribute("modder_visible")->kind == StaticAttributeKind::Bool);
static_assert(reflect<Weapon>().field<"loadout">().find_attribute("modder_visible")->bool_value);
static_assert(reflect<Weapon>().field<"loadout">().find_attribute("allowed_slots")->kind == StaticAttributeKind::Array);
static_assert(reflect<Weapon>().field<"loadout">().find_attribute("allowed_slots")->array_values().size() == 2);
static_assert(reflect<Weapon>().field<"loadout">().find_attribute("allowed_slots")->array_values()[0] == 4);
static_assert(reflect<Weapon>().field<"loadout">().find_attribute("allowed_slots")->array_values()[1] == 5);
static_assert(reflect<Weapon>().field<"loadout">().find_attribute("damage_type")->kind == StaticAttributeKind::Enum);
static_assert(reflect<Weapon>().field<"loadout">().find_attribute("damage_type")->type_value == type_id<DamageType>());
static_assert(reflect<Weapon>().field<"loadout">().find_attribute("damage_type")->signed_value
               == static_cast<i64>(DamageType::Frost));
static_assert(reflect<Weapon>().field<"loadout">().find_attribute("category")->kind == StaticAttributeKind::String);
static_assert(reflect<Weapon>().field<"loadout">().find_attribute("category")->string_value == "melee");
static_assert(reflect<Weapon>().field<"loadout">().find_attribute("absent") == nullptr);

int main() {
    // Runtime smoke test mirroring the static_asserts above, exercising the same
    // `AttributeFactory::operator()`/`ArrayAttributeFactory::operator()` runtime-degradation paths
    // that back `TypeRegistry`'s `FieldInfo::attributes`.
    const TypeInfo &runtime_type = TypeRegistry::instance().type<Weapon>();

    // Enum: the runtime side degrades to the enumerator's own integer value (see
    // `AttributeFactory::operator()`'s `Enum` branch) — the type identity itself is compile-time
    // only, so only the numeric value is checked here.
    const FieldInfo *runtime_damage = runtime_type.find_field("damage");
    check(runtime_damage != nullptr, "expected a runtime FieldInfo for `damage`");
    if (runtime_damage != nullptr) {
        check(runtime_damage->attributes.size() == reflect<Weapon>().field<"damage">().attributes().size(),
              "runtime and static attribute counts for `damage` must agree");
        const Attribute *runtime_category = find_attribute(*runtime_damage, "category");
        check(runtime_category != nullptr, "expected a runtime `category` attribute on `damage`");
        if (runtime_category != nullptr) {
            check(std::holds_alternative<i64>(runtime_category->value), "runtime enum attribute must degrade to i64");
            check(std::get<i64>(runtime_category->value) == static_cast<i64>(DamageType::Fire),
                  "runtime enum attribute value must match the enumerator's integer value");
        }
    }

    // Int array: the runtime side degrades to a delimited string (see
    // `ArrayAttributeFactory::operator()`) — check it exists, doesn't crash, and renders the
    // expected text, while the full-fidelity array is only asserted at compile time above.
    const FieldInfo *runtime_slots = runtime_type.find_field("valid_slots");
    check(runtime_slots != nullptr, "expected a runtime FieldInfo for `valid_slots`");
    if (runtime_slots != nullptr) {
        const Attribute *runtime_valid_slots = find_attribute(*runtime_slots, "valid_slots");
        check(runtime_valid_slots != nullptr, "expected a runtime `valid_slots` attribute");
        if (runtime_valid_slots != nullptr) {
            check(std::holds_alternative<UString>(runtime_valid_slots->value),
                  "runtime array attribute must degrade to a UString");
            check(std::get<UString>(runtime_valid_slots->value) == UString{"1,2,3"},
                  "runtime array attribute must render as a comma-delimited list");
        }
    }

    // Mixed-kind field: the runtime and static views must still agree on count.
    const FieldInfo *runtime_loadout = runtime_type.find_field("loadout");
    check(runtime_loadout != nullptr, "expected a runtime FieldInfo for `loadout`");
    if (runtime_loadout != nullptr) {
        check(runtime_loadout->attributes.size() == reflect<Weapon>().field<"loadout">().attributes().size(),
              "runtime and static attribute counts for `loadout` must agree");
        const Attribute *runtime_allowed_slots = find_attribute(*runtime_loadout, "allowed_slots");
        check(runtime_allowed_slots != nullptr, "expected a runtime `allowed_slots` attribute");
        if (runtime_allowed_slots != nullptr) {
            check(std::holds_alternative<UString>(runtime_allowed_slots->value),
                  "runtime array attribute must degrade to a UString");
            check(std::get<UString>(runtime_allowed_slots->value) == UString{"4,5"},
                  "runtime array attribute must render as a comma-delimited list");
        }
    }

    return failures == 0 ? 0 : 1;
}
