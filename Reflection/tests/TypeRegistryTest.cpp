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

    /// Deliberately data-less: single-inheritance test fixture.
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

    // ── Multiple inheritance ───────────────────────────────────────────────────────────────────
    // Two unrelated "interface" bases, and a type implementing both alongside its own primary
    // base — the common case this exists for (a type implementing several unrelated interfaces),
    // not diamond inheritance.

    struct Damageable {
        int hit_points = 50;

        int take_hit(int amount) noexcept {
            hit_points -= amount;
            return hit_points;
        }
    };

    struct Targetable {
        float priority = 1.0F;

        [[nodiscard]] float targeting_priority() const noexcept {
            return priority;
        }
    };

    /// `Entity` (declared above) is the primary base — its subobject sits at offset 0, exactly
    /// like single inheritance always worked. `Damageable`/`Targetable` are secondary bases: real
    /// additional subobjects at nonzero offsets (guaranteed nonzero here since `Entity` itself has
    /// no data members but `Turret` does, and multiple inheritance lays out base subobjects before
    /// the derived type's own members) — the exact case that needs pointer adjustment.
    struct Turret : Entity, Damageable, Targetable {
        int ammo = 10;
    };

} // namespace

SFT_REFLECT_TYPE(Damageable, "test.reflection.damageable");
SFT_REFLECT_FIELD(hit_points);
SFT_REFLECT_METHOD(take_hit);
SFT_REFLECT_END();

SFT_REFLECT_TYPE(Targetable, "test.reflection.targetable");
SFT_REFLECT_FIELD(priority);
SFT_REFLECT_METHOD(targeting_priority);
SFT_REFLECT_END();

SFT_REFLECT_TYPE_WITH_BASES(Turret, "test.reflection.turret", Entity, Damageable, Targetable);
SFT_REFLECT_FIELD(ammo);
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

    // ── Stable handles (TypeHandle/FieldHandle/MethodHandle) ──────────────────────────────────
    const TypeHandle player_handle = registry.handle_for(type.key);
    check(static_cast<bool>(player_handle), "handle_for must return a valid handle for a registered type");
    check(registry.resolve(player_handle) == &type, "resolving a fresh TypeHandle must return the same TypeInfo find() would");
    check(!static_cast<bool>(registry.handle_for(TypeId::from_name("test.reflection.does_not_exist"))),
          "handle_for must return an invalid handle for an unregistered TypeId");
    check(registry.resolve(TypeHandle{}) == nullptr, "resolving a default-constructed (invalid) TypeHandle must fail");

    const FieldHandle health_field_handle = registry.field_handle(player_handle, health_field->key);
    check(static_cast<bool>(health_field_handle), "field_handle must find a declared field by key");
    check(registry.resolve(health_field_handle) == health_field, "resolving a FieldHandle must return the same FieldInfo find_field() would");
    check(!static_cast<bool>(registry.field_handle(player_handle, TypeId::from_name("does_not_exist"))),
          "field_handle must return an invalid handle for an unknown field name");

    // ── Runtime field overlays (mod-mutable name/attributes over an unmutated FieldInfo) ──────
    check(registry.effective_field_name(health_field_handle).cpp_string_view() == "health",
          "effective_field_name must equal the static name before any override is installed");
    check(registry.set_field_name_override(health_field_handle, UString{"HP"}),
          "set_field_name_override must succeed for a resolvable handle");
    check(registry.effective_field_name(health_field_handle).cpp_string_view() == "HP",
          "effective_field_name must reflect the installed override");
    check(health_field != nullptr && health_field->name.cpp_string_view() == "health",
          "installing a name override must NOT mutate the underlying FieldInfo::name");
    check(registry.find_field(type, "health") == health_field,
          "find_field must still resolve by the field's real (un-overridden) name after an overlay is installed");

    const usize static_attribute_count = health_field != nullptr ? health_field->attributes.size() : 0;
    check(registry.add_field_attribute_override(health_field_handle, Detail::make_attribute("modder_added", true)),
          "add_field_attribute_override must succeed for a resolvable handle");
    const std::vector<Attribute> effective_attrs = registry.effective_field_attributes(health_field_handle);
    check(effective_attrs.size() == static_attribute_count + 1,
          "effective_field_attributes must be the static attributes plus the overlay attribute, additive");
    check(health_field != nullptr && health_field->attributes.size() == static_attribute_count,
          "adding an attribute overlay must NOT mutate the underlying FieldInfo::attributes");

    check(registry.clear_field_name_override(health_field_handle), "clear_field_name_override must succeed when an override was present");
    check(registry.effective_field_name(health_field_handle).cpp_string_view() == "health",
          "effective_field_name must fall back to the static name once the override is cleared");
    check(!registry.clear_field_name_override(health_field_handle),
          "clear_field_name_override must report false when no override is present");

    check(registry.clear_field_overlay(health_field_handle), "clear_field_overlay must remove the remaining attribute overlay");
    check(registry.effective_field_attributes(health_field_handle).size() == static_attribute_count,
          "effective_field_attributes must be back to just the static attributes after clear_field_overlay");

    check(!registry.set_field_name_override(FieldHandle{}, UString{"nope"}),
          "set_field_name_override must fail for an invalid FieldHandle");
    check(registry.effective_field_name(FieldHandle{}).cpp_string_view().empty(),
          "effective_field_name must return an empty UString for an invalid FieldHandle");

    const MethodHandle take_damage_handle = registry.method_handle(player_handle, take_damage_method->key);
    check(static_cast<bool>(take_damage_handle), "method_handle must find a declared method by key");
    check(registry.resolve(take_damage_handle) == take_damage_method, "resolving a MethodHandle must return the same MethodInfo find_method() would");

    // ── Runtime type-level overlays (mod-mutable name/attributes over an unmutated TypeInfo) ──
    check(registry.effective_type_name(player_handle).cpp_string_view() == type.canonical_name.cpp_string_view(),
          "effective_type_name must equal the static canonical_name before any override is installed");
    check(registry.set_type_name_override(player_handle, UString{"PlayerCharacter"}),
          "set_type_name_override must succeed for a resolvable handle");
    check(registry.effective_type_name(player_handle).cpp_string_view() == "PlayerCharacter",
          "effective_type_name must reflect the installed override");
    check(type.canonical_name.cpp_string_view() != "PlayerCharacter",
          "installing a type name override must NOT mutate the underlying TypeInfo::canonical_name");
    check(registry.find(type.key) == &type,
          "find must still resolve by the type's real (un-overridden) name after an overlay is installed");

    const usize static_type_attribute_count = type.attributes.size();
    check(registry.add_type_attribute_override(player_handle, Detail::make_attribute("modder_renamed", true)),
          "add_type_attribute_override must succeed for a resolvable handle");
    check(registry.effective_type_attributes(player_handle).size() == static_type_attribute_count + 1,
          "effective_type_attributes must be the static attributes plus the overlay attribute, additive");
    check(type.attributes.size() == static_type_attribute_count,
          "adding a type attribute overlay must NOT mutate the underlying TypeInfo::attributes");

    check(registry.clear_type_name_override(player_handle), "clear_type_name_override must succeed when an override was present");
    check(registry.effective_type_name(player_handle).cpp_string_view() == type.canonical_name.cpp_string_view(),
          "effective_type_name must fall back to the static name once the override is cleared");
    check(!registry.clear_type_name_override(player_handle),
          "clear_type_name_override must report false when no override is present");

    check(registry.clear_type_overlay(player_handle), "clear_type_overlay must remove the remaining attribute overlay");
    check(registry.effective_type_attributes(player_handle).size() == static_type_attribute_count,
          "effective_type_attributes must be back to just the static attributes after clear_type_overlay");

    check(!registry.set_type_name_override(TypeHandle{}, UString{"nope"}),
          "set_type_name_override must fail for an invalid TypeHandle");
    check(registry.effective_type_name(TypeHandle{}).cpp_string_view().empty(),
          "effective_type_name must return an empty UString for an invalid TypeHandle");

    // ── Runtime method-level overlays (mod-mutable name/attributes over an unmutated MethodInfo) ──
    check(registry.effective_method_name(take_damage_handle).cpp_string_view() == "take_damage",
          "effective_method_name must equal the static name before any override is installed");
    check(registry.set_method_name_override(take_damage_handle, UString{"apply_damage"}),
          "set_method_name_override must succeed for a resolvable handle");
    check(registry.effective_method_name(take_damage_handle).cpp_string_view() == "apply_damage",
          "effective_method_name must reflect the installed override");
    check(take_damage_method->name.cpp_string_view() == "take_damage",
          "installing a method name override must NOT mutate the underlying MethodInfo::name");
    check(registry.find_method(type, "take_damage") == take_damage_method,
          "find_method must still resolve by the method's real (un-overridden) name after an overlay is installed");

    const usize static_method_attribute_count = take_damage_method->attributes.size();
    check(registry.add_method_attribute_override(take_damage_handle, Detail::make_attribute("modder_renamed", true)),
          "add_method_attribute_override must succeed for a resolvable handle");
    check(registry.effective_method_attributes(take_damage_handle).size() == static_method_attribute_count + 1,
          "effective_method_attributes must be the static attributes plus the overlay attribute, additive");
    check(take_damage_method->attributes.size() == static_method_attribute_count,
          "adding a method attribute overlay must NOT mutate the underlying MethodInfo::attributes");

    check(registry.clear_method_name_override(take_damage_handle), "clear_method_name_override must succeed when an override was present");
    check(registry.effective_method_name(take_damage_handle).cpp_string_view() == "take_damage",
          "effective_method_name must fall back to the static name once the override is cleared");
    check(!registry.clear_method_name_override(take_damage_handle),
          "clear_method_name_override must report false when no override is present");

    check(registry.clear_method_overlay(take_damage_handle), "clear_method_overlay must remove the remaining attribute overlay");
    check(registry.effective_method_attributes(take_damage_handle).size() == static_method_attribute_count,
          "effective_method_attributes must be back to just the static attributes after clear_method_overlay");

    check(!registry.set_method_name_override(MethodHandle{}, UString{"nope"}),
          "set_method_name_override must fail for an invalid MethodHandle");
    check(registry.effective_method_name(MethodHandle{}).cpp_string_view().empty(),
          "effective_method_name must return an empty UString for an invalid MethodHandle");

    // A handle obtained *before* a type is unregistered must stop resolving afterward, even
    // though the underlying TypeInfo/FieldInfo storage is never physically freed — this is the
    // whole point of a generation-checked handle over a bare pointer (see TypeHandle's doc
    // comment). Exercised against the dynamically-built type since it's the one this test
    // deliberately unregisters below.
    check(dynamic_type != nullptr, "dynamic_type must exist before testing handle staleness across unregister");
    const TypeHandle dynamic_handle = dynamic_type != nullptr ? registry.handle_for(dynamic_type->key) : TypeHandle{};
    check(static_cast<bool>(dynamic_handle), "handle_for must resolve the dynamically-built type before it is unregistered");
    check(registry.resolve(dynamic_handle) == dynamic_type, "the dynamic type's handle must resolve while it is still registered");

    // ── unregister_type / mod unload ──────────────────────────────────────────────────────────
    check(dynamic_type != nullptr && registry.unregister_type(dynamic_type->key),
          "unregistering a type must succeed");
    check(static_cast<bool>(dynamic_handle) && registry.resolve(dynamic_handle) == nullptr,
          "a TypeHandle obtained before unregister_type must stop resolving afterward");
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

    // ── Static/runtime TypeId agreement ───────────────────────────────────────────────────────
    // The compile-time layer (`reflect<T>().field<"name">().type()`) and the runtime layer
    // (`FieldInfo::field_type`) must assign the *same* identity to the same field. They did not
    // before `Detail::erased_type_id` stopped deriving identity from `typeid(M).name()`: the
    // runtime path hashed a compiler-mangled name while the static path hashed the canonical one,
    // so the two silently disagreed for every non-reflected field type (`int`, `UString`,
    // containers) — making a static-side type check and a runtime-side one answer differently
    // about the very same field.
    check(health_field != nullptr && health_field->field_type == reflect<PlayerController>().field<"health">().type(),
          "runtime FieldInfo::field_type must equal the static layer's field type for an int field");
    check(speed_field != nullptr && speed_field->field_type == reflect<PlayerController>().field<"speed">().type(),
          "runtime and static field type identity must agree for a float field");
    check(type_id_for<int>() == type_id<int>(),
          "the runtime type_id_for<T>() and compile-time type_id<T>() must be the same identity");
    check(type_id_for<UString>() == type_id<UString>(),
          "UString must have one identity across both layers, not a mangled one and a canonical one");

    // ── Reflected-nested-type TypeId identity ─────────────────────────────────────────────────
    const TypeInfo &position_type = registry.type<Position>();
    check(position_type.key == type_id_for<Position>(),
          "a reflected type's own TypeInfo::key must equal type_id_for<T>() for that same type, "
          "so a field/return TypeId of that type resolves back through TypeRegistry::find");

    // ── Multiple inheritance ───────────────────────────────────────────────────────────────────
    const TypeInfo &turret_type = registry.type<Turret>();
    check(turret_type.base_type == type_id_for<Entity>(),
          "the primary base (first in SFT_REFLECT_TYPE_WITH_BASES) must still populate base_type exactly as single inheritance always did");
    check(turret_type.secondary_bases.size() == 2, "both secondary bases must be recorded");

    // isInstance/isAssignableFrom-style checks: a Turret is-a Entity, is-a Damageable, is-a
    // Targetable, reachable through the primary chain OR either secondary base.
    check(registry.is_assignable_from(type_id_for<Entity>(), turret_type.key),
          "is_assignable_from must still find the primary base (unchanged single-inheritance behavior)");
    check(registry.is_assignable_from(type_id_for<Damageable>(), turret_type.key),
          "is_assignable_from must find a type reachable only through a secondary base");
    check(registry.is_assignable_from(type_id_for<Targetable>(), turret_type.key),
          "is_assignable_from must find a type reachable through a DIFFERENT secondary base");
    check(!registry.is_assignable_from(turret_type.key, type_id_for<Damageable>()),
          "is_assignable_from must not report the relationship backwards");
    struct Unrelated1 {};
    check(!registry.is_assignable_from(TypeId::from_name("nonexistent"), turret_type.key),
          "is_assignable_from must fail cleanly against an unregistered base");

    Turret turret{};
    turret.ammo = 7;
    turret.hit_points = 50;
    turret.priority = 2.5F;

    // Fields reached through the primary base need no adjustment — object and adjusted-object
    // must be the exact same address, matching single inheritance's existing guarantee.
    {
        auto [field, adjusted] = registry.find_field_adjusted(turret_type, "describe_nonexistent_field", &turret);
        check(field == nullptr && adjusted == nullptr, "find_field_adjusted must fail cleanly for an unknown field");
    }

    // Fields reached through secondary bases: THIS is the actual test of pointer adjustment. If
    // the adjustment were wrong (e.g. the raw Turret* were used unmodified against Damageable's
    // own hit_points offset), this would read garbage or corrupt memory instead of the real value.
    {
        auto [field, adjusted] = registry.find_field_adjusted(turret_type, "hit_points", &turret);
        check(field != nullptr, "hit_points must be found through the Damageable secondary base");
        int read_hp = 0;
        check(field != nullptr && adjusted != nullptr && copy_field_out(*field, adjusted, &read_hp, sizeof(read_hp)),
              "reading a field through a secondary base must succeed");
        check(read_hp == 50, "reading a field through a secondary base must return the REAL value, proving the pointer adjustment was correct");

        const int new_hp = 33;
        check(field != nullptr && adjusted != nullptr && copy_field_in(*field, adjusted, &new_hp, sizeof(new_hp)),
              "writing a field through a secondary base must succeed");
        check(turret.hit_points == 33, "writing through the adjusted pointer must land in the REAL Damageable subobject inside turret, not some other address");
    }
    {
        auto [field, adjusted] = registry.find_field_adjusted(turret_type, "priority", &turret);
        check(field != nullptr, "priority must be found through the Targetable secondary base");
        float read_priority = 0.0F;
        check(field != nullptr && adjusted != nullptr && copy_field_out(*field, adjusted, &read_priority, sizeof(read_priority)),
              "reading a field through a DIFFERENT secondary base must also succeed");
        check(read_priority == 2.5F, "reading through Targetable's adjustment must return the real value, not Damageable's or Entity's");
    }
    {
        // The type's OWN field, for contrast: no base walk needed at all, object == adjusted.
        auto [field, adjusted] = registry.find_field_adjusted(turret_type, "ammo", &turret);
        check(field != nullptr && adjusted == static_cast<void *>(&turret),
              "a field declared directly on the type itself must need no adjustment");
    }

    // Methods reached through secondary bases: same pointer-adjustment hazard, for invocation
    // instead of field offsets — take_hit()'s implicit `this` must point at the real Damageable
    // subobject, or it corrupts unrelated memory instead of mutating turret.hit_points.
    {
        auto [method, adjusted] = registry.find_method_adjusted(turret_type, "take_hit", &turret);
        check(method != nullptr && adjusted != nullptr, "take_hit must be found and adjusted through Damageable");
        turret.hit_points = 100;
        const int damage = 40;
        const void *args[] = {&damage};
        int result = 0;
        check(invoke_method(*method, adjusted, args, 1, &result), "invoking a method through a secondary base must dispatch");
        check(result == 60 && turret.hit_points == 60,
              "invoking through the adjusted receiver must mutate the REAL Damageable subobject inside turret");
    }
    {
        auto [method, adjusted] = registry.find_method_adjusted(turret_type, "targeting_priority", &turret);
        check(method != nullptr && adjusted != nullptr, "targeting_priority must be found through Targetable, a different secondary base");
        float result = 0.0F;
        check(invoke_method(*method, adjusted, nullptr, 0, &result), "invoking through Targetable's adjustment must dispatch");
        check(result == 2.5F, "the result must be Targetable's real value, not corrupted by a wrong adjustment");
    }

    if (failures != 0) {
        (void)std::fprintf(stderr, "TypeRegistryTest: %d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
