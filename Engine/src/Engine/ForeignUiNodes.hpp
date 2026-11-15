#pragma once

#include <string>

#include <Engine/ForeignUi.hpp>
#include <Engine/UiNodes.hpp>

namespace SFT::Engine {

    /// `UiNode` entities (retained UI on the ECS, `UiNodes.hpp`) for code that cannot write components itself. A node is an entity; these
    /// calls create, edit and remove them. Like every world change they belong between updates (a game-logic callback, a system, a
    /// deferred task), not inside a UI builder.
    struct ForeignUiNode {
        ForeignUiElement element{};
        /// Drawn inside the element after its child nodes; empty for none.
        std::string text;
        ForeignUiTextStyle text_style{};
        i32 order = 0;
        bool visible = true;
        /// The parent node, or an invalid entity for a root.
        Ecs::Entity parent{};
    };

    struct ForeignUiNodeState {
        bool hovered = false;
        bool clicked = false;
    };

    /// The same operations for a ready-made `UiNode` (what a binding with a richer element description builds itself). `parent` invalid = root.
    [[nodiscard]] ForeignUiResult<Ecs::Entity> spawn_ui_node(Engine &engine, UiNode node, Ecs::Entity parent);
    [[nodiscard]] ForeignUiResult<void> update_ui_node(Engine &engine, Ecs::Entity entity, UiNode node, Ecs::Entity parent);

    /// Spawns a node entity (with a `UiNodeState`).
    [[nodiscard]] ForeignUiResult<Ecs::Entity> spawn_foreign_ui_node(Engine &engine, const ForeignUiNode &node);
    /// Replaces the node's data and parent.
    [[nodiscard]] ForeignUiResult<void> update_foreign_ui_node(Engine &engine, Ecs::Entity entity, const ForeignUiNode &node);
    /// Destroys the node; with `recursive` also everything under it (otherwise its children become roots).
    [[nodiscard]] ForeignUiResult<void> destroy_foreign_ui_node(Engine &engine, Ecs::Entity entity, bool recursive);
    [[nodiscard]] ForeignUiResult<ForeignUiNodeState> foreign_ui_node_state(Engine &engine, Ecs::Entity entity);

} // namespace SFT::Engine
