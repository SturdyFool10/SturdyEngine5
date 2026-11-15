#include <Engine/UiNodes.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Engine/EngineModule.hpp>

namespace SFT::Engine {

    namespace {

        struct EntityKey {
            usize operator()(Ecs::Entity entity) const noexcept {
                return (static_cast<usize>(entity.generation) << 32) ^ static_cast<usize>(entity.index);
            }
        };

        struct Child {
            Ecs::Entity entity{};
            i32 order = 0;
        };

        using Children = std::unordered_map<Ecs::Entity, std::vector<Child>, EntityKey>;

        void draw(UI::Context &ui, Ecs::World &world, const Children &children, Ecs::Entity entity,
                  std::unordered_set<u64> &on_path, usize &drawn) {
            const u64 key = (static_cast<u64>(entity.generation) << 32) | entity.index;
            if (!on_path.insert(key).second) {
                return; // a parent loop: cut it here
            }
            const auto node = world.get_component<UiNode>(entity);
            if (node && node->visible) {
                ++drawn;
                {
                    auto element = ui.element(node->decl);
                    if (const auto found = children.find(entity); found != children.end()) {
                        for (const Child &child : found->second) {
                            draw(ui, world, children, child.entity, on_path, drawn);
                        }
                    }
                    if (!node->text.empty()) {
                        ui.text(node->text, node->text_style);
                    }
                }
                // After the element closed, so the answer is about this frame's layout of it, like the rest of the immediate-mode queries.
                if (!node->decl.id.empty()) {
                    if (auto state = world.get_component<UiNodeState>(entity)) {
                        state->hovered = ui.hovered(node->decl.id);
                        state->clicked = ui.clicked(node->decl.id);
                    }
                }
            }
            on_path.erase(key);
        }

    } // namespace

    usize build_ui_nodes(UI::Context &ui, Ecs::World &world, Ecs::Entity root) {
        if (!world.is_alive(root)) {
            return 0;
        }
        // One pass over the nodes builds the adjacency, so the layout costs O(nodes) however deep the tree is.
        Children children;
        for (const auto &[entity, node] : world.query<const UiNode>()) {
            const auto parent = world.get_component<UiParent>(entity);
            if (parent && parent->entity && world.is_alive(parent->entity)) {
                children[parent->entity].push_back(Child{.entity = entity, .order = node.order});
            }
        }
        for (auto &[parent, list] : children) {
            std::ranges::sort(list, [](const Child &a, const Child &b) {
                return a.order != b.order ? a.order < b.order : a.entity.index < b.entity.index;
            });
        }
        std::unordered_set<u64> on_path;
        usize drawn = 0;
        draw(ui, world, children, root, on_path, drawn);
        return drawn;
    }

    namespace {
        std::atomic<u64> g_touch{0};

        void mix(u64 &h, u64 value) noexcept {
            h ^= value + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        }
        template <class T>
        void mix_bits(u64 &h, const T &value) noexcept {
            static_assert(std::is_trivially_copyable_v<T>);
            u64 bits = 0;
            std::memcpy(&bits, &value, std::min(sizeof(T), sizeof(bits)));
            mix(h, bits);
        }
        void mix_string(u64 &h, const UString &text) noexcept { mix(h, std::hash<std::string_view>{}(text.cpp_string_view())); }

        void hash_node(u64 &h, Ecs::Entity entity, const UiNode &node) {
            mix(h, (static_cast<u64>(entity.generation) << 32) | entity.index);
            mix(h, static_cast<u64>(static_cast<u32>(node.order)));
            mix(h, node.visible ? 1u : 0u);
            mix_string(h, node.text);
            const UI::ElementDecl &d = node.decl;
            for (const UI::SizingAxis &axis : {d.sizing.width, d.sizing.height}) {
                mix(h, static_cast<u64>(axis.kind));
                mix_bits(h, axis.min);
                mix_bits(h, axis.max);
                mix_bits(h, axis.value);
            }
            mix(h, (static_cast<u64>(d.padding.left) << 48) | (static_cast<u64>(d.padding.right) << 32) |
                       (static_cast<u64>(d.padding.top) << 16) | d.padding.bottom);
            mix(h, d.child_gap);
            mix(h, static_cast<u64>(d.direction));
            mix_bits(h, d.background_color.r);
            mix_bits(h, d.background_color.g);
            mix_bits(h, d.background_color.b);
            mix_bits(h, d.background_color.a);
            mix_bits(h, d.corner_radius);
            mix_bits(h, d.corner_shapes);
            mix(h, static_cast<u64>(static_cast<u32>(d.z)));
            mix_string(h, d.id);
            mix(h, reinterpret_cast<std::uintptr_t>(d.effect.get()));
        }

        void walk_signature(Ecs::World &world, const Children &children, Ecs::Entity entity, std::unordered_set<u64> &on_path, u64 &h) {
            const u64 key = (static_cast<u64>(entity.generation) << 32) | entity.index;
            if (!on_path.insert(key).second) {
                return;
            }
            if (const auto node = world.get_component<UiNode>(entity)) {
                hash_node(h, entity, *node);
                if (const auto found = children.find(entity); found != children.end()) {
                    for (const Child &child : found->second) {
                        mix(h, 0xC0FFEEull);
                        walk_signature(world, children, child.entity, on_path, h);
                    }
                }
            }
            on_path.erase(key);
        }
    } // namespace

    void touch_ui_nodes() noexcept { g_touch.fetch_add(1, std::memory_order_relaxed); }

    u64 ui_node_signature(Ecs::World &world, Ecs::Entity root) {
        u64 h = g_touch.load(std::memory_order_relaxed);
        if (!world.is_alive(root)) {
            return h;
        }
        Children children;
        for (const auto &[entity, node] : world.query<const UiNode>()) {
            const auto parent = world.get_component<UiParent>(entity);
            if (parent && parent->entity && world.is_alive(parent->entity)) {
                children[parent->entity].push_back(Child{.entity = entity, .order = node.order});
            }
        }
        for (auto &[parent, list] : children) {
            std::ranges::sort(list, [](const Child &a, const Child &b) {
                return a.order != b.order ? a.order < b.order : a.entity.index < b.entity.index;
            });
        }
        std::unordered_set<u64> on_path;
        walk_signature(world, children, root, on_path, h);
        return h;
    }

    UiTreeDesc ui_node_tree_desc(Engine &engine, Ecs::Entity root, std::string name) {
        UiTreeDesc desc;
        desc.name = std::move(name);
        desc.builder = ui_node_builder(root);
        desc.rebuild = UiRebuildPolicy::OnDirty;
        // The first check always reports a change, so the tree lays out once; after that only when the fingerprint moves.
        auto last = std::make_shared<std::optional<u64>>();
        desc.dirty_check = [engine_ptr = &engine, root, last]() {
            const u64 now = ui_node_signature(engine_ptr->ecs_world(), root);
            if (*last && **last == now) {
                return false;
            }
            *last = now;
            return true;
        };
        return desc;
    }

    UiTreeBuilder ui_node_builder(Ecs::Entity root) {
        return [root](UiTreeFrame &frame) { (void)build_ui_nodes(frame.ui, frame.engine.ecs_world(), root); };
    }

} // namespace SFT::Engine
