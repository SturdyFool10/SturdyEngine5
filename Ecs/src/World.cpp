#include <Ecs/src/World.hpp>


namespace SFT::Ecs {

    void World::destroy(Entity entity) noexcept {
        ZoneScopedN("World::destroy");
        ensure_not_scheduled("destroy entities directly; use Commands::destroy() inside a system");
        auto access = acquire_direct_mutation("destroy entities");
        ensure_not_scheduled("destroy entities directly; use Commands::destroy() inside a system");
        destroy_unchecked(entity);
    }

    bool World::is_alive(Entity entity) const noexcept {
        ZoneScopedN("World::is_alive");
        ensure_not_scheduled("inspect entity liveness directly");
        std::shared_lock access{direct_access_mutex_};
        ensure_not_scheduled("inspect entity liveness directly");
        return is_alive_unchecked(entity);
    }

    ComponentRegistry &World::registry() noexcept { return *registry_; }

    const ComponentRegistry &World::registry() const noexcept { return *registry_; }

    void World::ensure_not_scheduled(string_view action) const noexcept {
        ZoneScopedN("World::ensure_not_scheduled");
        if (scheduled_execution_.load(std::memory_order_acquire)) {
            Detail::contract_violation(
                "Unsafe ECS World access while an async schedule is active: cannot {}.",
                action);
        }
    }

    std::unique_lock<std::shared_mutex> World::acquire_direct_mutation(string_view action) noexcept {
        ZoneScopedN("World::acquire_direct_mutation");
        std::unique_lock access{direct_access_mutex_, std::try_to_lock};
        if (!access.owns_lock()) {
            Detail::contract_violation(
                "Cannot {} while a direct ECS query or component borrow is active.",
                action);
        }
        return access;
    }

    std::unique_lock<std::shared_mutex> World::begin_scheduled_execution() noexcept {
        ZoneScopedN("World::begin_scheduled_execution");
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

    void World::end_scheduled_execution() noexcept {
        ZoneScopedN("World::end_scheduled_execution");
        scheduled_execution_.store(false, std::memory_order_release);
    }

    bool World::is_alive_unchecked(Entity entity) const noexcept {
        ZoneScopedN("World::is_alive_unchecked");
        return entity.generation != 0 && entity.index < entity_records_.size() &&
               entity_records_[entity.index].generation == entity.generation;
    }

    void World::destroy_unchecked(Entity entity) noexcept {
        ZoneScopedN("World::destroy_unchecked");
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

    Entity World::allocate_entity() {
        ZoneScopedN("World::allocate_entity");
        if (!free_indices_.empty()) {
            const u32 index = free_indices_.back();
            free_indices_.pop_back();
            return Entity{.index = index, .generation = entity_records_[index].generation};
        }
        const auto index = static_cast<u32>(entity_records_.size());
        entity_records_.push_back(EntityRecord{.generation = 1});
        return Entity{.index = index, .generation = 1};
    }

    u32 World::archetype_index_for(const Signature &signature) {
        ZoneScopedN("World::archetype_index_for");
        for (usize i = 0; i < archetypes_.size(); ++i) {
            if (archetypes_[i].signature() == signature) {
                return static_cast<u32>(i);
            }
        }
        archetypes_.emplace_back(signature, *registry_);
        return static_cast<u32>(archetypes_.size() - 1);
    }

} // namespace SFT::Ecs

namespace SFT::Ecs::Detail {

    std::unique_lock<std::shared_mutex> WorldAccess::begin_schedule(World &world) noexcept {
        return world.begin_scheduled_execution();
    }

    void WorldAccess::end_schedule(World &world) noexcept {
        world.end_scheduled_execution();
    }

    void WorldAccess::destroy(World &world, Entity entity) noexcept {
        world.destroy_unchecked(entity);
    }

    void WorldAccess::clear_event_resources(World &world) noexcept {
        ZoneScopedN("WorldAccess::clear_event_resources");
        for (auto &[key, record] : world.resources_) {
            if (record.clear != nullptr) {
                record.clear(record.object);
            }
        }
    }

} // namespace SFT::Ecs::Detail


namespace SFT::Ecs {

    World::World(ComponentRegistry &registry) noexcept : registry_(&registry) {}

} // namespace SFT::Ecs

