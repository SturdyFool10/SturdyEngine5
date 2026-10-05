// Transform hierarchy: TRS composition, parent-first propagation, reparenting with and without keeping the world pose,
// loops refused, orphans released, recursive destruction. Uses plain checks so it means the same in every build type.

#include <Engine/Transform.hpp>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>

using namespace SFT;
using namespace SFT::Engine;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }
    bool near(const glm::vec3 &a, const glm::vec3 &b, float eps = 1e-4f) { return glm::length(a - b) <= eps; }
    glm::vec3 position(Ecs::World &world, Ecs::Entity e) {
        auto t = world.get_component<WorldTransform>(e);
        return t ? glm::vec3{t->value[3]} : glm::vec3{NAN};
    }
} // namespace

int main() {
    // TRS round trip, including a mirrored scale.
    {
        Transform t;
        t.translation = {1, 2, 3};
        t.rotation = glm::angleAxis(0.7f, glm::normalize(glm::vec3{1, 2, 0.5f}));
        t.scale = {2, -0.5f, 3};
        const Transform back = Transform::from_matrix(t.matrix());
        bool same = true;
        const glm::mat4 a = t.matrix(), b = back.matrix();
        for (int c = 0; c < 4; ++c) same = same && near(glm::vec3{a[c]}, glm::vec3{b[c]});
        check(same, "a decomposed matrix rebuilds the same matrix, mirrored scale included");
        const Transform look = Transform::looking_at({0, 0, 0}, {5, 0, 0});
        check(near(look.forward(), {1, 0, 0}) && near(look.up(), {0, 1, 0}), "looking_at points -Z at the target with +Y up");
    }

    Ecs::ComponentRegistry registry;
    Ecs::World world{registry};

    const Ecs::Entity root = world.spawn(Transform{.translation = {10, 0, 0}, .rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3{0, 1, 0})}, WorldTransform{});
    const Ecs::Entity child = world.spawn(Transform{.translation = {0, 0, -2}}, WorldTransform{});
    const Ecs::Entity grandchild = world.spawn(Transform{.translation = {1, 0, 0}, .scale = glm::vec3{2.0f}}, WorldTransform{});
    const Ecs::Entity authored = world.spawn(WorldTransform{glm::translate(glm::mat4{1.0f}, glm::vec3{0, 5, 0})}); // no Transform: not driven

    check(set_parent(world, child, root, /*keep_world_pose=*/false), "a child is parented");
    check(set_parent(world, grandchild, child, false), "and a grandchild under it");
    // Spawn order deliberately puts no constraint on processing order: propagation sorts by depth.
    const PropagationStats stats = propagate_transforms(world);
    check(stats.roots == 1 && stats.children == 2 && stats.orphans == 0 && stats.cycles == 0, "propagation sees one root and two children");
    // Root turned 90 deg about +Y: its local -Z points along world -X.
    check(near(position(world, child), {8, 0, 0}), "the child sits 2 m along the root's forward axis");
    check(near(position(world, grandchild), {8, 0, -1}), "the grandchild composes both levels (its local +X is world -Z)");
    check(near(position(world, authored), {0, 5, 0}), "an entity without Transform keeps its authored WorldTransform");

    // Moving the root moves the subtree.
    world.get_component<Transform>(root)->translation = {0, 0, 0};
    propagate_transforms(world);
    check(near(position(world, grandchild), {-2, 0, -1}), "moving a parent moves every descendant");

    // Reparent keeping the world pose: nothing moves.
    const glm::vec3 before = position(world, grandchild);
    check(set_parent(world, grandchild, authored, /*keep_world_pose=*/true), "reparenting under an authored-only entity is allowed");
    propagate_transforms(world);
    check(near(position(world, grandchild), before), "keep_world_pose leaves the world position unchanged");
    check(parent_of(world, grandchild) == authored && children_of(world, authored).size() == 1, "the new link is visible");

    // Loops are refused.
    check(!set_parent(world, root, child), "parenting the root under its own child is refused");
    check(!set_parent(world, root, root), "an entity cannot parent itself");

    // Detaching keeps the world pose by default.
    check(clear_parent(world, grandchild), "a parent link can be cleared");
    propagate_transforms(world);
    check(near(position(world, grandchild), before) && !parent_of(world, grandchild), "and the entity stays put");

    // A destroyed parent orphans its children (they keep their local transform relative to the world).
    world.destroy(root);
    const PropagationStats orphan_stats = propagate_transforms(world);
    check(orphan_stats.orphans == 1 && !world.get_component<Parent>(child), "a child of a destroyed parent is released");
    check(near(position(world, child), {0, 0, -2}), "and is placed relative to the world");

    // Recursive destruction.
    const Ecs::Entity a = world.spawn(Transform{}, WorldTransform{});
    const Ecs::Entity b = world.spawn(Transform{}, WorldTransform{});
    const Ecs::Entity c = world.spawn(Transform{}, WorldTransform{});
    set_parent(world, b, a);
    set_parent(world, c, b);
    destroy_recursive(world, a);
    check(!world.is_alive(a) && !world.is_alive(b) && !world.is_alive(c), "destroy_recursive removes the whole subtree");
    check(world.is_alive(child) && world.is_alive(grandchild), "and nothing else");

    // compute_world_matrix answers immediately, without a propagation pass.
    world.get_component<Transform>(child)->translation = {3, 3, 3};
    check(near(glm::vec3{compute_world_matrix(world, child)[3]}, {3, 3, 3}), "compute_world_matrix reflects the latest Transform");
    return failures == 0 ? 0 : 1;
}
