// Component field layouts: validation, the layout fingerprint's properties, and how the registry uses them to stop two
// languages' definitions of one component from silently aliasing different memory.

#include <Ecs/Component.hpp>

#include <cstdio>
#include <cstring>
#include <string>

using namespace SFT;
using namespace SFT::Ecs;

namespace {

    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }

    /// A 12-byte component: `float position[3]`.
    ComponentSchema position_schema(const char *name = "position") {
        return ComponentSchema{.fields = {ComponentField{.name = name, .kind = FieldKind::F32, .offset = 0, .count = 3}}};
    }

    ComponentInfo make_info(const char *name, usize size, usize align, ComponentSchema schema = {}) {
        ComponentInfo info{};
        info.key = ComponentKey::from_name(name);
        info.canonical_name = UString{name};
        info.size = size;
        info.align = align;
        info.flags = ComponentFlags::TriviallyCopyable | ComponentFlags::TriviallyDestructible | ComponentFlags::FfiBlittable;
        info.default_construct = [](void *, void *) noexcept {};
        info.copy_construct = [](void *, const void *, void *) noexcept {};
        info.move_construct = [](void *, void *, void *) noexcept {};
        info.destroy = [](void *, void *) noexcept {};
        info.schema = std::move(schema);
        return info;
    }

} // namespace

int main() {
    // ---- validation ----
    check(!validate_component_fields(12, position_schema().fields), "a float[3] fits a 12-byte component");
    check(validate_component_fields(8, position_schema().fields).has_value(), "a float[3] does not fit 8 bytes");
    check(validate_component_fields(12, std::vector{ComponentField{"a", FieldKind::F32, 0, 0}}).has_value(), "a zero count is rejected");
    check(validate_component_fields(12, std::vector{ComponentField{"a", FieldKind::F32, 0, 2}, ComponentField{"b", FieldKind::F32, 4, 1}})
              .has_value(),
          "overlapping fields are rejected");
    check(validate_component_fields(12, std::vector{ComponentField{"a", static_cast<FieldKind>(99), 0, 1}}).has_value(), "an unknown kind is rejected");
    check(!validate_component_fields(12, std::vector{ComponentField{"b", FieldKind::F32, 4, 2}, ComponentField{"a", FieldKind::F32, 0, 1}}),
          "fields may be listed in any order");
    check(!validate_component_fields(4, {}), "no fields is valid (shape unspecified)");

    // ---- fingerprint properties ----
    const auto base = fingerprint_component_layout(12, 4, position_schema().fields);
    check(static_cast<bool>(base), "a fingerprint is never the all-zero 'none' value");
    check(base == fingerprint_component_layout(12, 4, position_schema("pos").fields), "field names do not change the fingerprint");
    const std::vector<ComponentField> reordered{{"b", FieldKind::F32, 4, 2}, {"a", FieldKind::F32, 0, 1}};
    const std::vector<ComponentField> ordered{{"a", FieldKind::F32, 0, 1}, {"b", FieldKind::F32, 4, 2}};
    check(fingerprint_component_layout(12, 4, reordered) == fingerprint_component_layout(12, 4, ordered), "listing order does not change it");
    check(base != fingerprint_component_layout(12, 4, std::vector{ComponentField{"x", FieldKind::U32, 0, 3}}),
          "the same bytes read as u32 instead of f32 differ");
    check(base != fingerprint_component_layout(16, 4, position_schema().fields), "a different size differs");
    check(base != fingerprint_component_layout(12, 8, position_schema().fields), "a different alignment differs");
    check(base != fingerprint_component_layout(12, 4, std::vector{ComponentField{"x", FieldKind::F32, 0, 2}}), "a different count differs");

    // ---- the difference message names what differs ----
    check(describe_layout_difference(12, 4, position_schema().fields, 12, 4, position_schema("pos").fields).empty(), "identical layouts have no difference");
    const std::string kind_diff = describe_layout_difference(12, 4, position_schema().fields, 12, 4, std::vector{ComponentField{"x", FieldKind::U32, 0, 3}});
    check(kind_diff.find("f32") != std::string::npos && kind_diff.find("u32") != std::string::npos, "the message quotes both field kinds");
    check(describe_layout_difference(12, 4, {}, 16, 4, {}).find("size") != std::string::npos, "the message reports a size difference");

    // ---- registry behaviour ----
    {
        ComponentRegistry registry;
        const auto first = registry.register_component(make_info("game.position", 12, 4, position_schema("position")));
        check(first.has_value(), "a component with a shape registers");
        const ComponentInfo *stored = first ? registry.info(*first) : nullptr;
        check(stored != nullptr && stored->layout == base, "the registry stores the fingerprint it derived from the fields");

        const auto again = registry.register_component(make_info("game.position", 12, 4, position_schema("pos")));
        check(again && first && *again == *first, "the same shape under other field names returns the same id");

        const auto wrong = registry.register_component(make_info("game.position", 12, 4, ComponentSchema{.fields = {ComponentField{"x", FieldKind::U32, 0, 3}}}));
        check(!wrong && wrong.error().code == ComponentRegistryErrorCode::SchemaMismatch, "a different shape is a SchemaMismatch");
        check(!wrong && std::string{wrong.error().message.cpp_string_view()}.find("game.position") != std::string::npos, "the error names the component");

        const auto shapeless = registry.register_component(make_info("game.position", 12, 4));
        check(shapeless && first && *shapeless == *first, "a registration with no shape is still accepted, as before");

        const auto bad = registry.register_component(make_info("game.bad", 8, 4, position_schema()));
        check(!bad && bad.error().code == ComponentRegistryErrorCode::InvalidDescriptor, "a shape that does not fit the size is invalid");
    }

    // A binding that registers an engine component first (FfiBlittable) must not make the engine's own registration collide.
    {
        ComponentRegistry registry;
        const auto from_binding = registry.register_component(make_info("engine.thing", 12, 4));
        ComponentInfo from_engine = make_info("engine.thing", 12, 4);
        from_engine.flags = ComponentFlags::TriviallyCopyable | ComponentFlags::TriviallyDestructible;
        const auto engine_side = registry.register_component(std::move(from_engine));
        check(from_binding && engine_side && *from_binding == *engine_side, "FfiBlittable origin does not make a registration incompatible");
        ComponentInfo different = make_info("engine.thing", 12, 4);
        different.flags = ComponentFlags::TriviallyCopyable;
        const auto mismatch = registry.register_component(std::move(different));
        check(!mismatch && mismatch.error().code == ComponentRegistryErrorCode::StableKeyCollision, "other flag differences still collide");
    }

    if (failures != 0) {
        std::fprintf(stderr, "ComponentSchemaTest: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("ComponentSchemaTest: ok\n");
    return 0;
}
