#pragma once

#include <Foundation/Foundation.hpp>

#include <Ecs/Component.hpp>
#include <Ecs/Entity.hpp>
#include <Ecs/World.hpp>
#include <Engine/UiTree.hpp>
#include <Renderer/UI/UI.hpp>

namespace SFT::Engine {

    /// Retained UI as entities: an element is an entity with a `UiNode`, its place in the tree is a `UiParent` link, and a `UiTree`
    /// whose builder is `build_ui_nodes` lays the whole thing out each time it is due. Because the UI is ordinary component data, any
    /// system can create, edit, reparent or destroy nodes (a quest log entry, an inventory slot), queries see them like any other
    /// entity, and `UiNodeState` gives every node with an id its hover and click state back as a component to query.
    ///
    /// ```cpp
    /// const Ecs::Entity root = world.spawn(Engine::UiNode{.decl = {.sizing = {grow, grow}, .direction = UI::LayoutDirection::TopToBottom}});
    /// const Ecs::Entity title = world.spawn(Engine::UiNode{.text = "Inventory"_ustr}, Engine::UiParent{root});
    /// engine.spawn_ui_tree({.name = "inventory", .builder = Engine::ui_node_builder(root)});
    /// ```
    struct UiNode {
        /// The element: sizing, padding, colours, corner radii and shapes, effect, layer, and so on. A node with an `id` gets a `UiNodeState`.
        UI::ElementDecl decl{};
        /// Text drawn inside the element, after any child nodes. Empty for none.
        UString text{};
        UI::TextStyle text_style{};
        /// Siblings are laid out in ascending `order`, ties in entity order.
        i32 order = 0;
        /// A hidden node (and everything under it) is skipped.
        bool visible = true;
    };

    /// The node's parent. A node with no `UiParent` (or whose parent is gone) is a root.
    struct UiParent {
        Ecs::Entity entity{};
    };

    /// Written back by `build_ui_nodes` for every visible node whose `decl.id` is set, from what the last layout found.
    struct UiNodeState {
        bool hovered = false;
        bool clicked = false;
    };

    /// Lays out `root` and every visible node under it into `ui`. Call it from a `UiTree` builder (`ui_node_builder` makes one). Reads
    /// the world and writes `UiNodeState`; it never adds or removes entities. A parent loop is cut, not followed. Returns how many
    /// nodes were drawn.
    usize build_ui_nodes(UI::Context &ui, Ecs::World &world, Ecs::Entity root);

    /// A `UiTreeBuilder` that draws the nodes under `root`.
    [[nodiscard]] UiTreeBuilder ui_node_builder(Ecs::Entity root);

    /// A fingerprint of everything under `root` that affects the layout: which nodes exist and how they are linked and ordered, their
    /// visibility and text, and the common `decl` fields (sizing, padding, gap, direction, colours, corners, z, id, effect). Changes
    /// when any of that changes; equal otherwise. Fields it does not read (borders, clipping, floating, alignment, cursor) are covered by
    /// `touch_ui_nodes`.
    [[nodiscard]] u64 ui_node_signature(Ecs::World &world, Ecs::Entity root);

    /// Tells every node tree that something it cannot see changed (a field `ui_node_signature` does not read). Any thread.
    void touch_ui_nodes() noexcept;

    /// A tree description that mirrors the nodes under `root`: `ui_node_builder` plus a dirty check on `ui_node_signature`, laid out only
    /// when a node changed (`OnDirty`). Adjust the returned description (target, layer, ...) before spawning.
    [[nodiscard]] UiTreeDesc ui_node_tree_desc(Engine &engine, Ecs::Entity root, std::string name = "ui nodes");

} // namespace SFT::Engine

SFT_ECS_COMPONENT(SFT::Engine::UiNode, "sturdy.engine.ui_node");
SFT_ECS_COMPONENT(SFT::Engine::UiParent, "sturdy.engine.ui_parent");
SFT_ECS_COMPONENT(SFT::Engine::UiNodeState, "sturdy.engine.ui_node_state");
