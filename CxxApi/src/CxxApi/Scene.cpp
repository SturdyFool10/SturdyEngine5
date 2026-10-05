#include <CxxApi/Scene.hpp>

#include <Engine/Transform.hpp>

#include <CxxApi/Conversions.hpp>

namespace SFT::CxxApi {

    namespace {
        namespace E = SFT::Engine;

        [[nodiscard]] Ecs::Entity to_entity(Entity e) noexcept { return Ecs::Entity{.index = e.index, .generation = e.generation}; }
        [[nodiscard]] Entity from_entity(Ecs::Entity e) noexcept { return Entity{.index = e.index, .generation = e.generation}; }

        [[nodiscard]] E::Transform to_transform(const TransformDesc &d) noexcept {
            return E::Transform{.translation = to_vec3(d.translation), .rotation = to_quat(d.rotation), .scale = to_vec3(d.scale)};
        }
        [[nodiscard]] TransformDesc from_transform(const E::Transform &t) noexcept {
            TransformDesc d{};
            store(d.translation, t.translation);
            store(d.rotation, t.rotation);
            store(d.scale, t.scale);
            return d;
        }
    } // namespace

    Ecs::World &engine_world(E::Engine &engine) noexcept { return engine.ecs_world(); }

    TransformDesc transform_desc_defaults() noexcept { return from_transform(E::Transform{}); }

    Entity scene_spawn(Ecs::World &world, const TransformDesc &transform) {
        const E::Transform local = to_transform(transform);
        return from_entity(world.spawn(E::Transform{local}, E::WorldTransform{local.matrix()}));
    }

    bool scene_is_alive(const Ecs::World &world, Entity entity) noexcept { return world.is_alive(to_entity(entity)); }

    void scene_destroy_recursive(Ecs::World &world, Entity entity) {
        if (world.is_alive(to_entity(entity))) {
            E::destroy_recursive(world, to_entity(entity));
        }
    }

    bool scene_set_transform(Ecs::World &world, Entity entity, const TransformDesc &transform) {
        const Ecs::Entity e = to_entity(entity);
        if (!world.is_alive(e)) {
            return false;
        }
        const E::Transform local = to_transform(transform);
        bool has_transform = false;
        if (auto existing = world.get_component<E::Transform>(e)) {
            *existing = local;
            has_transform = true;
        }
        if (!has_transform) {
            world.add_component(e, local);
        }
        const bool has_world = static_cast<bool>(world.get_component<E::WorldTransform>(e));
        if (!has_world) {
            world.add_component(e, E::WorldTransform{E::compute_world_matrix(world, e)});
        }
        return true;
    }

    bool scene_transform(Ecs::World &world, Entity entity, TransformDesc &out) {
        const Ecs::Entity e = to_entity(entity);
        if (!world.is_alive(e)) {
            return false;
        }
        if (auto transform = world.get_component<E::Transform>(e)) {
            out = from_transform(*transform);
            return true;
        }
        return false;
    }

    std::array<float, 16> scene_world_matrix(Ecs::World &world, Entity entity) {
        return to_array(E::compute_world_matrix(world, to_entity(entity)));
    }

    bool scene_set_parent(Ecs::World &world, Entity child, Entity parent, bool keep_world_pose) {
        return E::set_parent(world, to_entity(child), to_entity(parent), keep_world_pose);
    }

    bool scene_clear_parent(Ecs::World &world, Entity child, bool keep_world_pose) {
        return E::clear_parent(world, to_entity(child), keep_world_pose);
    }

    Entity scene_parent(Ecs::World &world, Entity child) { return from_entity(E::parent_of(world, to_entity(child))); }

    std::unique_ptr<std::vector<Entity>> scene_children(Ecs::World &world, Entity parent) {
        auto out = std::make_unique<std::vector<Entity>>();
        for (const Ecs::Entity child : E::children_of(world, to_entity(parent))) {
            out->push_back(from_entity(child));
        }
        return out;
    }

    TransformPropagationStats scene_propagate_transforms(Ecs::World &world) {
        const E::PropagationStats stats = E::propagate_transforms(world);
        return TransformPropagationStats{.roots = stats.roots, .children = stats.children, .orphans = stats.orphans, .cycles = stats.cycles};
    }

} // namespace SFT::CxxApi
