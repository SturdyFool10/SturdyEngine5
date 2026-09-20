/// Coverage for the compile-time-first reflection layer (`StaticReflection.hpp`): `reflect<T>()`,
/// `StaticTypeInfo<T>::has_field`/`field_count`/`field<Name>()`, qualifier-preserving `TypeRef`,
/// the expanded `MemberFunctionTraits` cv/ref/noexcept matrix, and the zero-indirection
/// `get`/`set`/`invoke`/`construct` free functions. None of this touches `TypeRegistry` — every
/// check below either is a `static_assert` (proving it is genuinely `consteval`) or operates on a
/// plain stack object with no reflection registration performed anywhere in this file.

#include <Reflection/Reflection.hpp>

#include <optional>
#include <variant>
#include <unordered_map>
#include <vector>

namespace {

    struct Item;

    struct Player {
        int health = 100;
        float speed = 5.0F;
        bool alive = true;

        [[nodiscard]] constexpr int add_health(int amount) noexcept {
            health += amount;
            return health;
        }

        void freeze() const noexcept {}
        void thaw() volatile noexcept {}
        [[nodiscard]] int const_ref_method() const & noexcept { return health; }
        [[nodiscard]] int aim(const Item &target, float lead) const noexcept;
        void reload(std::vector<int> rounds) { (void)rounds; }
        static int capacity() noexcept { return 99; }
        [[nodiscard]] int rvalue_method() && noexcept { return health; }

        Player() = default;
        Player(int starting_health, float starting_speed) : health(starting_health), speed(starting_speed) {}
    };

} // namespace

SFT_REFLECT_TYPE(Player, "test.reflection.static.player");
SFT_REFLECT_FIELD(health, SFT_ATTR_INT("min", 0), SFT_ATTR_INT("max", 999), SFT_ATTR_BOOL("modder_visible", true));
SFT_REFLECT_FIELD(speed, SFT_ATTR_FLOAT("step", 0.25), SFT_ATTR_STRING("units", "m/s"));
SFT_REFLECT_FIELD(alive);
SFT_REFLECT_METHOD(add_health);
SFT_REFLECT_METHOD(aim, SFT_ATTR_STRING("category", "combat"));
SFT_REFLECT_METHOD(reload);
SFT_REFLECT_STATIC_METHOD(capacity);
SFT_REFLECT_CONSTRUCTOR(int, float);
SFT_REFLECT_EVENT("on_damaged", int);
SFT_REFLECT_END();

namespace {

    // ── Container/string fields (structural type identity) ────────────────────────────────────

    struct Item {
        int id = 0;
    };

    struct Bag {
        UString label;
        std::vector<Item> items;
        std::optional<int> slot;
        std::unordered_map<int, Item> by_id;
    };

} // namespace

SFT_REFLECT_TYPE(Item, "test.reflection.static.item");
SFT_REFLECT_FIELD(id);
SFT_REFLECT_END();

namespace {

    // Declared in-class above (where `Item` was still incomplete), defined here now that it is not.
    int Player::aim(const Item &target, float lead) const noexcept {
        return health + target.id + static_cast<int>(lead);
    }

} // namespace

SFT_REFLECT_TYPE(Bag, "test.reflection.static.bag");
SFT_REFLECT_FIELD(label);
SFT_REFLECT_FIELD(items);
SFT_REFLECT_FIELD(slot);
SFT_REFLECT_FIELD(by_id);
SFT_REFLECT_END();

namespace {

    // ── Compile-time base-chain exposure (StaticTypeInfo<T>::base_types()) ────────────────────

    struct Entity {
        int id = 0;
    };
    struct Damageable {
        int armor = 0;
    };
    struct Targetable {
        bool visible = true;
    };
    struct Turret : Entity, Damageable, Targetable {
        int ammo = 0;
    };

} // namespace

SFT_REFLECT_TYPE(Entity, "test.reflection.static.entity");
SFT_REFLECT_FIELD(id);
SFT_REFLECT_END();

SFT_REFLECT_TYPE(Damageable, "test.reflection.static.damageable");
SFT_REFLECT_FIELD(armor);
SFT_REFLECT_END();

SFT_REFLECT_TYPE(Targetable, "test.reflection.static.targetable");
SFT_REFLECT_FIELD(visible);
SFT_REFLECT_END();

SFT_REFLECT_TYPE_WITH_BASES(Turret, "test.reflection.static.turret", Entity, Damageable, Targetable);
SFT_REFLECT_FIELD(ammo);
SFT_REFLECT_END();

using namespace SFT::Reflection;

// ── StaticTypeInfo: every one of these is evaluated entirely at compile time ───────────────────
static_assert(reflect<Player>().name() == "test.reflection.static.player");
static_assert(reflect<Player>().field_count() == 3);
static_assert(reflect<Player>().has_field("health"));
static_assert(reflect<Player>().has_field("speed"));
static_assert(reflect<Player>().has_field("alive"));
static_assert(!reflect<Player>().has_field("mana"));
static_assert(reflect<Player>().field<"health">().type() == type_id<int>());
static_assert(reflect<Player>().field<"speed">().type() == type_id<float>());
static_assert(reflect<Player>().field<"health">().name == "health");
static_assert(reflect<Player>().field<"health">().size == sizeof(int));
static_assert(reflect<Player>().type_id() == type_id<Player>());

// The static and runtime layers must agree on canonical identity — `StaticTypeInfo<T>::type_id()`
// and `TypeRegistry::type<T>().key` are derived from the exact same `TypeTraits<T>::name`.
static_assert(StaticTypeInfo<Player>::type_id() == TypeId::from_name("test.reflection.static.player"));

// ── TypeRef: qualifiers must survive `remove_cvref`-free identity ──────────────────────────────
static_assert(type_ref<const Player &>().base == type_id<Player>());
static_assert(has_flag(type_ref<const Player &>().qualifiers, TypeQualifiers::Const));
static_assert(has_flag(type_ref<const Player &>().qualifiers, TypeQualifiers::LValueRef));
static_assert(!has_flag(type_ref<const Player &>().qualifiers, TypeQualifiers::RValueRef));

static_assert(type_ref<Player &&>().base == type_id<Player>());
static_assert(has_flag(type_ref<Player &&>().qualifiers, TypeQualifiers::RValueRef));
static_assert(!has_flag(type_ref<Player &&>().qualifiers, TypeQualifiers::Const));

static_assert(type_ref<const Player *>().base == type_id<Player>());
static_assert(has_flag(type_ref<const Player *>().qualifiers, TypeQualifiers::Pointer));
static_assert(has_flag(type_ref<const Player *>().qualifiers, TypeQualifiers::Const));

static_assert(type_ref<int>().qualifiers == TypeQualifiers::None);

// ── MemberFunctionTraits: the expanded cv/ref/noexcept matrix must actually parse ──────────────
static_assert(SFT::Reflection::Detail::MemberFunctionTraits<decltype(&Player::freeze)>::arity == 0);
static_assert(SFT::Reflection::Detail::MemberFunctionTraits<decltype(&Player::thaw)>::arity == 0);
static_assert(SFT::Reflection::Detail::MemberFunctionTraits<decltype(&Player::const_ref_method)>::arity == 0);
static_assert(SFT::Reflection::Detail::MemberFunctionTraits<decltype(&Player::rvalue_method)>::arity == 0);
static_assert(std::is_same_v<SFT::Reflection::Detail::MemberFunctionTraits<decltype(&Player::add_health)>::Return, int>);

// Parameter TypeRefs preserve qualifiers unlike `collect_param_types`'s `remove_cvref_t`-erased
// `TypeId`s (an `int` argument is unqualified either way, but this proves the mechanism compiles
// and produces the right base identity end to end).
static_assert(SFT::Reflection::Detail::MemberFunctionTraits<decltype(&Player::add_health)>::param_type_refs()[0].base == type_id<int>());

// ── Tier 0 direct access: these must compile to exactly `object.field`/`object.method(...)` ────
constexpr bool tier0_get_set_ok = [] {
    Player player{};
    if (get<Player, &Player::health>(player) != 100) {
        return false;
    }
    set<Player, &Player::health>(player, 42);
    if (get<Player, &Player::health>(player) != 42) {
        return false;
    }
    if (invoke<Player, &Player::add_health>(player, 8) != 50) {
        return false;
    }
    if (get<Player, &Player::health>(player) != 50) {
        return false;
    }
    Player built = construct<Player>();
    if (built.health != 100) {
        return false;
    }
    return true;
}();
static_assert(tier0_get_set_ok);

// ── Structural type identity: fields() must reach container/string-typed fields ────────────────
// Before `StructuralTypeId.hpp`, every one of these was a hard compile error: `type_ref<T>()`
// only accepted types with a canonical name of their own, which no container/wrapper has, so
// `fields()` (and everything built on it, including `schema_hash_of`) was usable only on structs
// whose every field was a scalar or another reflected struct.
static_assert(reflect<Bag>().field_count() == 4);
static_assert(reflect<Bag>().field<"label">().type() == type_id<UString>());
static_assert(reflect<Bag>().field<"items">().type() == structural_type_id<std::vector<Item>>());
static_assert(reflect<Bag>().field<"slot">().type() == structural_type_id<std::optional<int>>());
static_assert(reflect<Bag>().field<"by_id">().type() == structural_type_id<std::unordered_map<int, Item>>());

// Identity is composed from the element type, so two containers of different things differ.
static_assert(structural_type_id<std::vector<Item>>() != structural_type_id<std::vector<int>>());
static_assert(structural_type_id<std::vector<Item>>() == structural_type_id<std::vector<Item>>());
// Strictly an extension of the existing scheme: a type that already had an identity keeps it.
static_assert(structural_type_id<Item>() == type_id<Item>());
static_assert(structural_type_id<int>() == type_id<int>());
// Composition nests arbitrarily, and still refuses types with no stable identity underneath.
static_assert(StructurallyIdentifiable<std::vector<std::optional<std::vector<Item>>>>);
static_assert(!StructurallyIdentifiable<std::vector<Player *(*)(int)>>);

// ── StaticMethodInfo / StaticConstructorInfo ──────────────────────────────────────────────────
// The compile-time layer records the *full* signature, including everything the runtime
// `MethodInfo` erases: parameter cv/ref qualifiers (`MethodInfo::param_types` runs them through
// `remove_cvref_t`) and the implicit object parameter's own const/volatile/ref qualification.
static_assert(reflect<Player>().has_method("aim"));
static_assert(!reflect<Player>().has_method("no_such_method"));
static_assert(reflect<Player>().method<"aim">().is_const);
static_assert(reflect<Player>().method<"aim">().is_noexcept);
static_assert(!reflect<Player>().method<"reload">().is_const);
static_assert(reflect<Player>().method<"aim">().arity == 2);
static_assert(reflect<Player>().method<"aim">().returns() == type_id<int>());
static_assert(reflect<Player>().method<"aim">().parameter_types()[0].base == type_id<Item>());
static_assert(has_flag(reflect<Player>().method<"aim">().parameter_types()[0].qualifiers, TypeQualifiers::Const));
static_assert(has_flag(reflect<Player>().method<"aim">().parameter_types()[0].qualifiers, TypeQualifiers::LValueRef));
// A container-typed parameter resolves through composed identity, like container-typed fields do.
static_assert(reflect<Player>().method<"reload">().parameter_types()[0].base == structural_type_id<std::vector<int>>());
// Instance and static methods share one table, distinguished by a flag (as TypeInfo does).
static_assert(reflect<Player>().method<"capacity">().is_static);
static_assert(!reflect<Player>().method<"aim">().is_static);
// Constructors are identified by signature, having no name.
static_assert(reflect<Player>().constructor_count() == 1);
static_assert(reflect<Player>().constructors()[0].arity == 2);
static_assert(reflect<Player>().constructors()[0].parameter_types()[0].base == type_id<int>());
static_assert(reflect<Player>().constructors()[0].parameter_types()[1].base == type_id<float>());

// ── StaticAttribute: attributes readable at compile time ──────────────────────────────────────
// Attributes previously existed only as runtime `Attribute`s (owning a `UString`, unusable in a
// constant expression). `SFT_ATTR_*` now expands to a factory carrying its name and value as
// template arguments, so the very same declaration feeds both layers.
static_assert(reflect<Player>().field<"health">().attributes().size() == 3);
static_assert(reflect<Player>().field<"health">().has_attribute("min"));
static_assert(!reflect<Player>().field<"health">().has_attribute("not_declared"));
static_assert(reflect<Player>().field<"health">().find_attribute("max")->signed_value == 999);
static_assert(reflect<Player>().field<"health">().find_attribute("max")->kind == StaticAttributeKind::SignedInt);
static_assert(reflect<Player>().field<"health">().find_attribute("modder_visible")->bool_value);
static_assert(reflect<Player>().field<"speed">().find_attribute("units")->string_value == "m/s");
static_assert(reflect<Player>().field<"speed">().find_attribute("step")->float_value == 0.25);
static_assert(reflect<Player>().field<"speed">().find_attribute("absent") == nullptr);
static_assert(reflect<Player>().field<"alive">().attributes().empty());
static_assert(reflect<Player>().method<"aim">().find_attribute("category")->string_value == "combat");

// ── StaticEventInfo: the last member kind that previously had no compile-time counterpart ──────
static_assert(reflect<Player>().event_count() == 1);
static_assert(reflect<Player>().has_event("on_damaged"));
static_assert(!reflect<Player>().has_event("no_such_event"));
static_assert(reflect<Player>().events()[0].name == "on_damaged");
static_assert(reflect<Player>().events()[0].arity == 1);
static_assert(reflect<Player>().events()[0].parameter_types()[0].base == type_id<int>());

// ── Compile-time base-chain exposure ────────────────────────────────────────────────────────────
// A plain SFT_REFLECT_TYPE type has no reflected base at all.
static_assert(reflect<Player>().base_count() == 0);
static_assert(reflect<Player>().base_types().empty());
static_assert(!reflect<Player>().has_base(type_id<Player>()));

// Multiple inheritance (SFT_REFLECT_TYPE_WITH_BASES): primary base first, then every secondary
// base in declaration order -- the same set TypeInfo::base_type/secondary_bases expose at
// runtime, now askable at compile time too.
static_assert(reflect<Turret>().base_count() == 3);
static_assert(reflect<Turret>().base_types()[0] == type_id<Entity>());
static_assert(reflect<Turret>().base_types()[1] == type_id<Damageable>());
static_assert(reflect<Turret>().base_types()[2] == type_id<Targetable>());
static_assert(reflect<Turret>().has_base(type_id<Entity>()));
static_assert(reflect<Turret>().has_base(type_id<Damageable>()));
static_assert(reflect<Turret>().has_base(type_id<Targetable>()));
static_assert(!reflect<Turret>().has_base(type_id<Player>()));

int main() {
    // Runtime smoke test mirroring the static_assert above, so a debug build without full
    // constant-evaluation of every branch still exercises the same code paths at runtime.
    Player player{};
    if (get<Player, &Player::health>(player) != 100) {
        return 1;
    }
    set<Player, &Player::health>(player, 7);
    if (player.health != 7) {
        return 1;
    }
    if (invoke<Player, &Player::add_health>(player, 3) != 10) {
        return 1;
    }
    if (!reflect<Player>().has_field("health")) {
        return 1;
    }
    if (reflect<Player>().field_count() != 3) {
        return 1;
    }

    // The static and runtime attribute views must describe the same declarations — one macro now
    // feeds both, so a divergence here would mean the two readings of it disagree.
    const TypeInfo &runtime_type = TypeRegistry::instance().type<Player>();
    const FieldInfo *runtime_health = runtime_type.find_field("health");
    if (runtime_health == nullptr || runtime_health->attributes.size() != reflect<Player>().field<"health">().attributes().size()) {
        return 1;
    }
    const Attribute *runtime_max = find_attribute(*runtime_health, "max");
    if (runtime_max == nullptr || !std::holds_alternative<i64>(runtime_max->value) || std::get<i64>(runtime_max->value) != 999) {
        return 1;
    }
    return 0;
}
