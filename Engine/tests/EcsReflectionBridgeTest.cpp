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

} // namespace

SFT_ECS_COMPONENT(Health, "test.engine.ecs_reflection.health");
SFT_REFLECT_TYPE(Health, "test.engine.ecs_reflection.health");
SFT_REFLECT_FIELD(current);
SFT_REFLECT_FIELD(max);
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

    const Ecs::Entity entity = world.spawn(Health{.current = 42, .max = 100});

    int read_current = 0;
    const auto read = Engine::read_component_field(world, entity, component_id, *type, "current", &read_current, sizeof(read_current));
    check(read.has_value(), "read_component_field must succeed for a live entity and a real field");
    check(read_current == 42, "read_component_field must return the entity's live value");

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
