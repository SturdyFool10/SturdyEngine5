/// End-to-end coverage of the Reflection package's macro-based front end: registration/lookup by
/// name and `TypeId`, trivial (offset+memcpy) and non-trivial (copy-fn) field access, method
/// invocation with and without a mod-installed override, the `SFT_REFLECT_INVOKE` call-site
/// macro in both its unmodded and overridden paths, base-chain (inherited) field/method lookup,
/// additive before/after method hooks, the event/listener mechanism and its
/// `SFT_REFLECT_FIRE_EVENT` call-site macro, and the default-construct/destroy instance helpers.

#include <array>
#include <cstddef>
#include <cstdio>
#include <new>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <Reflection/Reflection.hpp>

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
            (void)std::fprintf(stderr, "TypeRegistryTest: %s\n", description);
            ++failures;
        }
    }

    /// Deliberately data-less: standard-layout single inheritance (v1's supported case) requires
    /// at most one class in the hierarchy to carry non-static data members.
    struct Entity {
        int describe() noexcept {
            return 1;
        }
    };

    struct PlayerController : Entity {
        int health = 100;
        float speed = 5.0f;
        std::vector<int> inventory;

        int take_damage(int amount) noexcept {
            health -= amount;
            return health;
        }

        void heal() noexcept {
            health += 10;
        }
    };

} // namespace

SFT_REFLECT_TYPE(Entity, "test.reflection.entity");
SFT_REFLECT_METHOD(describe);
SFT_REFLECT_END();

SFT_REFLECT_TYPE_WITH_BASE(PlayerController, "test.reflection.player_controller", Entity);
SFT_REFLECT_FIELD(health, SFT_ATTR_INT("min", 0), SFT_ATTR_INT("max", 999));
SFT_REFLECT_FIELD(speed);
SFT_REFLECT_FIELD(inventory);
SFT_REFLECT_METHOD(take_damage, SFT_ATTR_STRING("category", "combat"));
SFT_REFLECT_METHOD(heal);
SFT_REFLECT_EVENT("on_damaged", int);
SFT_REFLECT_END();

namespace {

    /// Used to verify that a reflected type's own `TypeId` (its `TypeInfo::key`) is exactly what
    /// `type_id_for<Position>()` produces — the fix that makes nested-reflected-type field/return
    /// types resolvable back through `TypeRegistry::find`, instead of colliding with a second,
    /// unrelated `typeid(Position).name()`-derived identity.
    struct Position {
        float x = 0.0f;
        float y = 0.0f;
    };

} // namespace

SFT_REFLECT_TYPE(Position, "test.reflection.position");
SFT_REFLECT_FIELD(x);
SFT_REFLECT_FIELD(y);
SFT_REFLECT_END();

namespace {

    void reflected_override(void *object, const void *const *args, void *out_return, void *user_data) noexcept {
        auto *calls = static_cast<int *>(user_data);
        ++(*calls);
        auto *typed = static_cast<PlayerController *>(object);
        const int amount = *static_cast<const int *>(args[0]);
        typed->health -= amount * 2; // "double damage" mod
        if (out_return != nullptr) {
            *static_cast<int *>(out_return) = typed->health;
        }
    }

} // namespace

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    using namespace SFT::Reflection;

    TypeRegistry &registry = TypeRegistry::instance();
    const TypeInfo &type = registry.type<PlayerController>();

    check(type.canonical_name.cpp_string_view() == "test.reflection.player_controller",
          "canonical name must round-trip");
    check(type.size == sizeof(PlayerController), "size must match sizeof(T)");
    check(type.fields.size() == 3, "three fields must be reflected");
    check(type.methods.size() == 2, "two methods must be reflected");

    check(registry.find(type.key) == &type, "find(TypeId) must return the same descriptor");
    check(registry.find(type.canonical_name) == &type, "find(name) must return the same descriptor");
    check(registry.find(TypeId::from_name("test.reflection.does_not_exist")) == nullptr,
          "find must miss unknown types");

    // ── Trivial field access ──────────────────────────────────────────────────────────────────
    const FieldInfo *health_field = type.find_field("health");
    check(health_field != nullptr, "health field must be found by name");
    check(health_field != nullptr && has_flag(health_field->flags, FieldFlags::Trivial),
          "int field must be Trivial");

    PlayerController player{};
    player.health = 42;

    int read_health = 0;
    check(health_field != nullptr && copy_field_out(*health_field, &player, &read_health, sizeof(read_health)),
          "reading trivial field must succeed");
    check(read_health == 42, "reading trivial field must reflect the live value");

    const int new_health = 77;
    check(health_field != nullptr && copy_field_in(*health_field, &player, &new_health, sizeof(new_health)),
          "writing trivial field must succeed");
    check(player.health == 77, "writing trivial field must land in the object");

    // ── Non-trivial field access ──────────────────────────────────────────────────────────────
    const FieldInfo *speed_field = type.find_field("speed");
    check(speed_field != nullptr, "speed field must be found by name");

    const FieldInfo *inventory_field = type.find_field("inventory");
    check(inventory_field != nullptr, "inventory field must be found by name");
    check(inventory_field != nullptr && !has_flag(inventory_field->flags, FieldFlags::Trivial),
          "vector field must not be Trivial");

    player.inventory = {1, 2, 3};
    alignas(std::vector<int>) unsigned char inventory_storage[sizeof(std::vector<int>)];
    check(inventory_field != nullptr &&
              copy_field_out(*inventory_field, &player, inventory_storage, sizeof(std::vector<int>)),
          "reading non-trivial field must succeed");
    auto *read_inventory = std::launder(reinterpret_cast<std::vector<int> *>(inventory_storage));
    check(*read_inventory == std::vector<int>{1, 2, 3}, "reading non-trivial field must copy the live value");
    read_inventory->~vector();

    // ── Method invocation, no override ────────────────────────────────────────────────────────
    const MethodInfo *take_damage_method = type.find_method("take_damage");
    check(take_damage_method != nullptr, "take_damage method must be found by name");

    player.health = 100;
    int amount = 30;
    const void *args[]{&amount};
    int result = 0;
    check(take_damage_method != nullptr && invoke_method(*take_damage_method, &player, args, 1, &result),
          "invoking take_damage must succeed");
    check(player.health == 70, "invoking take_damage must mutate the object");
    check(result == 70, "invoking take_damage must return the new health");

    // ── Method override ───────────────────────────────────────────────────────────────────────
    int override_calls = 0;
    check(take_damage_method != nullptr &&
              registry.set_method_override(type.key, take_damage_method->key, &reflected_override, &override_calls),
          "installing an override must succeed");

    player.health = 100;
    result = 0;
    check(take_damage_method != nullptr && invoke_method(*take_damage_method, &player, args, 1, &result),
          "invoking with an override installed must succeed");
    check(player.health == 40, "an installed override must run instead of the real implementation");
    check(override_calls == 1, "the override must have been called exactly once");

    check(take_damage_method != nullptr && registry.clear_method_override(type.key, take_damage_method->key),
          "clearing the override must succeed");
    player.health = 100;
    check(take_damage_method != nullptr && invoke_method(*take_damage_method, &player, args, 1, &result),
          "invoking after clearing the override must succeed");
    check(player.health == 70, "clearing the override must restore the real implementation");

    // ── SFT_REFLECT_INVOKE call-site macro ────────────────────────────────────────────────────
    player.health = 100;
    const int direct_result = SFT_REFLECT_INVOKE(&player, PlayerController, take_damage, 30);
    check(direct_result == 70, "SFT_REFLECT_INVOKE must call through to the real method when unmodded");
    check(player.health == 70, "SFT_REFLECT_INVOKE must mutate the object when unmodded");

    check(take_damage_method != nullptr &&
              registry.set_method_override(type.key, take_damage_method->key, &reflected_override, &override_calls),
          "reinstalling an override must succeed");
    player.health = 100;
    const int overridden_result = SFT_REFLECT_INVOKE(&player, PlayerController, take_damage, 30);
    check(overridden_result == 40, "SFT_REFLECT_INVOKE must call the override when one is installed");
    check(player.health == 40, "SFT_REFLECT_INVOKE's override path must mutate the object");
    check(take_damage_method != nullptr && registry.clear_method_override(type.key, take_damage_method->key),
          "clearing the override after SFT_REFLECT_INVOKE coverage must succeed");

    // ── Base-chain (inherited) field/method lookup ────────────────────────────────────────────
    check(type.base_type == TypeId::from_name("test.reflection.entity"), "base_type must record the reflected base");
    check(type.find_method("describe") == nullptr, "describe is declared on Entity, not on PlayerController itself");

    const MethodInfo *inherited_describe = registry.find_method(type, "describe");
    check(inherited_describe != nullptr, "find_method(type, name) must walk the base chain to find describe");

    player.health = 100;
    int describe_result = 0;
    check(inherited_describe != nullptr && invoke_method(*inherited_describe, &player, nullptr, 0, &describe_result),
          "invoking an inherited method must succeed");
    check(describe_result == 1, "invoking an inherited method must run Entity's real implementation");

    // ── Additive before/after method hooks ────────────────────────────────────────────────────
    int before_hook_calls = 0;
    int after_hook_calls = 0;
    static int *before_hook_target = nullptr;
    static int *after_hook_target = nullptr;
    before_hook_target = &before_hook_calls;
    after_hook_target = &after_hook_calls;

    const auto before_hook = [](void *, const void *const *, void *) noexcept {
        ++(*before_hook_target);
    };
    const auto after_hook = [](void *, const void *const *, void *) noexcept {
        ++(*after_hook_target);
    };

    const auto before_subscription = take_damage_method != nullptr
                                          ? registry.add_method_before_hook(type.key, take_damage_method->key, before_hook, nullptr)
                                          : std::nullopt;
    const auto after_subscription = take_damage_method != nullptr
                                         ? registry.add_method_after_hook(type.key, take_damage_method->key, after_hook, nullptr)
                                         : std::nullopt;
    check(before_subscription.has_value(), "installing a before-hook must succeed");
    check(after_subscription.has_value(), "installing an after-hook must succeed");

    player.health = 100;
    check(take_damage_method != nullptr && invoke_method(*take_damage_method, &player, args, 1, &result),
          "invoking with hooks installed must succeed");
    check(player.health == 70, "before/after hooks must not change the real implementation's effect");
    check(before_hook_calls == 1, "the before-hook must have run exactly once");
    check(after_hook_calls == 1, "the after-hook must have run exactly once");

    check(before_subscription.has_value() &&
              registry.remove_method_before_hook(type.key, take_damage_method->key, *before_subscription),
          "removing the before-hook must succeed");
    check(after_subscription.has_value() &&
              registry.remove_method_after_hook(type.key, take_damage_method->key, *after_subscription),
          "removing the after-hook must succeed");

    player.health = 100;
    check(take_damage_method != nullptr && invoke_method(*take_damage_method, &player, args, 1, &result),
          "invoking after removing hooks must succeed");
    check(before_hook_calls == 1, "a removed before-hook must not fire again");
    check(after_hook_calls == 1, "a removed after-hook must not fire again");

    // ── Events ─────────────────────────────────────────────────────────────────────────────────
    const EventInfo *on_damaged_event = type.find_event("on_damaged");
    check(on_damaged_event != nullptr, "on_damaged event must be found by name");
    check(on_damaged_event != nullptr && on_damaged_event->listeners.size() == 0,
          "a freshly registered event must start with no listeners");

    static int event_calls = 0;
    static int event_last_amount = 0;
    event_calls = 0;
    const auto event_listener = [](void *, const void *const *event_args, void *) noexcept {
        ++event_calls;
        event_last_amount = *static_cast<const int *>(event_args[0]);
    };

    // Firing before anyone has subscribed must be a safe no-op.
    SFT_REFLECT_FIRE_EVENT(&player, PlayerController, "on_damaged", amount);
    check(event_calls == 0, "firing an event with no subscribers must not call anything");

    const auto event_subscription = on_damaged_event != nullptr
                                         ? registry.subscribe_event(type.key, on_damaged_event->key, event_listener, nullptr)
                                         : std::nullopt;
    check(event_subscription.has_value(), "subscribing to an event must succeed");

    int fired_amount = 55;
    SFT_REFLECT_FIRE_EVENT(&player, PlayerController, "on_damaged", fired_amount);
    check(event_calls == 1, "firing a subscribed event must call the listener exactly once");
    check(event_last_amount == 55, "the listener must receive the fired argument");

    check(event_subscription.has_value() &&
              registry.unsubscribe_event(type.key, on_damaged_event->key, *event_subscription),
          "unsubscribing from an event must succeed");
    SFT_REFLECT_FIRE_EVENT(&player, PlayerController, "on_damaged", fired_amount);
    check(event_calls == 1, "firing after unsubscribing must not call the listener again");

    // ── Instance construction helpers ─────────────────────────────────────────────────────────
    alignas(PlayerController) unsigned char instance_storage[sizeof(PlayerController)];
    check(default_construct_instance(type, instance_storage), "default-constructing an instance must succeed");
    auto *constructed = std::launder(reinterpret_cast<PlayerController *>(instance_storage));
    check(constructed->health == 100, "a default-constructed instance must have default field values");
    destroy_instance(type, instance_storage);

    // ── TypeInfoBuilder: a mod registering a brand-new type entirely at runtime ──────────────────
    // No SFT_REFLECT_TYPE, no C++ type known to this translation unit ahead of time in spirit —
    // `DynamicItem` stands in for a type a scripted mod would describe from its own runtime data.
    struct DynamicItem {
        int durability = 0;
    };

    TypeInfo dynamic_item_info =
        TypeInfoBuilder("test.reflection.mod.dynamic_item", sizeof(DynamicItem), alignof(DynamicItem))
            .constructors(
                [](void *destination, void *source, void *) noexcept {
                    ::new (destination) DynamicItem(std::move(*static_cast<DynamicItem *>(source)));
                },
                [](void *object, void *) noexcept { static_cast<DynamicItem *>(object)->~DynamicItem(); },
                [](void *destination, void *) noexcept { ::new (destination) DynamicItem(); })
            .field("durability", offsetof(DynamicItem, durability), sizeof(int), alignof(int),
                   type_id_for<int>(), FieldFlags::Trivial)
            .build();

    const auto dynamic_registered = registry.register_type(std::move(dynamic_item_info));
    check(dynamic_registered.has_value(), "registering a dynamically-built type must succeed");

    const TypeInfo *dynamic_type = dynamic_registered.has_value() ? registry.find(*dynamic_registered) : nullptr;
    check(dynamic_type != nullptr, "a dynamically-built type must be findable by TypeId");
    check(dynamic_type != nullptr && registry.find(dynamic_type->canonical_name) == dynamic_type,
          "a dynamically-built type must be findable by name too");

    const FieldInfo *durability_field = dynamic_type != nullptr ? dynamic_type->find_field("durability") : nullptr;
    check(durability_field != nullptr, "a dynamically-built field must be found by name");

    DynamicItem item{};
    item.durability = 5;
    int read_durability = 0;
    check(durability_field != nullptr &&
              copy_field_out(*durability_field, &item, &read_durability, sizeof(read_durability)),
          "reading a dynamically-built field must succeed");
    check(read_durability == 5, "reading a dynamically-built field must reflect the live value");

    // A "mod discovering a mod": found purely by enumeration, the way a second mod with no
    // compile-time knowledge of DynamicItem would have to find it.
    bool found_via_enumeration = false;
    registry.for_each_type([&found_via_enumeration](const TypeInfo &info) {
        if (info.canonical_name.cpp_string_view() == "test.reflection.mod.dynamic_item") {
            found_via_enumeration = true;
        }
    });
    check(found_via_enumeration,
          "for_each_type must enumerate dynamically-built types alongside macro-reflected ones");

    // ── Type-checked field/method access ──────────────────────────────────────────────────────
    check(health_field != nullptr &&
              copy_field_out_checked(*health_field, type_id_for<int>(), &player, &read_health, sizeof(read_health)),
          "checked field read must succeed when the expected type matches");
    check(health_field != nullptr &&
              !copy_field_out_checked(*health_field, type_id_for<float>(), &player, &read_health, sizeof(read_health)),
          "checked field read must reject a mismatched expected type even when the size matches");
    check(health_field != nullptr &&
              !copy_field_in_checked(*health_field, type_id_for<float>(), &player, &new_health, sizeof(new_health)),
          "checked field write must reject a mismatched expected type");

    const std::array<TypeId, 1> take_damage_params{type_id_for<int>()};
    const std::array<TypeId, 1> wrong_params{type_id_for<float>()};
    player.health = 100;
    check(take_damage_method != nullptr &&
              invoke_method_checked(*take_damage_method, type_id_for<int>(), take_damage_params, &player, args, 1, &result),
          "checked invoke must succeed when the expected signature matches");
    check(player.health == 70, "a successful checked invoke must still run the real call");
    check(take_damage_method != nullptr &&
              !invoke_method_checked(*take_damage_method, type_id_for<int>(), wrong_params, &player, args, 1, &result),
          "checked invoke must reject a mismatched parameter type");
    check(player.health == 70, "a rejected checked invoke must not run anything");

    // ── unregister_type / mod unload ──────────────────────────────────────────────────────────
    check(dynamic_type != nullptr && registry.unregister_type(dynamic_type->key),
          "unregistering a type must succeed");
    check(registry.find(TypeId::from_name("test.reflection.mod.dynamic_item")) == nullptr,
          "an unregistered type must no longer be findable by TypeId");
    const UString dynamic_item_name{"test.reflection.mod.dynamic_item"};
    check(registry.find(dynamic_item_name) == nullptr,
          "an unregistered type must no longer be findable by name");
    check(dynamic_type != nullptr && !registry.unregister_type(dynamic_type->key),
          "unregistering an already-unregistered type must fail cleanly");

    bool still_enumerated = false;
    registry.for_each_type([&still_enumerated](const TypeInfo &info) {
        if (info.canonical_name.cpp_string_view() == "test.reflection.mod.dynamic_item") {
            still_enumerated = true;
        }
    });
    check(!still_enumerated, "for_each_type must skip unregistered types");

    // ── Attributes ─────────────────────────────────────────────────────────────────────────────
    check(health_field != nullptr && health_field->attributes.size() == 2,
          "health must carry the two attributes declared on it");
    const Attribute *min_attr = health_field != nullptr ? find_attribute(*health_field, "min") : nullptr;
    check(min_attr != nullptr, "the min attribute must be found by name");
    check(min_attr != nullptr && std::holds_alternative<i64>(min_attr->value) && std::get<i64>(min_attr->value) == 0,
          "the min attribute must carry the declared integer value");
    check(find_attribute(*health_field, "does_not_exist") == nullptr,
          "find_attribute must miss an attribute that was never declared");

    const Attribute *category_attr = take_damage_method != nullptr ? find_attribute(*take_damage_method, "category") : nullptr;
    check(category_attr != nullptr, "the category attribute must be found by name on take_damage");
    check(category_attr != nullptr && std::holds_alternative<UString>(category_attr->value) &&
              std::get<UString>(category_attr->value).cpp_string_view() == "combat",
          "the category attribute must carry the declared string value");
    check(speed_field != nullptr && speed_field->attributes.empty(),
          "a field with no declared attributes must have an empty attributes vector");

    // ── Reflected-nested-type TypeId identity ─────────────────────────────────────────────────
    const TypeInfo &position_type = registry.type<Position>();
    check(position_type.key == type_id_for<Position>(),
          "a reflected type's own TypeInfo::key must equal type_id_for<T>() for that same type, "
          "so a field/return TypeId of that type resolves back through TypeRegistry::find");

    if (failures != 0) {
        (void)std::fprintf(stderr, "TypeRegistryTest: %d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
