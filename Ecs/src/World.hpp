#pragma once

#include <Ecs/src/Archetype.hpp>
#include <Ecs/src/Component.hpp>
#include <Ecs/src/Entity.hpp>
#include <Ecs/src/Event.hpp>
#include <Ecs/src/Query.hpp>
#include <Ecs/src/Resource.hpp>
#include <Ecs/src/Signature.hpp>

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

#include <tracy/Tracy.hpp>

namespace SFT::Ecs {

    namespace Detail {
        struct WorldAccess;
    }


    template <class T>
    class ComponentBorrow {
      public:
        /// Constructs a `ComponentBorrow` in its default state.
        ///
        /// @note This function does not throw exceptions.
        ComponentBorrow() noexcept = default;
        /// Disables this construction form for `ComponentBorrow`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ComponentBorrow(const ComponentBorrow &) = delete;
        /// Assigns a new value to this `ComponentBorrow`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ComponentBorrow &operator=(const ComponentBorrow &) = delete;
        /// Constructs a `ComponentBorrow` from another instance.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @note This function does not throw exceptions.
        ComponentBorrow(ComponentBorrow &&other) noexcept
            : component_(std::exchange(other.component_, nullptr)), access_(std::move(other.access_)) {}

        /// Assigns a new value to this `ComponentBorrow`.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        ComponentBorrow &operator=(ComponentBorrow &&other) noexcept {
            if (this == &other) {
                return *this;
            }
            component_ = nullptr;
            access_ = std::move(other.access_);
            component_ = std::exchange(other.component_, nullptr);
            return *this;
        }

        /// Converts the `ComponentBorrow` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] explicit operator bool() const noexcept { return component_ != nullptr; }
        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool operator==(std::nullptr_t) const noexcept { return component_ == nullptr; }
        /// Accesses the object referenced by this `ComponentBorrow`.
        ///
        /// @return Returns a pointer through which the referenced object can be accessed.
        /// @note This function does not throw exceptions.
        [[nodiscard]] T *operator->() const noexcept { return component_; }
        /// Dereferences this iterator or handle.
        ///
        /// @return Returns the value or reference currently addressed by the iterator/handle.
        /// @note This function does not throw exceptions.
        [[nodiscard]] T &operator*() const noexcept { return *component_; }

        /// Compares the operands for equality.
        ///
        /// @param borrow `borrow` value used by the operation.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        friend bool operator==(std::nullptr_t, const ComponentBorrow &borrow) noexcept {
            return borrow == nullptr;
        }

      private:
        friend class World;

        /// Constructs a `ComponentBorrow` from the supplied initialization values.
        ///
        /// @param component Component used or affected by the operation.
        /// @param access `access` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        ComponentBorrow(T *component, std::shared_lock<std::shared_mutex> access) noexcept
            : component_(component), access_(std::move(access)) {}

        T *component_ = nullptr;
        std::shared_lock<std::shared_mutex> access_;
    };


    class World {
      public:
        /// Constructs a `World` from the supplied initialization values.
        ///
        /// @param registry `registry` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit World(ComponentRegistry &registry) noexcept;
        /// Destroys the `World` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~World() = default;
        /// Disables this construction form for `World`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        World(const World &) = delete;
        /// Assigns a new value to this `World`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        World &operator=(const World &) = delete;
        /// Disables this construction form for `World`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        World(World &&) = delete;
        /// Assigns a new value to this `World`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        World &operator=(World &&) = delete;


        /// Spawns the supplied asynchronous work.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class... Ts>
        [[nodiscard]] Entity spawn(Ts &&...components) {
            ZoneScopedN("World::spawn");
            ensure_not_scheduled("spawn entities");
            auto access = acquire_direct_mutation("spawn entities");
            ensure_not_scheduled("spawn entities");
            return spawn_unchecked(std::forward<Ts>(components)...);
        }

        /// Destroys or releases the `World` resource represented by the supplied parameters.
        ///
        /// @param entity Entity used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy(Entity entity) noexcept;


        /// Adds component using the supplied arguments and current state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class T>
        void add_component(Entity entity, T component) {
            ZoneScopedN("World::add_component");
            ensure_not_scheduled("add a component");
            auto access = acquire_direct_mutation("add a component");
            ensure_not_scheduled("add a component");
            add_component_unchecked(entity, std::move(component));
        }


        /// Removes the component from its owning collection or system.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class T>
        void remove_component(Entity entity) {
            ZoneScopedN("World::remove_component");
            ensure_not_scheduled("remove a component");
            auto access = acquire_direct_mutation("remove a component");
            ensure_not_scheduled("remove a component");
            remove_component_unchecked<T>(entity);
        }

        /// Reports whether alive holds for this `World`.
        ///
        /// @param entity Entity used or affected by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_alive(Entity entity) const noexcept;

        /// Returns the component associated with this `World`.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        template <class T>
        [[nodiscard]] ComponentBorrow<T> get_component(Entity entity) noexcept {
            ZoneScopedN("World::get_component");
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

        /// Returns the component associated with this `World`.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        template <class T>
        [[nodiscard]] ComponentBorrow<const T> get_component(Entity entity) const noexcept {
            ZoneScopedN("World::get_component_const");
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


        /// Returns the current or globally available query value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class... Ts>
        [[nodiscard]] Query<Ts...> query() {
            ZoneScopedN("World::query");
            ensure_not_scheduled("create a direct query; scheduled systems receive safe query chunks automatically");
            std::shared_lock access{direct_access_mutex_};
            ensure_not_scheduled("create a direct query; scheduled systems receive safe query chunks automatically");
            return query_impl<Ts...>(std::move(access));
        }

        /// Returns the current or globally available registry value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ComponentRegistry &registry() noexcept;
        /// Returns the current or globally available registry value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const ComponentRegistry &registry() const noexcept;


        /// Binds resource for subsequent operations.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class T>
        void bind_resource(T &resource) {
            ZoneScopedN("World::bind_resource");
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
                .clear = event_clear_fn<ResourceT>(),
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
                existing->second.clear = incoming.clear;
                return;
            }
            resources_.emplace(key, incoming);
        }

        /// Performs the unbind resource operation for `World` using the supplied arguments.
        ///
        /// @note This function does not throw exceptions.
        template <class T>
        void unbind_resource() noexcept {
            ZoneScopedN("World::unbind_resource");
            using ResourceT = std::remove_cv_t<T>;
            ensure_not_scheduled("unbind an ECS resource");
            auto access = acquire_direct_mutation("unbind an ECS resource");
            ensure_not_scheduled("unbind an ECS resource");
            resources_.erase(resource_key<ResourceT>());
        }

        /// Reports whether this `World` has resource.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        template <class T>
        [[nodiscard]] bool has_resource() const noexcept {
            ZoneScopedN("World::has_resource");
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


            void (*clear)(void *) noexcept = nullptr;
        };


        /// Returns the current or globally available event clear fn value.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        template <class T>
        [[nodiscard]] static constexpr auto event_clear_fn() noexcept -> void (*)(void *) noexcept {
            if constexpr (is_event_resource_v<T>) {
                return [](void *object) noexcept { static_cast<T *>(object)->clear(); };
            } else {
                return nullptr;
            }
        }

        /// Spawns unchecked.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class... Ts>
        [[nodiscard]] Entity spawn_unchecked(Ts &&...components) {
            ZoneScopedN("World::spawn_unchecked");
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

        /// Adds component unchecked using the supplied arguments and current state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class T>
        void add_component_unchecked(Entity entity, T component) {
            ZoneScopedN("World::add_component_unchecked");
            static_assert(std::is_same_v<T, std::remove_cv_t<T>>, "World::add_component<T>() expects an unqualified T.");
            static_assert(std::is_nothrow_move_constructible_v<T>,
                          "World::add_component<T>() requires a nothrow move-constructible T.");
            if (!is_alive_unchecked(entity)) {
                Detail::contract_violation(
                    "ECS add_component<{}> on a dead or default-constructed entity.",
                    Detail::component_name<T>());
            }
            const ComponentId new_id = registry_->component<T>();
            EntityRecord &record = entity_records_[entity.index];
            Signature destination_signature = archetypes_[record.archetype_index].signature();
            if (archetypes_[record.archetype_index].column_index_of(new_id) != ~0u) {
                Detail::contract_violation(
                    "ECS add_component<{}>: entity already has this component.",
                    Detail::component_name<T>());
            }
            destination_signature.insert(
                std::lower_bound(destination_signature.begin(), destination_signature.end(), new_id),
                new_id);

            const u32 source_index = record.archetype_index;
            const u32 source_row = record.row;


            const u32 destination_index = archetype_index_for(destination_signature);
            Archetype &destination = archetypes_[destination_index];
            const u32 destination_row = destination.add_row(entity);
            ::new (destination.row_pointer(destination.column_index_of(new_id), destination_row))
                T(std::move(component));
            const Entity moved = archetypes_[source_index].move_row_into(source_row, destination, destination_row);
            if (moved) {
                entity_records_[moved.index].row = source_row;
            }
            record.archetype_index = destination_index;
            record.row = destination_row;
        }

        /// Removes the component unchecked from its owning collection or system.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class T>
        void remove_component_unchecked(Entity entity) {
            ZoneScopedN("World::remove_component_unchecked");
            static_assert(std::is_same_v<T, std::remove_cv_t<T>>, "World::remove_component<T>() expects an unqualified T.");
            if (!is_alive_unchecked(entity)) {
                Detail::contract_violation(
                    "ECS remove_component<{}> on a dead or default-constructed entity.",
                    Detail::component_name<T>());
            }
            const std::optional<ComponentId> removed_id = registry_->find(component_key<T>());
            EntityRecord &record = entity_records_[entity.index];
            Archetype &source = archetypes_[record.archetype_index];
            if (!removed_id || source.column_index_of(*removed_id) == ~0u) {
                Detail::contract_violation(
                    "ECS remove_component<{}>: entity does not have this component.",
                    Detail::component_name<T>());
            }
            Signature destination_signature = source.signature();
            destination_signature.erase(
                std::remove(destination_signature.begin(), destination_signature.end(), *removed_id),
                destination_signature.end());

            const u32 source_index = record.archetype_index;
            const u32 source_row = record.row;


            const u32 destination_index = archetype_index_for(destination_signature);
            Archetype &destination = archetypes_[destination_index];
            const u32 destination_row = destination.add_row(entity);
            const Entity moved = archetypes_[source_index].move_row_into(source_row, destination, destination_row);
            if (moved) {
                entity_records_[moved.index].row = source_row;
            }
            record.archetype_index = destination_index;
            record.row = destination_row;
        }

        /// Finds or creates the not scheduled required by the operation.
        ///
        /// @param action `action` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void ensure_not_scheduled(string_view action) const noexcept;

        /// Acquires direct mutation.
        ///
        /// @param action `action` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::unique_lock<std::shared_mutex> acquire_direct_mutation(string_view action) noexcept;

        /// Returns the current or globally available begin scheduled execution value.
        ///
        /// @return Returns the current begin scheduled execution value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::unique_lock<std::shared_mutex> begin_scheduled_execution() noexcept;

        /// Performs the end scheduled execution operation for `World` using the supplied arguments.
        ///
        /// @note This function does not throw exceptions.
        void end_scheduled_execution() noexcept;

        /// Reports whether alive unchecked holds for this `World`.
        ///
        /// @param entity Entity used or affected by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_alive_unchecked(Entity entity) const noexcept;

        /// Destroys the unchecked identified by the supplied parameters.
        ///
        /// @param entity Entity used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_unchecked(Entity entity) noexcept;

        /// Queries impl from the active backend or runtime state.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class... Ts>
        [[nodiscard]] Query<Ts...> query_impl(std::shared_lock<std::shared_mutex> direct_access_lock = {}) {
            ZoneScopedN("World::query_impl");
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

        /// Returns the current or globally available resource unchecked value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        template <class T>
        [[nodiscard]] T &resource_unchecked() noexcept {
            ZoneScopedN("World::resource_unchecked");
            using ResourceT = std::remove_cv_t<T>;
            const auto resource = resources_.find(resource_key<ResourceT>());
            if (resource == resources_.end() || resource->second.object == nullptr) {
                Detail::contract_violation(
                    "ECS system requested unbound resource '{}'. Bind it to the World before Schedule::run().",
                    Detail::resource_name<ResourceT>());
            }
            return *static_cast<ResourceT *>(resource->second.object);
        }

        /// Allocates entity.
        ///
        /// @return Returns the current allocate entity value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Entity allocate_entity();

        /// Resolves the archetype index associated with the supplied key, handle, or resource.
        ///
        /// @param signature `signature` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] u32 archetype_index_for(const Signature &signature);

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
            /// Performs the begin schedule operation for `WorldAccess` using the supplied arguments.
            ///
            /// @param world World used or affected by the operation.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function does not throw exceptions.
            [[nodiscard]] static std::unique_lock<std::shared_mutex> begin_schedule(World &world) noexcept;

            /// Performs the end schedule operation for `WorldAccess` using the supplied arguments.
            ///
            /// @param world World used or affected by the operation.
            ///
            /// @note This function does not throw exceptions.
            static void end_schedule(World &world) noexcept;

            /// Returns the current or globally available query value.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            template <class... Ts>
            [[nodiscard]] static Query<Ts...> query(World &world) {
                return world.query_impl<Ts...>();
            }

            /// Returns the current or globally available resource value.
            ///
            /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
            /// @note This function does not throw exceptions.
            template <class T>
            [[nodiscard]] static T &resource(World &world) noexcept {
                return world.resource_unchecked<T>();
            }

            /// Spawns the supplied asynchronous work.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            template <class... Ts>
            [[nodiscard]] static Entity spawn(World &world, Ts &&...components) {
                return world.spawn_unchecked(std::forward<Ts>(components)...);
            }

            /// Destroys or releases the `WorldAccess` resource represented by the supplied parameters.
            ///
            /// @param world World used or affected by the operation.
            /// @param entity Entity used or affected by the operation.
            ///
            /// @note This function does not throw exceptions.
            static void destroy(World &world, Entity entity) noexcept;

            /// Adds component using the supplied arguments and current state.
            ///
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            template <class T>
            static void add_component(World &world, Entity entity, T component) {
                world.add_component_unchecked(entity, std::move(component));
            }

            /// Removes the component from its owning collection or system.
            ///
            /// @param world World used or affected by the operation.
            /// @param entity Entity used or affected by the operation.
            ///
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            template <class T>
            static void remove_component(World &world, Entity entity) {
                world.remove_component_unchecked<T>(entity);
            }


            /// Clears event resources.
            ///
            /// @param world World used or affected by the operation.
            ///
            /// @note This function does not throw exceptions.
            static void clear_event_resources(World &world) noexcept;
        };

    } // namespace Detail

} // namespace SFT::Ecs
