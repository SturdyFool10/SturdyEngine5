#include <Engine/EngineComponentLayouts.hpp>

#include <cstddef>

#include <Engine/EcsRendering.hpp>
#include <Engine/Transform.hpp>

namespace SFT::Engine {

    namespace {

        // glm stores a quaternion as x, y, z, w (its constructor takes w first, its memory does not), a mat4 as 16 floats in
        // column-major order, and a vec3 as 3 floats with no padding.
        static_assert(sizeof(glm::vec3) == 3 * sizeof(float) && sizeof(glm::quat) == 4 * sizeof(float) && sizeof(glm::mat4) == 16 * sizeof(float));
        static_assert(std::is_same_v<decltype(glm::vec3::x), float>);

        static_assert(sizeof(Transform) == 40 && alignof(Transform) == 4);
        static_assert(offsetof(Transform, translation) == 0 && offsetof(Transform, rotation) == 12 && offsetof(Transform, scale) == 28);

        static_assert(sizeof(WorldTransform) == 64 && alignof(WorldTransform) == alignof(glm::mat4));
        static_assert(offsetof(WorldTransform, value) == 0);

        static_assert(sizeof(Parent) == 8 && alignof(Parent) == 4);
        static_assert(sizeof(Ecs::Entity) == 8 && offsetof(Ecs::Entity, index) == 0 && offsetof(Ecs::Entity, generation) == 4);

        void describe(Ecs::ComponentRegistry &registry, Ecs::ComponentId id, Ecs::ComponentSchema schema) {
            // A component that already carries a different layout is a bug in this file, not something to ignore silently.
            if (auto described = registry.describe_layout(id, std::move(schema)); !described) {
                Ecs::Detail::contract_violation("engine component layout declaration is wrong: {}", described.error().message);
            }
        }

    } // namespace

    void describe_engine_component_layouts(Ecs::ComponentRegistry &registry) {
        using Ecs::ComponentField;
        using Ecs::FieldKind;
        describe(registry, registry.component<Transform>(),
                 Ecs::ComponentSchema{.fields = {ComponentField{"translation", FieldKind::F32, 0, 3},
                                                 ComponentField{"rotation_xyzw", FieldKind::F32, 12, 4},
                                                 ComponentField{"scale", FieldKind::F32, 28, 3}}});
        describe(registry, registry.component<WorldTransform>(),
                 Ecs::ComponentSchema{.fields = {ComponentField{"matrix_column_major", FieldKind::F32, 0, 16}}});
        describe(registry, registry.component<Parent>(),
                 Ecs::ComponentSchema{.fields = {ComponentField{"entity_index", FieldKind::U32, 0, 1},
                                                 ComponentField{"entity_generation", FieldKind::U32, 4, 1}}});
    }

} // namespace SFT::Engine
