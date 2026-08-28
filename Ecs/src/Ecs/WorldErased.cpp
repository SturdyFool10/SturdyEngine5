/// Type-erased `World` operations: the non-template siblings of `spawn`, `add_component`,
/// `remove_component`, `get_component` and query iteration.
///
/// The templated API in `World.hpp` resolves a `ComponentId` from a C++ type and treats a dead
/// entity or a duplicate component as a contract violation — `Detail::contract_violation` logs and
/// calls `std::terminate`. That is the right behavior for engine C++, which had the type system and
/// could have checked. It is the wrong behavior when the caller is a Rust or C# binding holding an
/// entity value that was valid a frame ago: killing the process is a denial of service triggered by
/// an ordinary application bug.
///
/// So every entry point here validates first and returns `WorldErasedError` rather than
/// terminating, and leaves the world unmodified on any failure. That validation *is* the feature —
/// it is what makes the ECS safe to expose across a language boundary.

#include <Ecs/World.hpp>

#include <cstring>

namespace SFT::Ecs {

    namespace {

        /// Builds a failure result.
        ///
        /// @param code Machine-readable reason.
        /// @param message Human-readable detail.
        ///
        /// @return The populated error.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] std::unexpected<WorldErasedError> erased_error(WorldErasedErrorCode code,
                                                                     std::string_view message) {
            return std::unexpected(WorldErasedError{.code = code, .message = UString{std::string{message}}});
        }

    } // namespace

    /// Spawns an entity carrying the supplied components, addressed by id.
    ///
    /// @param component_ids Components to attach.
    /// @param component_data Initial value for each component.
    ///
    /// @return The new entity, or the reason it could not be created.
    /// @note Normal failures are returned through the type-specific error/status state.
    WorldErasedExpected<Entity> World::spawn_erased(std::span<const ComponentId> component_ids,
                                                    std::span<const void *const> component_data) {
        ZoneScopedN("World::spawn_erased");

        if (component_ids.empty()) {
            return erased_error(WorldErasedErrorCode::NoComponents,
                                "spawning an entity requires at least one component");
        }
        if (component_ids.size() != component_data.size()) {
            return erased_error(WorldErasedErrorCode::InvalidArgument,
                                "component id and data counts must match");
        }
        if (scheduled_execution_.load(std::memory_order_acquire)) {
            return erased_error(WorldErasedErrorCode::ScheduleRunning,
                                "cannot spawn entities directly while a schedule is running");
        }

        auto access = acquire_direct_mutation("spawn entities");
        if (scheduled_execution_.load(std::memory_order_acquire)) {
            return erased_error(WorldErasedErrorCode::ScheduleRunning,
                                "cannot spawn entities directly while a schedule is running");
        }
        return spawn_erased_unlocked(component_ids, component_data);
    }

    /// Spawns an entity without locking or checking for a running schedule.
    ///
    /// Split out for deferred commands, which apply from inside `Schedule::run` where the world
    /// lock is already held and the schedule flag is deliberately still set. The validation is
    /// identical; only the two guards that assume a direct caller are absent.
    ///
    /// @param component_ids Components to attach.
    /// @param component_data Initial value for each component.
    ///
    /// @return The new entity, or the reason it could not be created.
    /// @note Normal failures are returned through the type-specific error/status state.
    WorldErasedExpected<Entity> World::spawn_erased_unlocked(std::span<const ComponentId> component_ids,
                                                             std::span<const void *const> component_data) {
        ZoneScopedN("World::spawn_erased_unlocked");

        if (component_ids.empty()) {
            return erased_error(WorldErasedErrorCode::NoComponents,
                                "spawning an entity requires at least one component");
        }
        if (component_ids.size() != component_data.size()) {
            return erased_error(WorldErasedErrorCode::InvalidArgument,
                                "component id and data counts must match");
        }

        // Everything is validated before the world is touched, so a rejected spawn leaves no
        // half-built entity or stray archetype behind.
        for (usize index = 0; index < component_ids.size(); ++index) {
            const ComponentInfo *descriptor = registry_->info(component_ids[index]);
            if (descriptor == nullptr) {
                return erased_error(WorldErasedErrorCode::UnknownComponent,
                                    "no component is registered under one of the supplied ids");
            }
            if (component_data[index] == nullptr && descriptor->size != 0) {
                return erased_error(WorldErasedErrorCode::InvalidArgument,
                                    "component data pointer must not be null");
            }
            if (!has_flag(descriptor->flags, ComponentFlags::TriviallyCopyable)) {
                return erased_error(WorldErasedErrorCode::NotTriviallyCopyable,
                                    "component bytes cannot be copied in without running its C++ constructor");
            }
            for (usize other = 0; other < index; ++other) {
                if (component_ids[other] == component_ids[index]) {
                    return erased_error(WorldErasedErrorCode::DuplicateComponent,
                                        "the same component was supplied more than once");
                }
            }
        }

        Signature signature{component_ids.begin(), component_ids.end()};
        std::sort(signature.begin(), signature.end());

        const u32 archetype_index = archetype_index_for(signature);
        Archetype &archetype = archetypes_[archetype_index];

        const Entity entity = allocate_entity();
        const u32 row = archetype.add_row(entity);
        for (usize index = 0; index < component_ids.size(); ++index) {
            const ComponentInfo *descriptor = registry_->info(component_ids[index]);
            void *destination = archetype.row_pointer(archetype.column_index_of(component_ids[index]), row);
            if (descriptor->size != 0) {
                std::memcpy(destination, component_data[index], descriptor->size);
            }
        }

        EntityRecord &record = entity_records_[entity.index];
        record.archetype_index = archetype_index;
        record.row = row;
        return entity;
    }

    /// Adds one component to an existing entity, addressed by id.
    ///
    /// @param entity Entity to modify.
    /// @param component Component to attach.
    /// @param data Initial value.
    ///
    /// @return Success, or the reason the component could not be attached.
    /// @note Normal failures are returned through the type-specific error/status state.
    WorldErasedExpected<void> World::add_component_erased(Entity entity,
                                                          ComponentId component,
                                                          const void *data) {
        ZoneScopedN("World::add_component_erased");

        const ComponentInfo *descriptor = registry_->info(component);
        if (descriptor == nullptr) {
            return erased_error(WorldErasedErrorCode::UnknownComponent,
                                "no component is registered under that id");
        }
        if (data == nullptr && descriptor->size != 0) {
            return erased_error(WorldErasedErrorCode::InvalidArgument,
                                "component data pointer must not be null");
        }
        if (!has_flag(descriptor->flags, ComponentFlags::TriviallyCopyable)) {
            return erased_error(WorldErasedErrorCode::NotTriviallyCopyable,
                                "component bytes cannot be copied in without running its C++ constructor");
        }
        if (scheduled_execution_.load(std::memory_order_acquire)) {
            return erased_error(WorldErasedErrorCode::ScheduleRunning,
                                "cannot add components directly while a schedule is running");
        }

        auto access = acquire_direct_mutation("add a component");
        if (scheduled_execution_.load(std::memory_order_acquire)) {
            return erased_error(WorldErasedErrorCode::ScheduleRunning,
                                "cannot add components directly while a schedule is running");
        }
        return add_component_erased_unlocked(entity, component, data);
    }

    /// Adds one component without locking or checking for a running schedule.
    ///
    /// Split out for deferred commands; see `spawn_erased_unlocked`.
    ///
    /// @param entity Entity to modify.
    /// @param component Component to attach.
    /// @param data Initial value.
    ///
    /// @return Success, or the reason the component could not be attached.
    /// @note Normal failures are returned through the type-specific error/status state.
    WorldErasedExpected<void> World::add_component_erased_unlocked(Entity entity,
                                                                    ComponentId component,
                                                                    const void *data) {
        ZoneScopedN("World::add_component_erased_unlocked");

        const ComponentInfo *descriptor = registry_->info(component);
        if (descriptor == nullptr) {
            return erased_error(WorldErasedErrorCode::UnknownComponent,
                                "no component is registered under that id");
        }
        if (data == nullptr && descriptor->size != 0) {
            return erased_error(WorldErasedErrorCode::InvalidArgument,
                                "component data pointer must not be null");
        }
        if (!has_flag(descriptor->flags, ComponentFlags::TriviallyCopyable)) {
            return erased_error(WorldErasedErrorCode::NotTriviallyCopyable,
                                "component bytes cannot be copied in without running its C++ constructor");
        }
        if (!is_alive_unchecked(entity)) {
            return erased_error(WorldErasedErrorCode::DeadEntity,
                                "the entity is not alive");
        }

        EntityRecord &record = entity_records_[entity.index];
        if (archetypes_[record.archetype_index].column_index_of(component) != ~0u) {
            return erased_error(WorldErasedErrorCode::DuplicateComponent,
                                "the entity already carries that component");
        }

        Signature destination_signature = archetypes_[record.archetype_index].signature();
        destination_signature.insert(
            std::lower_bound(destination_signature.begin(), destination_signature.end(), component),
            component);

        const u32 source_index = record.archetype_index;
        const u32 source_row = record.row;

        const u32 destination_index = archetype_index_for(destination_signature);
        Archetype &destination = archetypes_[destination_index];
        const u32 destination_row = destination.add_row(entity);
        if (descriptor->size != 0) {
            std::memcpy(destination.row_pointer(destination.column_index_of(component), destination_row),
                        data, descriptor->size);
        }

        const Entity moved = archetypes_[source_index].move_row_into(source_row, destination, destination_row);
        if (moved) {
            entity_records_[moved.index].row = source_row;
        }
        record.archetype_index = destination_index;
        record.row = destination_row;
        return {};
    }

    /// Removes one component from an entity, addressed by id.
    ///
    /// @param entity Entity to modify.
    /// @param component Component to detach.
    ///
    /// @return Success, or the reason the component could not be detached.
    /// @note Normal failures are returned through the type-specific error/status state.
    WorldErasedExpected<void> World::remove_component_erased(Entity entity, ComponentId component) {
        ZoneScopedN("World::remove_component_erased");

        if (registry_->info(component) == nullptr) {
            return erased_error(WorldErasedErrorCode::UnknownComponent,
                                "no component is registered under that id");
        }
        if (scheduled_execution_.load(std::memory_order_acquire)) {
            return erased_error(WorldErasedErrorCode::ScheduleRunning,
                                "cannot remove components directly while a schedule is running");
        }

        auto access = acquire_direct_mutation("remove a component");
        if (scheduled_execution_.load(std::memory_order_acquire)) {
            return erased_error(WorldErasedErrorCode::ScheduleRunning,
                                "cannot remove components directly while a schedule is running");
        }
        return remove_component_erased_unlocked(entity, component);
    }

    /// Removes one component without locking or checking for a running schedule.
    ///
    /// Split out for deferred commands; see `spawn_erased_unlocked`.
    ///
    /// @param entity Entity to modify.
    /// @param component Component to detach.
    ///
    /// @return Success, or the reason the component could not be detached.
    /// @note Normal failures are returned through the type-specific error/status state.
    WorldErasedExpected<void> World::remove_component_erased_unlocked(Entity entity, ComponentId component) {
        ZoneScopedN("World::remove_component_erased_unlocked");

        if (registry_->info(component) == nullptr) {
            return erased_error(WorldErasedErrorCode::UnknownComponent,
                                "no component is registered under that id");
        }
        if (!is_alive_unchecked(entity)) {
            return erased_error(WorldErasedErrorCode::DeadEntity, "the entity is not alive");
        }

        EntityRecord &record = entity_records_[entity.index];
        Archetype &source = archetypes_[record.archetype_index];
        if (source.column_index_of(component) == ~0u) {
            return erased_error(WorldErasedErrorCode::MissingComponent,
                                "the entity does not carry that component");
        }

        Signature destination_signature = source.signature();
        destination_signature.erase(
            std::remove(destination_signature.begin(), destination_signature.end(), component),
            destination_signature.end());

        // Removing an entity's last component would leave it in an archetype with an empty
        // signature, which the templated path never produces because spawn requires at least one.
        // Refusing keeps that invariant rather than creating a shape nothing else expects.
        if (destination_signature.empty()) {
            return erased_error(WorldErasedErrorCode::NoComponents,
                                "an entity must keep at least one component; destroy it instead");
        }

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
        return {};
    }

    /// Reports whether an entity carries a component.
    ///
    /// @param entity Entity to inspect.
    /// @param component Component to look for.
    ///
    /// @return `true` when the entity is alive and carries the component.
    /// @note This function does not throw exceptions.
    bool World::has_component_erased(Entity entity, ComponentId component) const noexcept {
        ZoneScopedN("World::has_component_erased");
        std::shared_lock access{direct_access_mutex_};
        if (!is_alive_unchecked(entity)) {
            return false;
        }
        const EntityRecord &record = entity_records_[entity.index];
        return archetypes_[record.archetype_index].column_index_of(component) != ~0u;
    }

    /// Copies a component's bytes out of the world.
    ///
    /// @param entity Entity to read from.
    /// @param component Component to read.
    /// @param destination Buffer receiving the bytes.
    /// @param size Bytes available.
    ///
    /// @return Success, or the reason the component could not be read.
    /// @note Normal failures are returned through the type-specific error/status state.
    WorldErasedExpected<void> World::read_component_erased(Entity entity,
                                                           ComponentId component,
                                                           void *destination,
                                                           usize size) const {
        ZoneScopedN("World::read_component_erased");

        const ComponentInfo *descriptor = registry_->info(component);
        if (descriptor == nullptr) {
            return erased_error(WorldErasedErrorCode::UnknownComponent,
                                "no component is registered under that id");
        }
        if (destination == nullptr && descriptor->size != 0) {
            return erased_error(WorldErasedErrorCode::InvalidArgument,
                                "destination pointer must not be null");
        }
        // Exact match rather than "at least": a caller whose struct is a different size than the
        // engine's has a layout disagreement, and copying the overlap would hand back plausible
        // but wrong values instead of surfacing the mismatch.
        if (size != descriptor->size) {
            return erased_error(WorldErasedErrorCode::SizeMismatch,
                                "supplied size does not match the component's registered size");
        }
        if (!has_flag(descriptor->flags, ComponentFlags::TriviallyCopyable)) {
            return erased_error(WorldErasedErrorCode::NotTriviallyCopyable,
                                "component bytes cannot be copied out without running its C++ copy constructor");
        }

        std::shared_lock access{direct_access_mutex_};
        if (!is_alive_unchecked(entity)) {
            return erased_error(WorldErasedErrorCode::DeadEntity, "the entity is not alive");
        }
        const EntityRecord &record = entity_records_[entity.index];
        const Archetype &archetype = archetypes_[record.archetype_index];
        const u32 column = archetype.column_index_of(component);
        if (column == ~0u) {
            return erased_error(WorldErasedErrorCode::MissingComponent,
                                "the entity does not carry that component");
        }
        if (descriptor->size != 0) {
            std::memcpy(destination, archetype.row_pointer(column, record.row), descriptor->size);
        }
        return {};
    }

    /// Overwrites a component's bytes in place.
    ///
    /// @param entity Entity to write to.
    /// @param component Component to write.
    /// @param source Bytes to copy in.
    /// @param size Bytes supplied.
    ///
    /// @return Success, or the reason the component could not be written.
    /// @note Normal failures are returned through the type-specific error/status state.
    WorldErasedExpected<void> World::write_component_erased(Entity entity,
                                                            ComponentId component,
                                                            const void *source,
                                                            usize size) {
        ZoneScopedN("World::write_component_erased");

        const ComponentInfo *descriptor = registry_->info(component);
        if (descriptor == nullptr) {
            return erased_error(WorldErasedErrorCode::UnknownComponent,
                                "no component is registered under that id");
        }
        if (source == nullptr && descriptor->size != 0) {
            return erased_error(WorldErasedErrorCode::InvalidArgument,
                                "source pointer must not be null");
        }
        if (size != descriptor->size) {
            return erased_error(WorldErasedErrorCode::SizeMismatch,
                                "supplied size does not match the component's registered size");
        }
        if (!has_flag(descriptor->flags, ComponentFlags::TriviallyCopyable)) {
            return erased_error(WorldErasedErrorCode::NotTriviallyCopyable,
                                "component bytes cannot be copied in without running its C++ assignment");
        }

        // A shared lock is enough: this overwrites bytes in place and performs no structural
        // change, so it cannot relocate rows out from under a concurrent reader.
        std::shared_lock access{direct_access_mutex_};
        if (!is_alive_unchecked(entity)) {
            return erased_error(WorldErasedErrorCode::DeadEntity, "the entity is not alive");
        }
        EntityRecord &record = entity_records_[entity.index];
        Archetype &archetype = archetypes_[record.archetype_index];
        const u32 column = archetype.column_index_of(component);
        if (column == ~0u) {
            return erased_error(WorldErasedErrorCode::MissingComponent,
                                "the entity does not carry that component");
        }
        if (descriptor->size != 0) {
            std::memcpy(archetype.row_pointer(column, record.row), source, descriptor->size);
        }
        return {};
    }

    /// Visits every live entity carrying all of `component_ids`.
    ///
    /// @param component_ids Components an entity must all carry to be visited.
    /// @param visit Invoked once per matching entity.
    /// @param user_data Passed through to `visit`.
    ///
    /// @return How many entities were visited, or the reason iteration could not start.
    /// @note Normal failures are returned through the type-specific error/status state.
    WorldErasedExpected<usize> World::for_each_erased(std::span<const ComponentId> component_ids,
                                                      ErasedVisitFn visit,
                                                      void *user_data) {
        ZoneScopedN("World::for_each_erased");
        std::shared_lock access{direct_access_mutex_};
        return for_each_unlocked(component_ids, visit, user_data);
    }

    /// Walks matching archetypes without taking any lock.
    ///
    /// @param component_ids Components an entity must all carry to be visited.
    /// @param visit Invoked once per matching entity.
    /// @param user_data Passed through to `visit`.
    ///
    /// @return How many entities were visited, or why iteration could not start.
    /// @note Normal failures are returned through the type-specific error/status state.
    WorldErasedExpected<usize> World::for_each_unlocked(std::span<const ComponentId> component_ids,
                                                        ErasedVisitFn visit,
                                                        void *user_data) {
        ZoneScopedN("World::for_each_unlocked");

        if (visit == nullptr) {
            return erased_error(WorldErasedErrorCode::InvalidArgument, "visitor must not be null");
        }
        if (component_ids.empty()) {
            return erased_error(WorldErasedErrorCode::NoComponents,
                                "iteration requires at least one component to match on");
        }
        if (component_ids.size() > max_erased_visit_components) {
            return erased_error(WorldErasedErrorCode::InvalidArgument,
                                "too many components requested for a single visit");
        }
        for (const ComponentId component : component_ids) {
            if (registry_->info(component) == nullptr) {
                return erased_error(WorldErasedErrorCode::UnknownComponent,
                                    "no component is registered under one of the supplied ids");
            }
        }

        usize visited = 0;
        std::array<void *, max_erased_visit_components> pointers{};
        std::array<u32, max_erased_visit_components> columns{};

        for (Archetype &archetype : archetypes_) {
            bool matches = true;
            for (usize index = 0; index < component_ids.size(); ++index) {
                const u32 column = archetype.column_index_of(component_ids[index]);
                if (column == ~0u) {
                    matches = false;
                    break;
                }
                columns[index] = column;
            }
            if (!matches) {
                continue;
            }

            // Row count is read once per archetype rather than per row: `visit` is forbidden from
            // mutating the world, so the count cannot change mid-archetype, and re-reading it
            // would only hide a violation of that rule rather than tolerate it.
            const usize row_count = archetype.size();
            for (usize row = 0; row < row_count; ++row) {
                for (usize index = 0; index < component_ids.size(); ++index) {
                    pointers[index] = archetype.row_pointer(columns[index], static_cast<u32>(row));
                }
                visit(archetype.entity_at(static_cast<u32>(row)), pointers.data(), user_data);
                ++visited;
            }
        }
        return visited;
    }

} // namespace SFT::Ecs
