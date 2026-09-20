/// Coverage for the "as capable as Java reflection" follow-up work: method overloading,
/// exception-safe invocation, inheritance queries (`is_assignable_from`), expanded container
/// coverage (`std::array`, ordered `std::map`, `std::set`, `std::unique_ptr`, `std::shared_ptr`),
/// attribute reverse-lookup, and dynamic proxies.

#include <array>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

#include <Reflection/Reflection.hpp>

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "CapabilityTest: %s\n", description);
            ++failures;
        }
    }

    // ── Overloading ────────────────────────────────────────────────────────────────────────────

    struct Blaster {
        int last_amount = 0;
        int last_kind = 0;

        void take_damage(int amount) {
            last_amount = amount;
            last_kind = 1;
        }
        void take_damage(int amount, int kind) {
            last_amount = amount;
            last_kind = kind;
        }
    };

    // ── Ref-qualified / volatile member functions ─────────────────────────────────────────────

    struct RefQualified {
        int ammo = 0;

        /// `&&`-qualified: callable only on an rvalue, so the reflection trampoline must move the
        /// receiver rather than calling through an lvalue (see `Detail::MethodRefQualifier`).
        [[nodiscard]] int fire() && noexcept { return ammo * 2; }
        [[nodiscard]] int peek() const & noexcept { return ammo; }
        void tweak(int amount) volatile { ammo = amount; }
    };

} // namespace

SFT_REFLECT_TYPE(Blaster, "test.reflection.capability.blaster");
SFT_REFLECT_FIELD(last_amount);
SFT_REFLECT_FIELD(last_kind);
SFT_REFLECT_METHOD_OVERLOAD(take_damage, void (ReflectedType::*)(int));
SFT_REFLECT_METHOD_OVERLOAD(take_damage, void (ReflectedType::*)(int, int));
SFT_REFLECT_END();

SFT_REFLECT_TYPE(RefQualified, "test.reflection.capability.ref_qualified");
SFT_REFLECT_FIELD(ammo);
SFT_REFLECT_METHOD_OVERLOAD(fire, int (ReflectedType::*)() && noexcept);
SFT_REFLECT_METHOD_OVERLOAD(peek, int (ReflectedType::*)() const & noexcept);
SFT_REFLECT_METHOD_OVERLOAD(tweak, void (ReflectedType::*)(int) volatile);
SFT_REFLECT_END();

namespace {

    // ── Exceptions ─────────────────────────────────────────────────────────────────────────────

    struct Unidentifiable {
        int value = 0;
    };

    struct Signatures {
        int hits = 0;

        [[nodiscard]] int aim(const Blaster &target, float lead) const noexcept {
            return target.last_amount + static_cast<int>(lead);
        }
        /// `Unidentifiable` is deliberately never reflected, so this signature has no canonical
        /// identity and must degrade gracefully rather than fail to register.
        void opaque(const Unidentifiable &thing) {
            hits += thing.value;
        }
    };

    struct Fragile {
        int calls = 0;

        void detonate() {
            ++calls;
            throw std::runtime_error("boom");
        }
        int add_one(int value) {
            ++calls;
            return value + 1;
        }
    };

} // namespace

SFT_REFLECT_TYPE(Signatures, "test.reflection.capability.signatures");
SFT_REFLECT_FIELD(hits);
SFT_REFLECT_METHOD(aim);
SFT_REFLECT_METHOD(opaque);
SFT_REFLECT_END();

SFT_REFLECT_TYPE(Fragile, "test.reflection.capability.fragile");
SFT_REFLECT_FIELD(calls);
SFT_REFLECT_METHOD(detonate);
SFT_REFLECT_METHOD(add_one);
SFT_REFLECT_END();

namespace {

    // ── Inheritance ────────────────────────────────────────────────────────────────────────────

    struct Base {
        int base_value = 0;
    };
    struct Derived : Base {
        int derived_value = 0;
    };
    struct Unrelated {
        int something = 0;
    };

} // namespace

SFT_REFLECT_TYPE(Base, "test.reflection.capability.base");
SFT_REFLECT_FIELD(base_value);
SFT_REFLECT_END();

SFT_REFLECT_TYPE_WITH_BASE(Derived, "test.reflection.capability.derived", Base);
SFT_REFLECT_FIELD(derived_value);
SFT_REFLECT_END();

SFT_REFLECT_TYPE(Unrelated, "test.reflection.capability.unrelated");
SFT_REFLECT_FIELD(something);
SFT_REFLECT_END();

namespace {

    // ── Containers ─────────────────────────────────────────────────────────────────────────────

    struct Inventory {
        std::array<int, 3> slots{};
        std::map<UString, int> ordered_scores;
        std::set<int> unlocked_levels;
        std::unique_ptr<int> equipped_weapon_id;
        std::shared_ptr<UString> guild_name;
    };

} // namespace

SFT_REFLECT_TYPE(Inventory, "test.reflection.capability.inventory");
SFT_REFLECT_FIELD(slots);
SFT_REFLECT_FIELD(ordered_scores);
SFT_REFLECT_FIELD(unlocked_levels);
SFT_REFLECT_FIELD(equipped_weapon_id);
SFT_REFLECT_FIELD(guild_name);
SFT_REFLECT_END();

namespace {

    // ── Attributes ─────────────────────────────────────────────────────────────────────────────

    struct Taggable {
        int x = 0;
    };
    struct NotTaggable {
        int y = 0;
    };

} // namespace

SFT_REFLECT_TYPE(Taggable, "test.reflection.capability.taggable");
SFT_REFLECT_FIELD(x, SFT_ATTR_BOOL("modder_visible", true));
SFT_REFLECT_END();

SFT_REFLECT_TYPE(NotTaggable, "test.reflection.capability.not_taggable");
SFT_REFLECT_FIELD(y);
SFT_REFLECT_END();

int main() {
    using namespace SFT::Reflection;

    TypeRegistry &registry = TypeRegistry::instance();

    // ── Overloading ────────────────────────────────────────────────────────────────────────────
    const TypeInfo &blaster_type = registry.type<Blaster>();
    check(blaster_type.methods.size() == 2, "two distinct overloads of take_damage must both register");
    check(blaster_type.methods[0].key != blaster_type.methods[1].key, "overloads must not collide on the same MethodInfo::key");

    const std::vector<TypeId> one_arg_sig{type_id_for<int>()};
    const std::vector<TypeId> two_arg_sig{type_id_for<int>(), type_id_for<int>()};
    const MethodInfo *one_arg_overload = blaster_type.find_method("take_damage", one_arg_sig);
    const MethodInfo *two_arg_overload = blaster_type.find_method("take_damage", two_arg_sig);
    check(one_arg_overload != nullptr && one_arg_overload->param_types.size() == 1, "the 1-arg overload must be found by exact signature");
    check(two_arg_overload != nullptr && two_arg_overload->param_types.size() == 2, "the 2-arg overload must be found by exact signature");
    check(one_arg_overload != two_arg_overload, "the two overloads must be distinct MethodInfo entries");

    Blaster blaster{};
    const int amount_arg = 7;
    const void *one_arg_args[] = {&amount_arg};
    check(invoke_method(*one_arg_overload, &blaster, one_arg_args, 1, nullptr), "invoking the 1-arg overload must succeed");
    check(blaster.last_amount == 7 && blaster.last_kind == 1, "the 1-arg overload must run, not the 2-arg one");

    const int kind_arg = 9;
    const void *two_arg_args[] = {&amount_arg, &kind_arg};
    check(invoke_method(*two_arg_overload, &blaster, two_arg_args, 2, nullptr), "invoking the 2-arg overload must succeed");
    check(blaster.last_amount == 7 && blaster.last_kind == 9, "the 2-arg overload must run, using both of its own arguments");

    Blaster invoked_via_macro{};
    SFT_REFLECT_INVOKE_OVERLOAD(&invoked_via_macro, Blaster, take_damage, void (Blaster::*)(int), 3);
    check(invoked_via_macro.last_amount == 3 && invoked_via_macro.last_kind == 1, "SFT_REFLECT_INVOKE_OVERLOAD must resolve the 1-arg overload");
    SFT_REFLECT_INVOKE_OVERLOAD(&invoked_via_macro, Blaster, take_damage, void (Blaster::*)(int, int), 4, 5);
    check(invoked_via_macro.last_amount == 4 && invoked_via_macro.last_kind == 5, "SFT_REFLECT_INVOKE_OVERLOAD must resolve the 2-arg overload");

    // ── Exceptions ─────────────────────────────────────────────────────────────────────────────
    const TypeInfo &fragile_type = registry.type<Fragile>();
    const MethodInfo *detonate_method = fragile_type.find_method("detonate");
    check(detonate_method != nullptr, "detonate must be found");

    Fragile fragile{};
    InvokeException thrown{};
    const bool detonate_dispatched = invoke_method(*detonate_method, &fragile, nullptr, 0, nullptr, &thrown);
    check(detonate_dispatched, "invoking a throwing method must still report a successful dispatch (arity matched)");
    check(thrown.threw, "InvokeException::threw must be set after a real exception");
    check(thrown.message.cpp_string_view().find("boom") != std::string_view::npos, "InvokeException::message must carry the exception's what()");
    check(fragile.calls == 1, "the throwing method's side effect before the throw must still have happened");

    const MethodInfo *add_one_method = fragile_type.find_method("add_one");
    check(add_one_method != nullptr, "add_one must be found");
    const int add_one_arg = 41;
    const void *add_one_args[] = {&add_one_arg};
    int add_one_result = 0;
    InvokeException not_thrown{.threw = true, .message = UString{"stale"}}; // deliberately pre-set to confirm invoke_method clears it
    check(invoke_method(*add_one_method, &fragile, add_one_args, 1, &add_one_result, &not_thrown), "invoking a non-throwing method must succeed");
    check(!not_thrown.threw, "InvokeException::threw must be cleared for a call that didn't throw");
    check(add_one_result == 42, "a non-throwing method must still return its real result after the exception-safety wrapping");
    check(fragile.calls == 2, "the process must still be alive and the object still usable after a prior reflected call threw");

    // ── Inheritance (is_assignable_from) ──────────────────────────────────────────────────────
    const TypeInfo &base_type = registry.type<Base>();
    const TypeInfo &derived_type = registry.type<Derived>();
    const TypeInfo &unrelated_type = registry.type<Unrelated>();
    check(registry.is_assignable_from(base_type.key, derived_type.key), "Base must be assignable from Derived (Derived IS-A Base)");
    check(!registry.is_assignable_from(derived_type.key, base_type.key), "Derived must not be assignable from Base (Base is not a Derived)");
    check(registry.is_assignable_from(base_type.key, base_type.key), "a type must be assignable from itself");
    check(!registry.is_assignable_from(base_type.key, unrelated_type.key), "an unrelated type must not be assignable to Base");
    check(registry.find_field(derived_type, "base_value") != nullptr, "inherited-field lookup must still work alongside the new query");

    // ── Containers ─────────────────────────────────────────────────────────────────────────────
    const TypeInfo &inventory_type = registry.type<Inventory>();

    const FieldInfo *slots_field = inventory_type.find_field("slots");
    check(slots_field != nullptr && slots_field->container != nullptr, "std::array<int, 3> must carry a ContainerInfo");
    check(slots_field != nullptr && slots_field->container != nullptr && slots_field->container->fixed_size, "std::array's ContainerInfo must be marked fixed_size");
    check(slots_field != nullptr && !has_flag(slots_field->flags, FieldFlags::Trivial), "a fixed-size array field must not take the opaque Trivial fast path");

    const FieldInfo *ordered_scores_field = inventory_type.find_field("ordered_scores");
    check(ordered_scores_field != nullptr && ordered_scores_field->map != nullptr, "ordered std::map must carry a MapInfo, same as unordered_map");

    const FieldInfo *unlocked_levels_field = inventory_type.find_field("unlocked_levels");
    check(unlocked_levels_field != nullptr && unlocked_levels_field->set != nullptr, "std::set<int> must carry a SetInfo");

    const FieldInfo *equipped_weapon_field = inventory_type.find_field("equipped_weapon_id");
    check(equipped_weapon_field != nullptr && equipped_weapon_field->optional != nullptr, "std::unique_ptr<int> must reuse OptionalInfo");

    const FieldInfo *guild_name_field = inventory_type.find_field("guild_name");
    check(guild_name_field != nullptr && guild_name_field->optional != nullptr, "std::shared_ptr<UString> must reuse OptionalInfo");

    Inventory inventory{};
    check(slots_field != nullptr && container_resize(slots_field->container, &inventory.slots, 3), "resizing a std::array to its own size must succeed");
    check(slots_field != nullptr && !container_resize(slots_field->container, &inventory.slots, 4), "resizing a std::array to a different size must fail");
    const int slot_value = 99;
    check(slots_field != nullptr && container_set_element(slots_field->container, &inventory.slots, 1, &slot_value), "setting a std::array element must succeed");
    check(inventory.slots[1] == 99, "the std::array element must actually be set");

    const UString alice_name{"alice"};
    const int alice_score = 99;
    check(ordered_scores_field != nullptr && map_insert_or_assign(ordered_scores_field->map, &inventory.ordered_scores, &alice_name, &alice_score),
          "inserting into an ordered std::map through MapInfo must succeed");
    check(inventory.ordered_scores.at(UString{"alice"}) == 99, "the ordered map's entry must actually be inserted");

    const int level_value = 5;
    check(unlocked_levels_field != nullptr && set_insert(unlocked_levels_field->set, &inventory.unlocked_levels, &level_value), "inserting into a std::set through SetInfo must succeed");
    check(inventory.unlocked_levels.contains(5), "the set's element must actually be inserted");
    check(unlocked_levels_field != nullptr && set_contains(unlocked_levels_field->set, &inventory.unlocked_levels, &level_value), "SetInfo::contains must agree");

    const int weapon_id = 1234;
    check(equipped_weapon_field != nullptr && optional_set(equipped_weapon_field->optional, &inventory.equipped_weapon_id, &weapon_id),
          "emplacing a value into a std::unique_ptr through OptionalInfo must succeed");
    check(inventory.equipped_weapon_id != nullptr && *inventory.equipped_weapon_id == 1234, "the unique_ptr must actually hold the emplaced value");
    check(equipped_weapon_field != nullptr && optional_has_value(equipped_weapon_field->optional, &inventory.equipped_weapon_id),
          "OptionalInfo::has_value must agree for a populated unique_ptr");

    const UString guild_value{"Silver Hand"};
    check(guild_name_field != nullptr && optional_set(guild_name_field->optional, &inventory.guild_name, &guild_value),
          "emplacing a value into a std::shared_ptr through OptionalInfo must succeed");
    check(inventory.guild_name != nullptr && inventory.guild_name->cpp_string_view() == "Silver Hand", "the shared_ptr must actually hold the emplaced value");

    // Binary serialization round trip covering every new container shape at once.
    std::vector<std::byte> inventory_bytes;
    check(serialize_to_bytes(inventory_type, &inventory, inventory_bytes).has_value(), "serializing an Inventory with every new container shape must succeed");
    Inventory restored_inventory{};
    usize inventory_consumed = 0;
    check(deserialize_from_bytes(inventory_type, &restored_inventory, inventory_bytes, inventory_consumed).has_value(),
          "deserializing an Inventory with every new container shape must succeed");
    check(restored_inventory.slots == inventory.slots, "deserializing must restore a std::array field");
    check(restored_inventory.ordered_scores == inventory.ordered_scores, "deserializing must restore an ordered std::map field");
    check(restored_inventory.unlocked_levels == inventory.unlocked_levels, "deserializing must restore a std::set field");
    check(restored_inventory.equipped_weapon_id != nullptr && *restored_inventory.equipped_weapon_id == 1234,
          "deserializing must restore a std::unique_ptr field");
    check(restored_inventory.guild_name != nullptr && restored_inventory.guild_name->cpp_string_view() == "Silver Hand",
          "deserializing must restore a std::shared_ptr field");

    // Document (JSON-tree) round trip covering the same shapes.
    auto inventory_document = to_document(inventory_type, &inventory);
    check(inventory_document.has_value(), "to_document must succeed for every new container shape");
    Inventory restored_from_document{};
    check(from_document(inventory_type, &restored_from_document, *inventory_document).has_value(),
          "from_document must succeed for every new container shape");
    check(restored_from_document.slots == inventory.slots, "from_document must restore a std::array field");
    check(restored_from_document.unlocked_levels == inventory.unlocked_levels, "from_document must restore a std::set field");
    check(restored_from_document.equipped_weapon_id != nullptr && *restored_from_document.equipped_weapon_id == 1234,
          "from_document must restore a std::unique_ptr field");

    const UString inventory_json = write_json(*inventory_document);
    auto reparsed_inventory = parse_json(inventory_json.cpp_string_view());
    check(reparsed_inventory.has_value(), "the Inventory document must round-trip through JSON text");

    // ── Attribute reverse lookup ───────────────────────────────────────────────────────────────
    // `Taggable`/`NotTaggable` carry a *field*-level attribute (registered above via
    // SFT_ATTR_BOOL on the field itself), which `find_types_with_attribute` must not confuse
    // with a *type*-level one — it only ever inspects `TypeInfo::attributes`. Build a real
    // type-level attribute via `TypeInfoBuilder` (macro-based `SFT_REFLECT_TYPE` has no
    // type-attribute syntax) to exercise the actual contract.
    (void)registry.type<Taggable>();
    (void)registry.type<NotTaggable>();
    check(registry.find_types_with_attribute("modder_visible").empty(),
          "find_types_with_attribute must not match a field-level attribute against the type itself");

    TypeInfoBuilder tagged_builder{"test.reflection.capability.tagged_dynamic_type", sizeof(int), alignof(int)};
    tagged_builder.constructors(
        [](void *destination, void *source, void *) noexcept { ::new (destination) int(*static_cast<int *>(source)); },
        [](void *object, void *) noexcept { std::destroy_at(static_cast<int *>(object)); });
    tagged_builder.type_attribute(SFT_ATTR_BOOL("modder_visible", true));
    check(registry.register_type(tagged_builder.build()).has_value(), "registering a type with a type-level attribute must succeed");

    const std::vector<const TypeInfo *> tagged_types = registry.find_types_with_attribute("modder_visible");
    bool found_tagged_dynamic_type = false;
    for (const TypeInfo *type : tagged_types) {
        if (type->canonical_name.cpp_string_view() == "test.reflection.capability.tagged_dynamic_type") {
            found_tagged_dynamic_type = true;
        }
        check(type->canonical_name.cpp_string_view() != "test.reflection.capability.taggable" &&
                  type->canonical_name.cpp_string_view() != "test.reflection.capability.not_taggable",
              "find_types_with_attribute must not include a type whose attribute is only on a field");
    }
    check(found_tagged_dynamic_type, "find_types_with_attribute must find a type carrying a real type-level attribute");

    // ── Proxy ──────────────────────────────────────────────────────────────────────────────────
    struct ProxyState {
        int last_value = 0;
        bool saw_expected_key = false;
        TypeId expected_key;
    };
    ProxyState proxy_state{};
    proxy_state.expected_key = TypeId::from_name("do_something");

    ProxyObject proxy{};
    proxy.type = TypeId::from_name("test.reflection.capability.proxy_target");
    proxy.user_data = &proxy_state;
    proxy.dispatch = [](void *user_data, TypeId method_key, const void *const *args, usize arg_count, void *out_return, InvokeException *out_exception) noexcept {
        auto *state = static_cast<ProxyState *>(user_data);
        state->saw_expected_key = (method_key == state->expected_key);
        if (arg_count == 1) {
            state->last_value = *static_cast<const int *>(args[0]);
        }
        if (out_exception != nullptr) {
            *out_exception = InvokeException{};
        }
        if (out_return != nullptr) {
            ::new (out_return) int(state->last_value * 2);
        }
    };

    const int proxy_arg = 21;
    const void *proxy_args[] = {&proxy_arg};
    int proxy_result = 0;
    check(invoke_proxy_method(proxy, TypeId::from_name("do_something"), proxy_args, 1, &proxy_result), "invoke_proxy_method must dispatch when a dispatcher is set");
    check(proxy_state.saw_expected_key, "the proxy dispatcher must receive the exact method_key it was called with");
    check(proxy_state.last_value == 21, "the proxy dispatcher must receive the real arguments");
    check(proxy_result == 42, "the proxy dispatcher must be able to write a real return value");

    ProxyObject empty_proxy{};
    check(!invoke_proxy_method(empty_proxy, TypeId::from_name("anything"), nullptr, 0, nullptr), "invoke_proxy_method must fail cleanly when no dispatcher is set");

    // ── Ref-qualified / volatile method dispatch ──────────────────────────────────────────────
    // Expanding `MemberFunctionTraits` to cover every cv/ref/noexcept combination only made these
    // signatures *describable*; actually invoking an `&&`-qualified method additionally requires
    // the generated trampoline to move the receiver, since its implicit object parameter is `T&&`
    // and will not bind to an lvalue. Before that fix, merely declaring one of these with
    // SFT_REFLECT_METHOD_OVERLOAD failed to compile.
    const TypeInfo &ref_qualified_type = registry.type<RefQualified>();
    check(ref_qualified_type.methods.size() == 3, "all three ref/cv-qualified methods must register");

    RefQualified ref_qualified_instance{};
    ref_qualified_instance.ammo = 21;

    const MethodInfo *fire_method = ref_qualified_type.find_method("fire");
    int fire_result = 0;
    check(fire_method != nullptr && invoke_method(*fire_method, &ref_qualified_instance, nullptr, 0, &fire_result),
          "invoking an &&-qualified method must dispatch");
    check(fire_result == 42, "an &&-qualified method must run against the real receiver (21 * 2)");

    const MethodInfo *peek_method = ref_qualified_type.find_method("peek");
    int peek_result = 0;
    check(peek_method != nullptr && invoke_method(*peek_method, &ref_qualified_instance, nullptr, 0, &peek_result),
          "invoking a const &-qualified method must dispatch");
    check(peek_result == 21, "a const &-qualified method must read the real receiver");

    const MethodInfo *tweak_method = ref_qualified_type.find_method("tweak");
    const int tweak_arg = 7;
    const void *tweak_args[] = {&tweak_arg};
    check(tweak_method != nullptr && invoke_method(*tweak_method, &ref_qualified_instance, tweak_args, 1, nullptr),
          "invoking a volatile-qualified method must dispatch");
    check(ref_qualified_instance.ammo == 7, "a volatile-qualified method must mutate the real receiver");

    // ── Qualifier-preserving runtime signatures ───────────────────────────────────────────────
    // `MethodInfo::param_types` erases parameters through `remove_cvref_t`, so `const Blaster &`
    // and `Blaster` are indistinguishable there. `param_type_refs` carries what was lost, for
    // callers (FFI marshalling, script bindings, RPC stubs) that must know by-value from
    // by-reference. It is populated only when every type in the signature has a canonical
    // identity, so a method over an unreflected type still registers — just without the extra
    // fidelity.
    const TypeInfo &signatures_type = registry.type<Signatures>();

    const MethodInfo *aim_method = signatures_type.find_method("aim");
    check(aim_method != nullptr && aim_method->has_qualified_signature,
          "a fully identifiable signature must carry qualifier-preserving type refs");
    check(aim_method != nullptr && aim_method->param_type_refs.size() == 2,
          "the qualified signature must have one entry per declared parameter");
    check(aim_method != nullptr && aim_method->param_type_refs[0].base == type_id_for<Blaster>(),
          "a qualified parameter's base identity must match the erased one");
    check(aim_method != nullptr && has_flag(aim_method->param_type_refs[0].qualifiers, TypeQualifiers::Const),
          "const on a parameter must survive into the qualified signature");
    check(aim_method != nullptr && has_flag(aim_method->param_type_refs[0].qualifiers, TypeQualifiers::LValueRef),
          "a reference parameter must be recorded as a reference");
    check(aim_method != nullptr && aim_method->param_types[0] == type_id_for<Blaster>(),
          "the erased param_types must still be exactly what it always was");

    const MethodInfo *opaque_method = signatures_type.find_method("opaque");
    check(opaque_method != nullptr, "a method over an unreflected parameter type must still register");
    check(opaque_method != nullptr && !opaque_method->has_qualified_signature,
          "an unidentifiable signature must report that it has no qualified form");
    check(opaque_method != nullptr && opaque_method->param_type_refs.empty(),
          "an unidentifiable signature must leave the qualified refs empty rather than guess");

    if (failures != 0) {
        (void)std::fprintf(stderr, "CapabilityTest: %d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
