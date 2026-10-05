#pragma once

#include <Ecs/Entity.hpp>
#include <Ecs/World.hpp>
#include <Foundation/Foundation.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <vector>

#include <Engine/EcsRendering.hpp>

/// Transforms and parenting.
///
/// An entity is placed by a `Transform` (translation, rotation, scale) that is relative to its `Parent`, or to the world when
/// it has none. Every frame the engine composes them parent-first into `WorldTransform`, the matrix the renderer, audio,
/// physics and animation read. Entities that set `WorldTransform` directly and carry no `Transform` keep working unchanged:
/// they are simply not driven by the hierarchy (and can still be parents).
///
/// ```cpp
/// const Entity car = world.spawn(Transform{.translation = {0, 0, -10}}, WorldTransform{}, ModelRenderer{car_model});
/// const Entity wheel = world.spawn(Transform{.translation = {1, -0.3f, 1.4f}}, WorldTransform{}, ModelRenderer{wheel_model});
/// set_parent(world, wheel, car);              // the wheel now moves with the car
/// world.get_component<Transform>(car)->translation.x += 1.0f;
/// ```
///
/// The engine propagates at the start of `Engine::update` and again just before the renderer extracts the scene, so a
/// `Transform` changed anywhere in a frame is seen by rendering that frame. `propagate_transforms` can be called by hand.
namespace SFT::Engine {

    /// Local placement relative to the parent (or the world).
    struct Transform {
        glm::vec3 translation{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};

        [[nodiscard]] glm::mat4 matrix() const noexcept;
        /// The TRS that reproduces `matrix` (shear is dropped; negative scale is folded into the rotation's handedness).
        [[nodiscard]] static Transform from_matrix(const glm::mat4 &matrix) noexcept;
        /// Points the forward axis (-Z) at `target`, keeping `up` as close to up as possible.
        [[nodiscard]] static Transform looking_at(const glm::vec3 &position, const glm::vec3 &target, const glm::vec3 &up = {0.0f, 1.0f, 0.0f}) noexcept;

        [[nodiscard]] glm::vec3 forward() const noexcept { return rotation * glm::vec3{0.0f, 0.0f, -1.0f}; }
        [[nodiscard]] glm::vec3 right() const noexcept { return rotation * glm::vec3{1.0f, 0.0f, 0.0f}; }
        [[nodiscard]] glm::vec3 up() const noexcept { return rotation * glm::vec3{0.0f, 1.0f, 0.0f}; }
    };

    /// Makes the entity's `Transform` relative to `entity`'s world transform. A parent that is gone (destroyed) is treated
    /// as the world origin until the child is reparented or `propagate_transforms` clears the link.
    struct Parent {
        Ecs::Entity entity{};
    };

    struct PropagationStats {
        u32 roots = 0;
        u32 children = 0;
        /// Children whose parent no longer exists; their `Parent` was removed and they now hang off the world.
        u32 orphans = 0;
        /// Children that sat in a parent loop; they are placed relative to the world and reported here.
        u32 cycles = 0;
    };

    /// Recomputes `WorldTransform` for every entity with a `Transform`, parents before children. Must not run while the
    /// world's schedule is executing (it is a direct, exclusive pass).
    PropagationStats propagate_transforms(Ecs::World &world);

    /// Parents `child` to `parent`. With `keep_world_pose` the child's `Transform` is rewritten so it stays where it is in
    /// the world; without it the current `Transform` becomes relative to the new parent. Adds `Transform`/`WorldTransform`
    /// to the child if it lacks them (taken from its current world matrix). Returns false (and changes nothing) when either
    /// entity is dead, they are the same entity, or the link would create a loop.
    bool set_parent(Ecs::World &world, Ecs::Entity child, Ecs::Entity parent, bool keep_world_pose = true);
    /// Detaches `child` from its parent, keeping its world pose by default.
    bool clear_parent(Ecs::World &world, Ecs::Entity child, bool keep_world_pose = true);
    /// The parent, or an invalid entity.
    [[nodiscard]] Ecs::Entity parent_of(Ecs::World &world, Ecs::Entity child);
    /// Direct children (a scan over every parented entity).
    [[nodiscard]] std::vector<Ecs::Entity> children_of(Ecs::World &world, Ecs::Entity parent);
    /// Destroys `root` and everything parented under it, at any depth.
    void destroy_recursive(Ecs::World &world, Ecs::Entity root);
    /// The world matrix of an entity right now (computed from its ancestors' `Transform`s without waiting for propagation).
    [[nodiscard]] glm::mat4 compute_world_matrix(Ecs::World &world, Ecs::Entity entity);

} // namespace SFT::Engine

SFT_ECS_COMPONENT(SFT::Engine::Transform, "sturdy.engine.transform");
SFT_ECS_COMPONENT(SFT::Engine::Parent, "sturdy.engine.parent");
