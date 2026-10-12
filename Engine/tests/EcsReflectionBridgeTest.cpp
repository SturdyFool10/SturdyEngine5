/// Coverage for the Ecs<->Reflection bridge (`Engine/EcsReflection.hpp`): `ensure_reflected`
/// catching a deliberately-mismatched descriptor, and `read_component_field`/
/// `write_component_field` round-tripping a named field through `World`'s type-erased
/// read/write path.

#include <Engine/EcsReflection.hpp>

#include <Ecs/Component.hpp>

#include <cstdio>

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "EcsReflectionBridgeTest: %s\n", description);
            ++failures;
        }
    }

    struct Health {
        int current = 100;
        int max = 100;
    };

    struct Drifted {
        int only_field = 0;
    };

    /// Natural alignment padding between fields is fine: {u8 @0, f32 @4}, 8 bytes.
    struct Padded {
        std::uint8_t flag = 0;
        float value = 0.0f;
    };

    /// Only one of two members is reflected, so the reflected fields do not account for the whole struct.
    struct PartlyReflected {
        int shown = 0;
        int hidden = 0;
    };

    struct Inner {
        float x = 0.0f;
    };

    /// A non-scalar member: no complete shape can be stated.
    struct Nested {
        Inner inner{};
        int count = 0;
    };

} // namespace

SFT_ECS_COMPONENT(Health, "test.engine.ecs_reflection.health");
SFT_REFLECT_TYPE(Health, "test.engine.ecs_reflection.health");
SFT_REFLECT_FIELD(current);
SFT_REFLECT_FIELD(max);
SFT_REFLECT_END();

SFT_ECS_COMPONENT(Padded, "test.engine.ecs_reflection.padded");
SFT_REFLECT_TYPE(Padded, "test.engine.ecs_reflection.padded");
SFT_REFLECT_FIELD(flag);
SFT_REFLECT_FIELD(value);
SFT_REFLECT_END();

SFT_ECS_COMPONENT(PartlyReflected, "test.engine.ecs_reflection.partly");
SFT_REFLECT_TYPE(PartlyReflected, "test.engine.ecs_reflection.partly");
SFT_REFLECT_FIELD(shown);
SFT_REFLECT_END();

SFT_ECS_COMPONENT(Nested, "test.engine.ecs_reflection.nested");
SFT_REFLECT_TYPE(Nested, "test.engine.ecs_reflection.nested");
SFT_REFLECT_FIELD(inner);
SFT_REFLECT_FIELD(count);
SFT_REFLECT_END();

SFT_ECS_COMPONENT(Drifted, "test.engine.ecs_reflection.drifted");
SFT_REFLECT_TYPE(Drifted, "test.engine.ecs_reflection.drifted");
SFT_REFLECT_FIELD(only_field);
SFT_REFLECT_END();

int main() {
    using namespace SFT;

    Ecs::ComponentRegistry components;
    Ecs::World world(components);

    const auto reflected = Engine::ensure_reflected<Health>(components);
    check(reflected.has_value(), "ensure_reflected must succeed when the component and reflection descriptors agree");

    // Simulates a stale/mis-registered ComponentInfo: register Drifted by hand with a size that
    // no longer agrees with sizeof(Drifted) (ComponentRegistry keeps whatever was registered
    // first under a matching name rather than re-deriving it), then confirm ensure_reflected
    // catches the disagreement instead of silently trusting it.
    Ecs::ComponentInfo drifted_info = Ecs::Detail::make_component_info<Drifted>();
    drifted_info.size = drifted_info.size + 4;
    const auto manually_registered = components.register_component(drifted_info);
    check(manually_registered.has_value(), "manually registering a structurally-valid (if stale) ComponentInfo must succeed");

    const auto drifted = Engine::ensure_reflected<Drifted>(components);
    check(!drifted.has_value(), "ensure_reflected must reject a stale ComponentInfo whose size disagrees with TypeInfo");
    check(!drifted.has_value() || drifted.error().code == Engine::EcsReflectionBridgeErrorCode::DescriptorMismatch,
          "a size mismatch must be reported as DescriptorMismatch");

    // Name-based overload: no C++ type, just two registrations that must be tied together.
    const auto by_name = Engine::ensure_reflected(components, "test.engine.ecs_reflection.health", "test.engine.ecs_reflection.health");
    check(by_name.has_value() && reflected.has_value() && by_name->first == reflected->first && by_name->second == reflected->second,
          "the name-based ensure_reflected must resolve to the same component id and type key");
    const auto unknown_component = Engine::ensure_reflected(components, "test.engine.ecs_reflection.nope", "test.engine.ecs_reflection.health");
    check(!unknown_component.has_value() && unknown_component.error().code == Engine::EcsReflectionBridgeErrorCode::ComponentNotRegistered,
          "an unknown component name must fail with ComponentNotRegistered");
    const auto unknown_type = Engine::ensure_reflected(components, "test.engine.ecs_reflection.health", "test.engine.ecs_reflection.nope");
    check(!unknown_type.has_value() && unknown_type.error().code == Engine::EcsReflectionBridgeErrorCode::TypeNotRegistered,
          "an unknown type name must fail with TypeNotRegistered");
    (void)Reflection::TypeRegistry::instance().type<Drifted>(); // ensure_reflected<Drifted> bailed out before registering it
    const auto mismatched = Engine::ensure_reflected(components, "test.engine.ecs_reflection.health", "test.engine.ecs_reflection.drifted");
    check(!mismatched.has_value() && mismatched.error().code == Engine::EcsReflectionBridgeErrorCode::DescriptorMismatch,
          "tying a component to a differently sized type must fail with DescriptorMismatch");

    if (!reflected.has_value()) {
        (void)std::fprintf(stderr, "EcsReflectionBridgeTest: %d check(s) failed\n", failures);
        return 1;
    }
    const auto [component_id, type_key] = *reflected;

    const Reflection::TypeInfo *type = Reflection::TypeRegistry::instance().find(type_key);
    check(type != nullptr, "the reflected type must be findable in TypeRegistry after ensure_reflected");
    if (type == nullptr) {
        (void)std::fprintf(stderr, "EcsReflectionBridgeTest: %d check(s) failed\n", failures);
        return 1;
    }

    // ---- layouts derived from reflection (so foreign registrations of the same component are checked against them) ----
    {
        const Ecs::ComponentInfo *health_info = components.info(component_id);
        const std::vector<Ecs::ComponentField> health_fields{{"a", Ecs::FieldKind::I32, 0, 1}, {"b", Ecs::FieldKind::I32, 4, 1}};
        check(health_info != nullptr && health_info->layout == Ecs::fingerprint_component_layout(8, 4, health_fields),
              "a reflected component of two ints gets the layout a foreign {i32, i32} would describe");

        const auto padded = Engine::ensure_reflected<Padded>(components);
        const Ecs::ComponentInfo *padded_info = padded ? components.info(padded->first) : nullptr;
        const std::vector<Ecs::ComponentField> padded_fields{{"flag", Ecs::FieldKind::U8, 0, 1}, {"value", Ecs::FieldKind::F32, 4, 1}};
        check(padded_info != nullptr && padded_info->layout == Ecs::fingerprint_component_layout(8, 4, padded_fields),
              "alignment padding between fields is accepted and does not change the layout");

        const auto partly = Engine::ensure_reflected<PartlyReflected>(components);
        check(partly.has_value(), "a partly reflected component still registers");
        const Ecs::ComponentInfo *partly_info = partly ? components.info(partly->first) : nullptr;
        check(partly_info != nullptr && !partly_info->layout, "reflected fields that do not cover the struct give no shape, rather than a partial one");

        const auto nested = Engine::ensure_reflected<Nested>(components);
        const Ecs::ComponentInfo *nested_info = nested ? components.info(nested->first) : nullptr;
        check(nested_info != nullptr && !nested_info->layout, "a non-scalar member gives no shape");

        // A foreign registration that describes the same memory differently is now caught against the C++ definition.
        const Reflection::TypeInfo &health_type = Reflection::TypeRegistry::instance().type<Health>();
        const auto derived = Engine::derive_component_schema(health_type);
        check(derived.has_value() && derived->fields.size() == 2 && derived->fields[1].offset == 4, "derive_component_schema reports each field's offset");
        auto as_floats = Ecs::Detail::make_component_info<Health>();
        as_floats.schema.fields = {{"a", Ecs::FieldKind::F32, 0, 1}, {"b", Ecs::FieldKind::F32, 4, 1}};
        const auto aliased = components.register_component(as_floats);
        check(!aliased && aliased.error().code == Ecs::ComponentRegistryErrorCode::SchemaMismatch,
              "registering a reflected C++ component with the same size but float fields is a SchemaMismatch");
    }

    const Ecs::Entity entity = world.spawn(Health{.current = 42, .max = 100});

    int read_current = 0;
    const auto read = Engine::read_component_field(world, entity, component_id, *type, "current", &read_current, sizeof(read_current));
    check(read.has_value(), "read_component_field must succeed for a live entity and a real field");
    check(read_current == 42, "read_component_field must return the entity's live value");

    const Ecs::World &const_world = world;
    int read_through_const = 0;
    check(Engine::read_component_field(const_world, entity, component_id, *type, "current", &read_through_const, sizeof(read_through_const)).has_value() &&
              read_through_const == 42,
          "read_component_field must work through a const World");

    const int new_current = 7;
    const auto written = Engine::write_component_field(world, entity, component_id, *type, "current", &new_current, sizeof(new_current));
    check(written.has_value(), "write_component_field must succeed for a live entity and a real field");

    Health after{};
    check(world.read_component_erased(entity, component_id, &after, sizeof(after)).has_value(),
          "reading the whole component after write_component_field must succeed");
    check(after.current == 7, "write_component_field must have actually mutated the live component");
    check(after.max == 100, "write_component_field must not disturb fields it wasn't asked to touch");

    const auto missing_field = Engine::read_component_field(world, entity, component_id, *type, "does_not_exist", &read_current, sizeof(read_current));
    check(!missing_field.has_value() && missing_field.error().code == Engine::EcsReflectionBridgeErrorCode::FieldNotFound,
          "reading a nonexistent field must fail with FieldNotFound");

    world.destroy(entity);
    const auto dead_entity = Engine::read_component_field(world, entity, component_id, *type, "current", &read_current, sizeof(read_current));
    check(!dead_entity.has_value() && dead_entity.error().code == Engine::EcsReflectionBridgeErrorCode::WorldAccessFailed,
          "reading from a destroyed entity must fail with WorldAccessFailed, not crash");

    if (failures != 0) {
        (void)std::fprintf(stderr, "EcsReflectionBridgeTest: %d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
