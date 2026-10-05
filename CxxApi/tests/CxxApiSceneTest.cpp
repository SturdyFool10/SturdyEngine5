// The CxxApi scene surface over a bare ECS world: spawn, local/world placement, parenting, propagation, recursive
// destruction, and the render-settings round trip through a frame's parameters.

#include <CxxApi/Render.hpp>
#include <CxxApi/Scene.hpp>

#include <cmath>
#include <cstdio>

using namespace SFT;
using namespace SFT::CxxApi;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }
    bool near(float a, float b) { return std::abs(a - b) <= 1e-4f; }
    TransformDesc at(float x, float y, float z) {
        TransformDesc d = transform_desc_defaults();
        d.translation[0] = x;
        d.translation[1] = y;
        d.translation[2] = z;
        return d;
    }
} // namespace

int main() {
    Ecs::ComponentRegistry registry;
    Ecs::World world{registry};

    const TransformDesc defaults = transform_desc_defaults();
    check(defaults.rotation[3] == 1.0f && defaults.scale[0] == 1.0f, "default placement is the identity");

    const Entity car = scene_spawn(world, at(0.0f, 0.0f, -10.0f));
    const Entity wheel = scene_spawn(world, at(1.0f, 0.0f, 0.0f));
    check(car.generation != 0 && scene_is_alive(world, car), "spawn returns a live entity");

    // Keep the world pose: the wheel stays at (1, 0, 0) and its local placement becomes relative to the car.
    check(scene_set_parent(world, wheel, car, true), "parenting succeeds");
    TransformDesc local{};
    check(scene_transform(world, wheel, local) && near(local.translation[2], 10.0f), "local placement rewritten relative to the parent");
    check(scene_parent(world, wheel).index == car.index, "the parent reads back");
    check(scene_children(world, car)->size() == 1, "the car has one child");
    check(!scene_set_parent(world, car, wheel, true), "a loop is refused");

    // Move the car; the wheel follows once propagated (and immediately through scene_world_matrix).
    check(scene_set_transform(world, car, at(5.0f, 0.0f, -10.0f)), "moving the car");
    const auto wheel_world = scene_world_matrix(world, wheel);
    check(near(wheel_world[12], 6.0f) && near(wheel_world[14], 0.0f), "the wheel's world matrix follows the car");
    const TransformPropagationStats stats = scene_propagate_transforms(world);
    check(stats.roots == 1 && stats.children == 1, "one root, one child propagated");

    check(scene_clear_parent(world, wheel, true), "unparenting");
    check(scene_transform(world, wheel, local) && near(local.translation[0], 6.0f), "unparenting keeps the world pose");
    check(scene_set_parent(world, wheel, car, false), "reparenting without keeping the pose");

    scene_destroy_recursive(world, car);
    check(!scene_is_alive(world, car) && !scene_is_alive(world, wheel), "recursive destruction takes the child");

    // Render settings are the engine's records: defaults round-trip unchanged.
    const FrameSettings settings = frame_settings_defaults();
    check(settings.lighting.clustered && !settings.volumetric_fog.enabled, "render-settings defaults");

    if (failures == 0) {
        std::printf("CxxApiSceneTest: all checks passed.\n");
    }
    return failures == 0 ? 0 : 1;
}
