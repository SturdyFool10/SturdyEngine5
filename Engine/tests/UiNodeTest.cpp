/// Retained UI as entities: `UiNode` entities linked by `UiParent` are laid out by a `UiTree` built with `ui_node_builder`; editing the
/// entities changes the UI, and hover/click come back as `UiNodeState`. CPU only.

#include <Async/Scheduler.hpp>
#include <Engine/EngineModule.hpp>
#include <Engine/UiNodes.hpp>

#include <string>

namespace {
    using namespace SFT;

    int failures = 0;
    void check(bool condition, const char *message) {
        if (!condition) {
            Foundation::log_error("FAILED: {}", message);
            ++failures;
        }
    }

    Engine::UiNode box(f32 size, UString id = {}) {
        Engine::UiNode node;
        node.decl = UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(size), UI::SizingAxis::fixed(size)},
                                    .background_color = UI::Color{0.2, 0.4, 0.8, 1.0},
                                    .id = std::move(id)};
        return node;
    }
} // namespace

int main() {
    Engine::Engine engine;
    Ecs::World &world = engine.ecs_world();

    // root -> [a (order 2), b (order 1)], b -> c
    Engine::UiNode root_node;
    root_node.decl = UI::ElementDecl{.direction = UI::LayoutDirection::LeftToRight};
    const Ecs::Entity root = world.spawn(std::move(root_node));
    const Ecs::Entity a = world.spawn(box(40.0f, UString{"a"}), Engine::UiParent{root}, Engine::UiNodeState{});
    Engine::UiNode b_node = box(60.0f, UString{"b"});
    b_node.order = 1;
    const Ecs::Entity b = world.spawn(std::move(b_node), Engine::UiParent{root}, Engine::UiNodeState{});
    const Ecs::Entity c = world.spawn(box(10.0f, UString{"c"}), Engine::UiParent{b}, Engine::UiNodeState{});
    world.get_component<Engine::UiNode>(a)->order = 2;

    engine.spawn_ui_tree({.name = "nodes",
                          .target = Engine::UiHeadlessTarget{{400u, 300u}},
                          .builder = Engine::ui_node_builder(root)});
    engine.update(1.0 / 60.0);
    engine.update(1.0 / 60.0);

    // Layout: b (order 1) first at x = 0, then a at x = 60.
    const UI::Context *context = nullptr;
    (void)context;
    check(world.get_component<Engine::UiNodeState>(a)->hovered == false, "no pointer, nothing hovered");

    // Count the nodes a direct build draws, and check order, visibility and loop handling.
    {
        auto created = UI::Context::create(UI::Context::Config{});
        check(static_cast<bool>(created), "a headless context");
        UI::Context &ui = *created;
        ui.begin_layout(glm::vec2{400.0f, 300.0f}, UI::PointerState{}, 1.0f / 60.0f);
        check(Engine::build_ui_nodes(ui, world, root) == 4, "all four nodes are drawn");
        (void)ui.finish_frame();

        world.get_component<Engine::UiNode>(b)->visible = false;
        ui.begin_layout(glm::vec2{400.0f, 300.0f}, UI::PointerState{}, 1.0f / 60.0f);
        check(Engine::build_ui_nodes(ui, world, root) == 2, "hiding a node hides its subtree");
        (void)ui.finish_frame();
        world.get_component<Engine::UiNode>(b)->visible = true;

        // A parent loop (root made a child of c) is cut, not followed forever.
        world.add_component(root, Engine::UiParent{c});
        ui.begin_layout(glm::vec2{400.0f, 300.0f}, UI::PointerState{}, 1.0f / 60.0f);
        check(Engine::build_ui_nodes(ui, world, root) <= 4, "a parent loop does not recurse forever");
        (void)ui.finish_frame();
        world.remove_component<Engine::UiParent>(root);

        // Pointer over `a` (x 60..100 at the top-left) hovers it, and a click comes back as state.
        UI::PointerState pointer;
        pointer.position = glm::vec2{80.0f, 20.0f};
        ui.begin_layout(glm::vec2{400.0f, 300.0f}, pointer, 1.0f / 60.0f);
        (void)Engine::build_ui_nodes(ui, world, root);
        (void)ui.finish_frame();
        ui.begin_layout(glm::vec2{400.0f, 300.0f}, pointer, 1.0f / 60.0f);
        (void)Engine::build_ui_nodes(ui, world, root);
        check(world.get_component<Engine::UiNodeState>(a)->hovered, "a node under the pointer is hovered");
        check(!world.get_component<Engine::UiNodeState>(c)->hovered, "a node away from the pointer is not");
        (void)ui.finish_frame();
        ui.destroy();
    }

    // A mirror tree lays out only when a node changed.
    {
        Engine::UiTreeDesc desc = Engine::ui_node_tree_desc(engine, root, "mirror");
        desc.target = Engine::UiHeadlessTarget{{400u, 300u}};
        const Ecs::Entity mirror = engine.spawn_ui_tree(std::move(desc));
        const auto builds = [&] { return world.get_component<Engine::UiTree>(mirror)->stats().builds; };
        for (int k = 0; k < 3; ++k) {
            engine.update(1.0 / 60.0);
        }
        check(builds() == 1, "an unchanged node tree is laid out once");
        world.get_component<Engine::UiNode>(c)->decl.background_color = UI::Color{1.0, 0.0, 0.0, 1.0};
        engine.update(1.0 / 60.0);
        check(builds() == 2, "editing a node's colour lays the tree out again");
        engine.update(1.0 / 60.0);
        check(builds() == 2, "and only once");
        const Ecs::Entity extra = world.spawn(box(8.0f), Engine::UiParent{b});
        engine.update(1.0 / 60.0);
        check(builds() == 3, "adding a node lays the tree out again");
        Engine::touch_ui_nodes();
        engine.update(1.0 / 60.0);
        check(builds() == 4, "touch_ui_nodes lays the tree out again");
        world.destroy(extra);
        engine.update(1.0 / 60.0);
        check(builds() == 5, "removing a node lays the tree out again");
        world.destroy(mirror);
        engine.update(1.0 / 60.0);
    }

    // Destroying the entities is how a node goes away.
    world.destroy(a);
    engine.update(1.0 / 60.0);

    Async::Scheduler::shutdown();
    if (failures != 0) {
        Foundation::log_error("{} check(s) failed", failures);
        return 1;
    }
    Foundation::log_info("UiNodeTest passed");
    return 0;
}
