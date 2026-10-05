#include <Engine/Transform.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace SFT::Engine {

    namespace {
        [[nodiscard]] u64 key(Ecs::Entity entity) noexcept { return (static_cast<u64>(entity.index) << 32) | entity.generation; }

        /// The longest parent chain followed before a link is declared part of a loop.
        constexpr u32 kMaxDepth = 4096;
    } // namespace

    glm::mat4 Transform::matrix() const noexcept {
        glm::mat4 m = glm::mat4_cast(glm::normalize(rotation));
        m[0] *= scale.x;
        m[1] *= scale.y;
        m[2] *= scale.z;
        m[3] = glm::vec4{translation, 1.0f};
        return m;
    }

    Transform Transform::from_matrix(const glm::mat4 &matrix) noexcept {
        Transform out;
        out.translation = glm::vec3{matrix[3]};
        glm::vec3 axes[3] = {glm::vec3{matrix[0]}, glm::vec3{matrix[1]}, glm::vec3{matrix[2]}};
        for (u32 i = 0; i < 3; ++i) {
            out.scale[i] = glm::length(axes[i]);
            axes[i] = out.scale[i] > 1e-12f ? axes[i] / out.scale[i] : glm::vec3{i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f};
        }
        // A mirrored basis cannot be a rotation: move the reflection into the scale.
        if (glm::dot(glm::cross(axes[0], axes[1]), axes[2]) < 0.0f) {
            out.scale.x = -out.scale.x;
            axes[0] = -axes[0];
        }
        out.rotation = glm::normalize(glm::quat_cast(glm::mat3{axes[0], axes[1], axes[2]}));
        return out;
    }

    Transform Transform::looking_at(const glm::vec3 &position, const glm::vec3 &target, const glm::vec3 &up) noexcept {
        Transform out;
        out.translation = position;
        const glm::vec3 direction = target - position;
        if (glm::dot(direction, direction) > 1e-12f) {
            // glm::lookAt builds a view matrix (world -> camera); its inverse is the object's orientation.
            const glm::mat4 view = glm::lookAt(position, target, glm::abs(glm::dot(glm::normalize(direction), glm::normalize(up))) > 0.999f
                                                                     ? glm::vec3{0.0f, 0.0f, 1.0f}
                                                                     : up);
            out.rotation = glm::normalize(glm::quat_cast(glm::inverse(glm::mat3{view})));
        }
        return out;
    }

    PropagationStats propagate_transforms(Ecs::World &world) {
        PropagationStats stats;
        std::unordered_map<u64, Ecs::Entity> parent_of_child;
        {
            auto parents = world.query<const Parent>();
            for (const auto &[child, parent] : parents) {
                parent_of_child.emplace(key(child), parent.entity);
            }
        }

        struct Pending {
            Ecs::Entity entity;
            Ecs::Entity parent;
            glm::mat4 local;
            u32 depth = 0;
        };
        std::vector<Pending> pending;
        {
            auto locals = world.query<const Transform, WorldTransform>();
            for (const auto &[entity, transform, out] : locals) {
                const auto parent = parent_of_child.find(key(entity));
                if (parent == parent_of_child.end()) {
                    out.value = transform.matrix();
                    ++stats.roots;
                } else {
                    pending.push_back(Pending{entity, parent->second, transform.matrix(), 0});
                }
            }
        }
        if (pending.empty()) {
            return stats;
        }

        // Parents first: order children by how many parented ancestors they have.
        std::vector<Ecs::Entity> orphaned;
        std::unordered_map<u64, u32> depth_cache;
        for (Pending &item : pending) {
            u32 depth = 0;
            Ecs::Entity cursor = item.parent;
            std::unordered_set<u64> seen{key(item.entity)};
            while (true) {
                if (const auto cached = depth_cache.find(key(cursor)); cached != depth_cache.end()) {
                    depth += cached->second + 1;
                    break;
                }
                const auto next = parent_of_child.find(key(cursor));
                if (next == parent_of_child.end()) {
                    break;
                }
                if (!seen.insert(key(cursor)).second || depth > kMaxDepth) {
                    item.depth = ~0u; // a loop
                    break;
                }
                ++depth;
                cursor = next->second;
            }
            if (item.depth != ~0u) {
                item.depth = depth;
                depth_cache.emplace(key(item.entity), depth);
            }
        }
        std::ranges::stable_sort(pending, {}, &Pending::depth);

        std::unordered_map<u64, glm::mat4> computed;
        computed.reserve(pending.size());
        for (const Pending &item : pending) {
            glm::mat4 parent_world{1.0f};
            if (item.depth == ~0u) {
                ++stats.cycles;
            } else if (const auto done = computed.find(key(item.parent)); done != computed.end()) {
                parent_world = done->second;
            } else if (!world.is_alive(item.parent)) {
                orphaned.push_back(item.entity);
                ++stats.orphans;
            } else if (auto parent_transform = world.get_component<WorldTransform>(item.parent)) {
                parent_world = parent_transform->value;
            }
            const glm::mat4 value = parent_world * item.local;
            computed.emplace(key(item.entity), value);
            if (auto out = world.get_component<WorldTransform>(item.entity)) {
                out->value = value;
            }
            ++stats.children;
        }
        for (const Ecs::Entity entity : orphaned) {
            world.remove_component<Parent>(entity);
        }
        return stats;
    }

    glm::mat4 compute_world_matrix(Ecs::World &world, Ecs::Entity entity) {
        glm::mat4 result{1.0f};
        Ecs::Entity cursor = entity;
        for (u32 depth = 0; depth < kMaxDepth && world.is_alive(cursor); ++depth) {
            glm::mat4 local{1.0f};
            bool has_local = false;
            if (auto transform = world.get_component<Transform>(cursor)) {
                local = transform->matrix();
                has_local = true;
            }
            Ecs::Entity parent{};
            if (auto link = world.get_component<Parent>(cursor)) {
                parent = link->entity;
            }
            if (!has_local) {
                // Not driven by the hierarchy: its WorldTransform is authoritative and ends the chain.
                if (auto authored = world.get_component<WorldTransform>(cursor)) {
                    return authored->value * result;
                }
                return result;
            }
            result = local * result;
            if (!parent || !world.is_alive(parent)) {
                return result;
            }
            cursor = parent;
        }
        return result;
    }

    Ecs::Entity parent_of(Ecs::World &world, Ecs::Entity child) {
        if (auto link = world.get_component<Parent>(child)) {
            return link->entity;
        }
        return {};
    }

    std::vector<Ecs::Entity> children_of(Ecs::World &world, Ecs::Entity parent) {
        std::vector<Ecs::Entity> children;
        auto links = world.query<const Parent>();
        for (const auto &[child, link] : links) {
            if (link.entity == parent) {
                children.push_back(child);
            }
        }
        return children;
    }

    bool set_parent(Ecs::World &world, Ecs::Entity child, Ecs::Entity parent, bool keep_world_pose) {
        if (!world.is_alive(child) || !world.is_alive(parent) || child == parent) {
            return false;
        }
        // Refuse a loop: the new parent must not already descend from the child.
        for (Ecs::Entity cursor = parent; cursor;) {
            if (cursor == child) {
                return false;
            }
            const Ecs::Entity next = parent_of(world, cursor);
            if (next == cursor) {
                break;
            }
            cursor = next;
        }
        const glm::mat4 child_world = compute_world_matrix(world, child);
        const glm::mat4 parent_world = compute_world_matrix(world, parent);
        bool has_transform = false, has_world = false, has_parent = false;
        Transform local{};
        if (auto existing = world.get_component<Transform>(child)) {
            has_transform = true;
            local = *existing;
        }
        if (world.get_component<WorldTransform>(child)) has_world = true;
        if (world.get_component<Parent>(child)) has_parent = true;
        if (keep_world_pose) {
            local = Transform::from_matrix(glm::inverse(parent_world) * child_world);
        } else if (!has_transform) {
            // No local placement yet: its current world matrix becomes the placement relative to the parent.
            local = Transform::from_matrix(child_world);
        }
        if (has_transform) {
            if (auto existing = world.get_component<Transform>(child)) *existing = local;
        } else {
            world.add_component(child, local);
        }
        if (!has_world) {
            world.add_component(child, WorldTransform{parent_world * local.matrix()});
        } else if (auto out = world.get_component<WorldTransform>(child)) {
            out->value = parent_world * local.matrix();
        }
        if (has_parent) {
            if (auto link = world.get_component<Parent>(child)) link->entity = parent;
        } else {
            world.add_component(child, Parent{parent});
        }
        return true;
    }

    bool clear_parent(Ecs::World &world, Ecs::Entity child, bool keep_world_pose) {
        if (!world.is_alive(child) || !world.get_component<Parent>(child)) {
            return false;
        }
        const glm::mat4 child_world = compute_world_matrix(world, child);
        world.remove_component<Parent>(child);
        if (keep_world_pose) {
            if (auto transform = world.get_component<Transform>(child)) {
                *transform = Transform::from_matrix(child_world);
            }
        }
        if (auto out = world.get_component<WorldTransform>(child)) {
            out->value = keep_world_pose ? child_world : out->value;
        }
        return true;
    }

    void destroy_recursive(Ecs::World &world, Ecs::Entity root) {
        // Gather the whole subtree first: destroying entities while walking links would invalidate the scan.
        std::unordered_map<u64, std::vector<Ecs::Entity>> children;
        {
            auto links = world.query<const Parent>();
            for (const auto &[child, link] : links) {
                children[key(link.entity)].push_back(child);
            }
        }
        std::vector<Ecs::Entity> doomed{root};
        for (usize i = 0; i < doomed.size(); ++i) {
            if (const auto found = children.find(key(doomed[i])); found != children.end()) {
                doomed.insert(doomed.end(), found->second.begin(), found->second.end());
            }
            if (doomed.size() > (usize{1} << 24)) break; // a corrupted loop; stop rather than run away
        }
        for (auto it = doomed.rbegin(); it != doomed.rend(); ++it) {
            if (world.is_alive(*it)) world.destroy(*it);
        }
    }

} // namespace SFT::Engine
