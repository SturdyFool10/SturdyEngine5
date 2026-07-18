#pragma once

#include <Ecs/Archetype.hpp>
#include <Ecs/Component.hpp>
#include <Ecs/Entity.hpp>
#include <Ecs/Query.hpp>
#include <Ecs/Resource.hpp>
#include <Ecs/Signature.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SFT::Ecs {

    namespace Detail {
        struct WorldAccess;
    }

    // Pointer-like direct component access that keeps its World read-borrow alive. This prevents a
    // Schedule or structural mutation from invalidating the component while the consumer uses it.
    // Deliberately move-only: copying a borrow makes its lifetime and ownership much less obvious.
    template <class T>
    class ComponentBorrow {
      public:
        ComponentBorrow() noexcept = default;
        ComponentBorrow(const ComponentBorrow &) = delete;
        ComponentBorrow &operator=(const ComponentBorrow &) = delete;
        ComponentBorrow(ComponentBorrow &&other) noexcept
            : component_(std::exchange(other.component_, nullptr)), access_(std::move(other.access_)) {}

        ComponentBorrow &operator=(ComponentBorrow &&other) noexcept {
            if (this == &other) {
                return *this;
            }
            component_ = nullptr;
            access_ = std::move(other.access_);
            component_ = std::exchange(other.component_, nullptr);
            return *this;
        }

        [[nodiscard]] explicit operator bool() const noexcept { return component_ != nullptr; }
        [[nodiscard]] bool operator==(std::nullptr_t) const noexcept { return component_ == nullptr; }
        [[nodiscard]] T *operator->() const noexcept { return component_; }
        [[nodiscard]] T &operator*() const noexcept { return *component_; }

        friend bool operator==(std::nullptr_t, const ComponentBorrow &borrow) noexcept {
            return borrow == nullptr;
        }

      private:
        friend class World;

        ComponentBorrow(T *component, std::shared_lock<std::shared_mutex> access) noexcept
            : component_(component), access_(std::move(access)) {}

        T *component_ = nullptr;
        std::shared_lock<std::shared_mutex> access_;
    };

    // Owns every entity and archetype. First-cut scope: no add_component/remove_component yet — see
    // Archetype.hpp's doc comment on why that (archetype-transition) support is deferred rather than
    // shipped half-working. The registry is engine/application-owned and must outlive the World;
    // sharing it across worlds gives every component the same dense ID while stable ComponentKeys
    // remain portable across process/language boundaries.
    class World {
      public:
        explicit World(ComponentRegistry &registry) noexcept : registry_(&registry) {}
        ~World() = default;
        World(const World &) = delete;
        World &operator=(const World &) = delete;
        World(World &&) = delete;
        World &operator=(World &&) = delete;

        // Spawns one entity with exactly the given components — its archetype is fixed at spawn time
        // from the argument types (there's no way to add/remove components afterward yet). `Ts...`
        // deduce to the decayed argument types (pass components by value or as rvalues).
        template <class... Ts>
        [[nodiscard]] Entity spawn(Ts &&...components) {
            ensure_not_scheduled("spawn entities");
            auto access = acquire_direct_mutation("spawn entities");
            ensure_not_scheduled("spawn entities");
            return spawn_unchecked(std::forward<Ts>(components)...);
        }

        void destroy(Entity entity) noexcept {
            ensure_not_scheduled("destroy entities directly; use Commands::destroy() inside a system");
            auto access = acquire_direct_mutation("destroy entities");
            ensure_not_scheduled("destroy entities directly; use Commands::destroy() inside a system");
            destroy_unchecked(entity);
        }

        [[nodiscard]] bool is_alive(Entity entity) const noexcept {
            ensure_not_scheduled("inspect entity liveness directly");
            std::shared_lock access{direct_access_mutex_};
            ensure_not_scheduled("inspect entity liveness directly");
            return is_alive_unchecked(entity);
        }

        template <class T>
        [[nodiscard]] ComponentBorrow<T> get_component(Entity entity) noexcept {
            static_assert(std::is_same_v<T, std::remove_cv_t<T>>, "get_component<T>() expects an unqualified T.");
            ensure_not_scheduled("access components directly; declare access through a scheduled system instead");
            std::shared_lock access{direct_access_mutex_};
            ensure_not_scheduled("access components directly; declare access through a scheduled system instead");
            if (!is_alive_unchecked(entity)) {
                return {};
            }
            const std::optional<ComponentId> component = registry_->find(component_key<T>());
            if (!component) {
                return {};
            }
            EntityRecord &record = entity_records_[entity.index];
            Archetype &archetype = archetypes_[record.archetype_index];
            const u32 column = archetype.column_index_of(*component);
            if (column == ~0u) {
                return {};
            }
            return ComponentBorrow<T>{static_cast<T *>(archetype.row_pointer(column, record.row)), std::move(access)};
        }

        template <class T>
        [[nodiscard]] ComponentBorrow<const T> get_component(Entity entity) const noexcept {
            static_assert(std::is_same_v<T, std::remove_cv_t<T>>, "get_component<T>() expects an unqualified T.");
            ensure_not_scheduled("access components directly; declare access through a scheduled system instead");
            std::shared_lock access{direct_access_mutex_};
            ensure_not_scheduled("access components directly; declare access through a scheduled system instead");
            if (!is_alive_unchecked(entity)) {
                return nullptr;
            }
            const std::optional<ComponentId> component = registry_->find(component_key<T>());
            if (!component) {
                return nullptr;
            }
            const EntityRecord &record = entity_records_[entity.index];
            const Archetype &archetype = archetypes_[record.archetype_index];
            const u32 column = archetype.column_index_of(*component);
            if (column == ~0u) {
                return nullptr;
            }
            return ComponentBorrow<const T>{
                static_cast<const T *>(archetype.row_pointer(column, record.row)),
                std::move(access)};
        }

        // Every archetype whose signature is a superset of {remove_const_t<Ts>...} — matched fresh on
        // each call. Archetype counts are small (a handful to a few dozen distinct component-set
        // combinations, not per-entity), so this linear scan is cheap; caching per-Ts... results is a
        // documented, not-yet-needed optimization (see plans/ecs-design.md).
        template <class... Ts>
        [[nodiscard]] Query<Ts...> query() {
            ensure_not_scheduled("create a direct query; scheduled systems receive safe query chunks automatically");
            std::shared_lock access{direct_access_mutex_};
            ensure_not_scheduled("create a direct query; scheduled systems receive safe query chunks automatically");
            return query_impl<Ts...>(std::move(access));
        }

        [[nodiscard]] ComponentRegistry &registry() noexcept { return *registry_; }
        [[nodiscard]] const ComponentRegistry &registry() const noexcept { return *registry_; }

        // Binds a non-owning singleton resource. The resource must outlive this binding and every
        // Schedule using it. Systems request access explicitly with ReadResource<T> or
        // WriteResource<T>, allowing the scheduler to include resources in conflict analysis.
        template <class T>
        void bind_resource(T &resource) {
            using ResourceT = std::remove_cv_t<T>;
            static_assert(std::is_same_v<T, ResourceT>, "World::bind_resource requires a mutable, unqualified T.");
            ensure_not_scheduled("bind an ECS resource");
            auto access = acquire_direct_mutation("bind an ECS resource");
            ensure_not_scheduled("bind an ECS resource");

            constexpr ResourceKey key = resource_key<ResourceT>();
            constexpr std::string_view name = Detail::resource_name<ResourceT>();
            const ResourceRecord incoming{
                .canonical_name = name,
                .size = sizeof(ResourceT),
                .align = alignof(ResourceT),
                .object = &resource,
            };
            if (auto existing = resources_.find(key); existing != resources_.end()) {
                if (existing->second.canonical_name != incoming.canonical_name ||
                    existing->second.size != incoming.size || existing->second.align != incoming.align) {
                    Detail::contract_violation(
                        "ECS resource key collision: '{}' conflicts with already bound '{}'.",
                        name,
                        existing->second.canonical_name);
                }
                existing->second.object = &resource;
                return;
            }
            resources_.emplace(key, incoming);
        }

        template <class T>
        void unbind_resource() noexcept {
            using ResourceT = std::remove_cv_t<T>;
            ensure_not_scheduled("unbind an ECS resource");
            auto access = acquire_direct_mutation("unbind an ECS resource");
            ensure_not_scheduled("unbind an ECS resource");
            resources_.erase(resource_key<ResourceT>());
        }

        template <class T>
        [[nodiscard]] bool has_resource() const noexcept {
            using ResourceT = std::remove_cv_t<T>;
            ensure_not_scheduled("inspect ECS resources directly");
            std::shared_lock access{direct_access_mutex_};
            ensure_not_scheduled("inspect ECS resources directly");
            return resources_.contains(resource_key<ResourceT>());
        }

      private:
        friend struct Detail::WorldAccess;

        struct EntityRecord {
            u32 generation = 0;
            u32 archetype_index = ~0u;
            u32 row = ~0u;
        };

        struct ResourceRecord {
            std::string_view canonical_name;
            usize size = 0;
            usize align = 0;
            void *object = nullptr;
        };

        template <class... Ts>
        [[nodiscard]] Entity spawn_unchecked(Ts &&...components) {
            static_assert(sizeof...(Ts) > 0, "World::spawn requires at least one component for now.");
            static_assert((std::is_nothrow_constructible_v<std::decay_t<Ts>, Ts &&> && ...),
                          "World::spawn component construction must be noexcept.");

            const std::array<ComponentId, sizeof...(Ts)> component_ids{
                registry_->component<std::decay_t<Ts>>()...};
            Signature signature{component_ids.begin(), component_ids.end()};
            std::sort(signature.begin(), signature.end());
            if (const auto duplicate = std::adjacent_find(signature.begin(), signature.end());
                duplicate != signature.end()) {
                const ComponentInfo *descriptor = registry_->info(*duplicate);
                if (descriptor != nullptr) {
                    Detail::contract_violation(
                        "ECS spawn contains duplicate component '{}'.",
                        descriptor->canonical_name);
                }
                Detail::contract_violation(
                    "ECS spawn contains duplicate dense component ID {}.",
                    *duplicate);
            }
            const u32 archetype_index = archetype_index_for(signature);
            Archetype &archetype = archetypes_[archetype_index];

            const Entity entity = allocate_entity();
            const u32 row = archetype.add_row(entity);
            usize component_index = 0;
            (::new (archetype.row_pointer(archetype.column_index_of(component_ids[component_index++]), row))
                 std::decay_t<Ts>(std::forward<Ts>(components)),
             ...);

            EntityRecord &record = entity_records_[entity.index];
            record.archetype_index = archetype_index;
            record.row = row;
            return entity;
        }

        void ensure_not_scheduled(string_view action) const noexcept {
            if (scheduled_execution_.load(std::memory_order_acquire)) {
                Detail::contract_violation(
                    "Unsafe ECS World access while an async schedule is active: cannot {}.",
                    action);
            }
        }

        [[nodiscard]] std::unique_lock<std::shared_mutex> acquire_direct_mutation(string_view action) noexcept {
            std::unique_lock access{direct_access_mutex_, std::try_to_lock};
            if (!access.owns_lock()) {
                Detail::contract_violation(
                    "Cannot {} while a direct ECS query or component borrow is active.",
                    action);
            }
            return access;
        }

        [[nodiscard]] std::unique_lock<std::shared_mutex> begin_scheduled_execution() noexcept {
            ensure_not_scheduled("start another schedule");
            std::unique_lock access{direct_access_mutex_, std::try_to_lock};
            if (!access.owns_lock()) {
                Detail::contract_violation(
                    "Cannot start an ECS schedule while a direct World operation, query, or component borrow is active.");
            }
            bool expected = false;
            if (!scheduled_execution_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                Detail::contract_violation("The same ECS World cannot run multiple schedules concurrently.");
            }
            return access;
        }

        void end_scheduled_execution() noexcept {
            scheduled_execution_.store(false, std::memory_order_release);
        }

        [[nodiscard]] bool is_alive_unchecked(Entity entity) const noexcept {
            return entity.generation != 0 && entity.index < entity_records_.size() &&
                   entity_records_[entity.index].generation == entity.generation;
        }

        void destroy_unchecked(Entity entity) noexcept {
            if (!is_alive_unchecked(entity)) {
                return;
            }
            EntityRecord &record = entity_records_[entity.index];
            Archetype &archetype = archetypes_[record.archetype_index];
            const Entity moved = archetype.remove_row(record.row);
            if (moved) {
                entity_records_[moved.index].row = record.row;
            }
            ++record.generation;
            free_indices_.push_back(entity.index);
        }

        template <class... Ts>
        [[nodiscard]] Query<Ts...> query_impl(std::shared_lock<std::shared_mutex> direct_access_lock = {}) {
            const auto resolve = [this]<class T>() -> ComponentId {
                auto component = registry_->try_register<std::remove_const_t<T>>();
                if (!component) {
                    Detail::contract_violation(
                        "ECS query could not resolve component '{}': {}",
                        Detail::component_name<std::remove_const_t<T>>(),
                        component.error().message);
                }
                return *component;
            };
            const std::array<ComponentId, sizeof...(Ts)> ids{
                resolve.template operator()<Ts>()...};
            Signature required{ids.begin(), ids.end()};
            std::sort(required.begin(), required.end());
            if (const auto duplicate = std::adjacent_find(required.begin(), required.end());
                duplicate != required.end()) {
                const ComponentInfo *descriptor = registry_->info(*duplicate);
                if (descriptor != nullptr) {
                    Detail::contract_violation(
                        "ECS query contains duplicate component term '{}'.",
                        descriptor->canonical_name);
                }
                Detail::contract_violation(
                    "ECS query contains duplicate dense component ID {}.",
                    *duplicate);
            }
            std::vector<u32> matches;
            for (usize i = 0; i < archetypes_.size(); ++i) {
                if (signature_is_superset(archetypes_[i].signature(), required)) {
                    matches.push_back(static_cast<u32>(i));
                }
            }
            return Query<Ts...>(&archetypes_, std::move(matches), ids, std::move(direct_access_lock));
        }

        template <class T>
        [[nodiscard]] T &resource_unchecked() noexcept {
            using ResourceT = std::remove_cv_t<T>;
            const auto resource = resources_.find(resource_key<ResourceT>());
            if (resource == resources_.end() || resource->second.object == nullptr) {
                Detail::contract_violation(
                    "ECS system requested unbound resource '{}'. Bind it to the World before Schedule::run().",
                    Detail::resource_name<ResourceT>());
            }
            return *static_cast<ResourceT *>(resource->second.object);
        }

        [[nodiscard]] Entity allocate_entity() {
            if (!free_indices_.empty()) {
                const u32 index = free_indices_.back();
                free_indices_.pop_back();
                return Entity{.index = index, .generation = entity_records_[index].generation};
            }
            const auto index = static_cast<u32>(entity_records_.size());
            entity_records_.push_back(EntityRecord{.generation = 1});
            return Entity{.index = index, .generation = 1};
        }

        [[nodiscard]] u32 archetype_index_for(const Signature &signature) {
            for (usize i = 0; i < archetypes_.size(); ++i) {
                if (archetypes_[i].signature() == signature) {
                    return static_cast<u32>(i);
                }
            }
            archetypes_.emplace_back(signature, *registry_);
            return static_cast<u32>(archetypes_.size() - 1);
        }

        ComponentRegistry *registry_ = nullptr;
        std::vector<EntityRecord> entity_records_;
        std::vector<u32> free_indices_;
        std::vector<Archetype> archetypes_;
        std::unordered_map<ResourceKey, ResourceRecord, ResourceKeyHash> resources_;
        mutable std::shared_mutex direct_access_mutex_;
        std::atomic<bool> scheduled_execution_{false};
    };

    namespace Detail {

        struct WorldAccess {
            [[nodiscard]] static std::unique_lock<std::shared_mutex> begin_schedule(World &world) noexcept {
                return world.begin_scheduled_execution();
            }

            static void end_schedule(World &world) noexcept {
                world.end_scheduled_execution();
            }

            template <class... Ts>
            [[nodiscard]] static Query<Ts...> query(World &world) {
                return world.query_impl<Ts...>();
            }

            template <class T>
            [[nodiscard]] static T &resource(World &world) noexcept {
                return world.resource_unchecked<T>();
            }

            template <class... Ts>
            [[nodiscard]] static Entity spawn(World &world, Ts &&...components) {
                return world.spawn_unchecked(std::forward<Ts>(components)...);
            }

            static void destroy(World &world, Entity entity) noexcept {
                world.destroy_unchecked(entity);
            }
        };

    } // namespace Detail

} // namespace SFT::Ecs
