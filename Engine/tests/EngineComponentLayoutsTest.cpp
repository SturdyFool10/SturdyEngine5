// The engine's shareable components carry a layout, so a foreign definition of "sturdy.engine.transform" is checked against the C++
// one: the right layout resolves to the engine's component, a size-compatible but differently shaped one is rejected.

#include <Engine/Engine.hpp>
#include <Engine/ForeignEcs.hpp>

#include <cstdio>

using namespace SFT;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }
} // namespace

int main() {
    Engine::Engine engine;
    Ecs::ComponentRegistry &registry = engine.ecs_world().registry();

    const auto transform = registry.find(ustr{"sturdy.engine.transform"});
    check(transform.has_value(), "the engine registers its transform component");
    const Ecs::ComponentInfo *info = transform ? registry.info(*transform) : nullptr;
    check(info != nullptr && static_cast<bool>(info->layout) && info->schema.fields.size() == 3, "it carries a three-field layout");

    using Ecs::ComponentField;
    using Ecs::FieldKind;
    const Ecs::ComponentSchema right{.fields = {ComponentField{"t", FieldKind::F32, 0, 3}, ComponentField{"r", FieldKind::F32, 12, 4}, ComponentField{"s", FieldKind::F32, 28, 3}}};
    const auto same = Engine::register_foreign_component(registry, "sturdy.engine.transform", 40, 4, &right);
    check(same && transform && *same == *transform, "a foreign definition that matches resolves to the engine's component");

    // Ten floats in one run is the same bytes but not the same declaration of what is in them.
    const Ecs::ComponentSchema flat{.fields = {ComponentField{"all", FieldKind::F32, 0, 10}}};
    const auto flat_result = Engine::register_foreign_component(registry, "sturdy.engine.transform", 40, 4, &flat);
    check(!flat_result && flat_result.error().find("sturdy.engine.transform") != std::string::npos, "a differently shaped definition of the same bytes is rejected");

    const Ecs::ComponentSchema as_ints{.fields = {ComponentField{"t", FieldKind::U32, 0, 3}, ComponentField{"r", FieldKind::U32, 12, 4}, ComponentField{"s", FieldKind::U32, 28, 3}}};
    check(!Engine::register_foreign_component(registry, "sturdy.engine.transform", 40, 4, &as_ints), "the same offsets read as integers are rejected");

    const auto world_transform = registry.find(ustr{"sturdy.engine.world_transform"});
    const Ecs::ComponentInfo *world_info = world_transform ? registry.info(*world_transform) : nullptr;
    check(world_info != nullptr && world_info->schema.fields.size() == 1 && world_info->schema.fields[0].count == 16, "the world matrix is sixteen floats");

    const auto parent = registry.find(ustr{"sturdy.engine.parent"});
    const Ecs::ComponentInfo *parent_info = parent ? registry.info(*parent) : nullptr;
    check(parent_info != nullptr && parent_info->schema.fields.size() == 2, "a parent link is an index and a generation");

    if (failures != 0) {
        std::fprintf(stderr, "EngineComponentLayoutsTest: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("EngineComponentLayoutsTest: ok\n");
    return 0;
}
