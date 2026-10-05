#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <Ecs/World.hpp>
#include <Engine/Engine.hpp>

#include <CxxApi/Types/Scene.hpp>

/// Entities and the transform hierarchy (`Engine/Transform.hpp`). Functions take the world, which a binding gets from
/// `engine_world`; a `Transform` is local to the entity's parent, and the engine composes `WorldTransform` from them
/// every update and again before rendering.
///
/// Every function here changes or reads the world directly, so call them from game-logic callbacks, never from inside an ECS
/// system while the world's schedule runs (the ECS aborts on that misuse rather than racing).
namespace SFT::CxxApi {

    [[nodiscard]] Ecs::World &engine_world(Engine::Engine &engine) noexcept;

    /// The engine's default placement (identity).
    [[nodiscard]] TransformDesc transform_desc_defaults() noexcept;

    /// A new entity with a `Transform` and a `WorldTransform`.
    [[nodiscard]] Entity scene_spawn(Ecs::World &world, const TransformDesc &transform);
    [[nodiscard]] bool scene_is_alive(const Ecs::World &world, Entity entity) noexcept;
    /// Destroys the entity and everything parented under it.
    void scene_destroy_recursive(Ecs::World &world, Entity entity);

    /// Sets the local placement, adding `Transform`/`WorldTransform` when missing. Returns false for a dead entity.
    bool scene_set_transform(Ecs::World &world, Entity entity, const TransformDesc &transform);
    /// The local placement; false when the entity is dead or has no `Transform`.
    [[nodiscard]] bool scene_transform(Ecs::World &world, Entity entity, TransformDesc &out);
    /// Column-major world matrix composed from the ancestors right now (no need to wait for propagation).
    [[nodiscard]] std::array<float, 16> scene_world_matrix(Ecs::World &world, Entity entity);

    /// Parents `child` to `parent`, keeping its world pose when `keep_world_pose`. False (nothing changed) when either is
    /// dead, they are the same entity, or the link would make a loop.
    bool scene_set_parent(Ecs::World &world, Entity child, Entity parent, bool keep_world_pose);
    bool scene_clear_parent(Ecs::World &world, Entity child, bool keep_world_pose);
    /// The parent, or an entity with `generation == 0`.
    [[nodiscard]] Entity scene_parent(Ecs::World &world, Entity child);
    [[nodiscard]] std::unique_ptr<std::vector<Entity>> scene_children(Ecs::World &world, Entity parent);

    /// Recomputes every `WorldTransform` now (the engine also does this each update and before rendering).
    TransformPropagationStats scene_propagate_transforms(Ecs::World &world);

} // namespace SFT::CxxApi
