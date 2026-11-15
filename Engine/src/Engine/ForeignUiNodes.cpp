#include <Engine/ForeignUiNodes.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace SFT::Engine {

    namespace {

        [[nodiscard]] std::unexpected<ForeignUiError> invalid(std::string message) {
            return std::unexpected(ForeignUiError{.kind = ForeignUiErrorKind::InvalidArgument, .message = std::move(message)});
        }

        [[nodiscard]] ForeignUiResult<UiNode> to_node(const ForeignUiNode &node) {
            auto decl = make_foreign_ui_decl(node.element);
            if (!decl) {
                return std::unexpected(decl.error());
            }
            UiNode out;
            out.decl = std::move(*decl);
            out.text = UString{std::string_view{node.text}};
            auto text_style = make_foreign_ui_text_style(node.text_style);
            if (!text_style) {
                return std::unexpected(text_style.error());
            }
            out.text_style = *text_style;
            out.order = node.order;
            out.visible = node.visible;
            return out;
        }

        /// `root` and everything under it, by walking a snapshot of the links (no query is nested inside another).
        void collect(Ecs::World &world, Ecs::Entity root, std::vector<Ecs::Entity> &out) {
            std::vector<std::pair<Ecs::Entity, Ecs::Entity>> links; // child, parent
            for (const auto &[entity, parent] : world.query<const UiParent>()) {
                links.emplace_back(entity, parent.entity);
            }
            out.push_back(root);
            for (usize i = 0; i < out.size(); ++i) {
                for (const auto &[child, parent] : links) {
                    if (parent == out[i] && std::ranges::find(out, child) == out.end()) {
                        out.push_back(child);
                    }
                }
            }
        }

    } // namespace

    ForeignUiResult<Ecs::Entity> spawn_ui_node(Engine &engine, UiNode node, Ecs::Entity parent) {
        Ecs::World &world = engine.ecs_world();
        if (parent && !world.is_alive(parent)) {
            return invalid("the parent node does not exist");
        }
        const Ecs::Entity entity = world.spawn(std::move(node), UiNodeState{});
        if (parent) {
            world.add_component(entity, UiParent{parent});
        }
        return entity;
    }

    ForeignUiResult<void> update_ui_node(Engine &engine, Ecs::Entity entity, UiNode node, Ecs::Entity parent) {
        Ecs::World &world = engine.ecs_world();
        const auto existing = world.get_component<UiNode>(entity);
        if (!existing) {
            return invalid("the entity is not a UI node");
        }
        if (parent) {
            if (!world.is_alive(parent) || parent == entity) {
                return invalid("the parent node does not exist or is the node itself");
            }
            // The new parent must not already sit under this node.
            for (Ecs::Entity cursor = parent; cursor;) {
                if (cursor == entity) {
                    return invalid("that parent would make a loop");
                }
                const auto up = world.get_component<UiParent>(cursor);
                cursor = up ? up->entity : Ecs::Entity{};
            }
        }
        *existing = std::move(node);
        if (parent) {
            if (auto link = world.get_component<UiParent>(entity)) {
                link->entity = parent;
            } else {
                world.add_component(entity, UiParent{parent});
            }
        } else if (world.get_component<UiParent>(entity)) {
            world.remove_component<UiParent>(entity);
        }
        return {};
    }

    ForeignUiResult<Ecs::Entity> spawn_foreign_ui_node(Engine &engine, const ForeignUiNode &node) {
        auto built = to_node(node);
        if (!built) {
            return std::unexpected(built.error());
        }
        return spawn_ui_node(engine, std::move(*built), node.parent);
    }

    ForeignUiResult<void> update_foreign_ui_node(Engine &engine, Ecs::Entity entity, const ForeignUiNode &node) {
        auto built = to_node(node);
        if (!built) {
            return std::unexpected(built.error());
        }
        return update_ui_node(engine, entity, std::move(*built), node.parent);
    }

    ForeignUiResult<void> destroy_foreign_ui_node(Engine &engine, Ecs::Entity entity, bool recursive) {
        Ecs::World &world = engine.ecs_world();
        if (!world.get_component<UiNode>(entity)) {
            return invalid("the entity is not a UI node");
        }
        std::vector<Ecs::Entity> doomed;
        if (recursive) {
            collect(world, entity, doomed);
        } else {
            doomed.push_back(entity);
            // Orphaned children become roots.
            std::vector<Ecs::Entity> children;
            for (const auto &[child, parent] : world.query<const UiParent>()) {
                if (parent.entity == entity) {
                    children.push_back(child);
                }
            }
            for (const Ecs::Entity child : children) {
                world.remove_component<UiParent>(child);
            }
        }
        for (const Ecs::Entity victim : doomed) {
            world.destroy(victim);
        }
        return {};
    }

    ForeignUiResult<ForeignUiNodeState> foreign_ui_node_state(Engine &engine, Ecs::Entity entity) {
        const auto state = engine.ecs_world().get_component<UiNodeState>(entity);
        if (!state) {
            return invalid("the entity is not a UI node");
        }
        return ForeignUiNodeState{.hovered = state->hovered, .clicked = state->clicked};
    }

} // namespace SFT::Engine
