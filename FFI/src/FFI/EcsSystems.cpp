/// C ABI implementation of ECS systems and deferred commands.
///
/// A C++ system declares what it touches through its parameter types, and the engine derives a
/// `SystemAccess` from them; that declaration is what lets the scheduler run non-conflicting
/// systems in parallel. A function pointer carries no type information, so the caller supplies the
/// access list explicitly and this layer translates it into the same `SystemAccess` the typed path
/// produces. From the scheduler's point of view a foreign system is then indistinguishable from a
/// C++ one.
///
/// Structural changes go through `Commands`, exactly as they do for typed systems: the world is
/// locked for the duration of a schedule, so spawning or destroying has to be queued and applied at
/// the stage boundary. The `Commands` handle is minted once per dispatch rather than once per
/// entity — the engine's prepare/finish hooks exist for that — because minting is mutex-guarded and
/// a per-entity mint would put a lock acquisition on the hottest path in the ECS.

#include <Foundation/Foundation.hpp>

#include <mutex>
#include <unordered_map>
#include <vector>

#include <Ecs/Commands.hpp>
#include <Ecs/System.hpp>
#include <Ecs/World.hpp>
#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::HandleKind;
    using SFT::Ffi::guarded;
    using SFT::Ffi::mint_handle;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::resolve_handle;
    using SFT::Ffi::revoke_handle;
    using SFT::Ffi::set_error;
    using SFT::usize;

    /// Everything one registered system needs at dispatch time.
    ///
    /// Heap-allocated. A system registered through `sturdy_ecs_add_system*` lives as long as the engine
    /// and this is never freed; one registered through `sturdy_ecs_add_system_ex` is freed by
    /// `sturdy_ecs_remove_system`.
    struct RegisteredSystem {
        SturdySystemFn system;
        void *user_data;
    };

    /// Mints the `Commands` handle for one dispatch.
    ///
    /// @param commands Queue for this dispatch.
    /// @param user_data The registered system.
    ///
    /// @return The minted token, carried as an opaque pointer.
    /// @note This function does not throw exceptions.
    void *STURDY_ABI_CALL prepare_dispatch(SFT::Ecs::Commands *commands, void *) noexcept {
        try {
            return reinterpret_cast<void *>(static_cast<SFT::usize>(mint_handle(HandleKind::Commands, commands)));
        } catch (...) {
            // A failed mint costs this dispatch its Commands rather than taking the process down;
            // the per-entity work still runs, and queueing reports an invalid handle.
            return nullptr;
        }
    }

    /// Revokes the dispatch's `Commands` handle.
    ///
    /// This is what stops a system from stashing its `SturdyCommands` and queueing work after the
    /// buffer it points at has been applied and destroyed.
    ///
    /// @param dispatch_context Token minted by `prepare_dispatch`.
    /// @note This function does not throw exceptions.
    void STURDY_ABI_CALL finish_dispatch(void *dispatch_context, void *) noexcept {
        revoke_handle(static_cast<SFT::u64>(reinterpret_cast<SFT::usize>(dispatch_context)));
    }

    /// Forwards one entity to the caller's system body.
    ///
    /// @param entity Entity being visited.
    /// @param components Pointers to the declared components.
    /// @param dispatch_context Token minted by `prepare_dispatch`.
    /// @param user_data The registered system.
    /// @note This function does not throw exceptions.
    void STURDY_ABI_CALL invoke_system(SFT::Ecs::Entity entity,
                                       void **components,
                                       void *dispatch_context,
                                       void *user_data) noexcept {
        const auto *registered = static_cast<const RegisteredSystem *>(user_data);
        SturdyEntity abi_entity;
        abi_entity.index = entity.index;
        abi_entity.generation = entity.generation;
        SturdyCommands commands;
        commands.token = static_cast<uint64_t>(reinterpret_cast<SFT::usize>(dispatch_context));
        registered->system(abi_entity, components, commands, registered->user_data);
    }

    /// Resolves a commands handle to the queue it refers to.
    ///
    /// @param commands Handle supplied to a system body.
    /// @param out_commands Receives the borrowed queue on success.
    ///
    /// @return `STURDY_OK`, or the handle failure `resolve_handle` reported.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyResult resolve_commands(SturdyCommands commands,
                                                SFT::Ecs::Commands **out_commands) noexcept {
        void *pointer = nullptr;
        const SturdyResult result = resolve_handle(commands.token, HandleKind::Commands, &pointer);
        if (result != STURDY_OK) {
            return result;
        }
        *out_commands = static_cast<SFT::Ecs::Commands *>(pointer);
        return STURDY_OK;
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_ecs_add_system(SturdyEngine engine,
                                                   const SturdySystemAccess *access,
                                                   uint32_t access_count,
                                                   SturdySystemFn system,
                                                   void *user_data) {
    return sturdy_ecs_add_system_with_resources(engine, access, access_count, nullptr, 0, system,
                                                user_data);
}

}

namespace {

    /// Shared body of the two registration entry points. `out_handle`, when non-null, receives the
    /// scheduler's handle and the leaked `RegisteredSystem` is reported through `out_registered`
    /// so a removable registration can free it later.
    SturdyResult register_system(SturdyEngine engine,
                                 const SturdySystemAccess *access,
                                 uint32_t access_count,
                                 const SturdySystemResourceAccess *resource_access,
                                 uint32_t resource_access_count,
                                 SturdySystemFn system,
                                 void *user_data,
                                 SFT::Engine::Engine **out_engine,
                                 SFT::Ecs::SystemHandle *out_handle,
                                 RegisteredSystem **out_registered) {
    return guarded([&]() -> SturdyResult {
        if (system == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "system body must not be null");
        }
        if (access_count != 0 && access == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "component access array must not be null when a count is given");
        }
        if (resource_access_count != 0 && resource_access == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "resource access array must not be null when a count is given");
        }
        if (access_count > SFT::Ecs::max_erased_visit_components) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "a system may declare at most 16 components");
        }
        if (access_count == 0 && resource_access_count == 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "a system must declare at least one component or resource");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        SFT::Ecs::World &world = resolved_engine->ecs_world();

        SFT::Ecs::SystemAccess system_access;
        std::vector<SFT::Ecs::ComponentId> ids;
        ids.reserve(access_count);

        for (uint32_t index = 0; index < access_count; ++index) {
            const SFT::Ecs::ComponentInfo *descriptor = world.registry().info(access[index].component);
            if (descriptor == nullptr) {
                return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                                 "no component is registered under one of the declared ids");
            }
            for (uint32_t other = 0; other < index; ++other) {
                if (access[other].component == access[index].component) {
                    return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                                     "the same component was declared more than once");
                }
            }

            // The scheduler reasons in ComponentKeys while iteration needs ComponentIds, so both
            // are carried: the key drives conflict detection, the id drives archetype matching.
            switch (access[index].mode) {
            case STURDY_ACCESS_READ:
                system_access.reads.push_back(descriptor->key);
                break;
            case STURDY_ACCESS_WRITE:
                system_access.writes.push_back(descriptor->key);
                break;
            case STURDY_ACCESS_FORCE_U32:
            default:
                return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized access mode");
            }
            ids.push_back(access[index].component);
        }

        for (uint32_t index = 0; index < resource_access_count; ++index) {
            const SFT::Ecs::ResourceKey key{.high = resource_access[index].resource.high,
                                            .low = resource_access[index].resource.low};
            for (uint32_t other = 0; other < index; ++other) {
                if (resource_access[other].resource.high == resource_access[index].resource.high &&
                    resource_access[other].resource.low == resource_access[index].resource.low) {
                    return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                                     "the same resource was declared more than once");
                }
            }

            // Declared as a plain resource rather than an event even when it happens to be a
            // channel. The scheduler's event bookkeeping additionally orders writers before readers
            // within a frame, which only makes sense for a typed Events<T> whose reader semantics
            // the engine controls; a foreign channel is drained explicitly and reads whatever is
            // buffered, so plain read/write conflict detection is the honest description.
            switch (resource_access[index].mode) {
            case STURDY_ACCESS_READ:
                system_access.resource_reads.push_back(key);
                break;
            case STURDY_ACCESS_WRITE:
                system_access.resource_writes.push_back(key);
                break;
            case STURDY_ACCESS_FORCE_U32:
            default:
                return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized access mode");
            }
        }

        // Freed by sturdy_ecs_remove_system for removable registrations; otherwise it lives as long as
        // the schedule, and the schedule as long as the engine.
        auto *registered = new RegisteredSystem{system, user_data};

        SFT::Ecs::SystemHandle handle;
        if (ids.empty()) {
            handle = resolved_engine->update_schedule().add_erased_global_system(
                std::move(system_access), invoke_system, registered, prepare_dispatch, finish_dispatch);
        } else {
            handle = resolved_engine->update_schedule().add_erased_system(std::move(system_access), std::move(ids),
                                                                          invoke_system, registered,
                                                                          prepare_dispatch, finish_dispatch);
        }
        if (out_engine != nullptr) *out_engine = resolved_engine;
        if (out_handle != nullptr) *out_handle = handle;
        if (out_registered != nullptr) *out_registered = registered;
        return STURDY_OK;
    });
    }

    struct RemovableSystem {
        SFT::Engine::Engine *engine;
        SFT::Ecs::SystemHandle handle;
        RegisteredSystem *registered;
    };
    std::mutex g_systems_mutex;
    std::unordered_map<uint64_t, RemovableSystem> g_systems;
    uint64_t g_next_system_token = 1;

    [[nodiscard]] SturdyResult resolve_system(SturdyEngine engine, SturdySystem system, RemovableSystem *out) noexcept {
        SFT::Engine::Engine *resolved_engine = nullptr;
        if (const SturdyResult resolved = resolve_engine(engine, &resolved_engine); resolved != STURDY_OK) {
            return resolved;
        }
        const std::lock_guard<std::mutex> lock{g_systems_mutex};
        const auto found = g_systems.find(system.token);
        if (found == g_systems.end() || found->second.engine != resolved_engine) {
            return set_error(STURDY_ERROR_INVALID_HANDLE, "no such system is registered on this engine");
        }
        *out = found->second;
        return STURDY_OK;
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL
sturdy_ecs_add_system_with_resources(SturdyEngine engine,
                                     const SturdySystemAccess *access,
                                     uint32_t access_count,
                                     const SturdySystemResourceAccess *resource_access,
                                     uint32_t resource_access_count,
                                     SturdySystemFn system,
                                     void *user_data) {
    return register_system(engine, access, access_count, resource_access, resource_access_count, system, user_data,
                           nullptr, nullptr, nullptr);
}

SturdyResult STURDY_ABI_CALL
sturdy_ecs_add_system_ex(SturdyEngine engine,
                         const SturdySystemAccess *access,
                         uint32_t access_count,
                         const SturdySystemResourceAccess *resource_access,
                         uint32_t resource_access_count,
                         SturdySystemFn system,
                         void *user_data,
                         SturdySystem *out_system) {
    if (out_system == nullptr) {
        return set_error(STURDY_ERROR_INVALID_ARGUMENT, "out_system must not be null");
    }
    SFT::Engine::Engine *resolved_engine = nullptr;
    SFT::Ecs::SystemHandle handle;
    RegisteredSystem *registered = nullptr;
    const SturdyResult result = register_system(engine, access, access_count, resource_access, resource_access_count,
                                                system, user_data, &resolved_engine, &handle, &registered);
    if (result != STURDY_OK) {
        return result;
    }
    return guarded([&]() -> SturdyResult {
        const std::lock_guard<std::mutex> lock{g_systems_mutex};
        const uint64_t token = g_next_system_token++;
        g_systems.emplace(token, RemovableSystem{resolved_engine, handle, registered});
        out_system->token = token;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_remove_system(SturdyEngine engine, SturdySystem system) {
    return guarded([&]() -> SturdyResult {
        RemovableSystem entry{};
        if (const SturdyResult resolved = resolve_system(engine, system, &entry); resolved != STURDY_OK) {
            return resolved;
        }
        if (!entry.engine->update_schedule().remove_system(entry.handle)) {
            return set_error(STURDY_ERROR_INVALID_HANDLE, "the system is no longer registered");
        }
        {
            const std::lock_guard<std::mutex> lock{g_systems_mutex};
            g_systems.erase(system.token);
        }
        // Nothing can be running it any more (removal is refused mid-run), so the dispatch record and
        // whatever user_data the caller owns are safe to release.
        delete entry.registered;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_set_system_enabled(SturdyEngine engine, SturdySystem system, SturdyBool enabled) {
    return guarded([&]() -> SturdyResult {
        RemovableSystem entry{};
        if (const SturdyResult resolved = resolve_system(engine, system, &entry); resolved != STURDY_OK) {
            return resolved;
        }
        (void)entry.engine->update_schedule().set_system_enabled(entry.handle, enabled != STURDY_FALSE);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_order_system_after(SturdyEngine engine, SturdySystem system, SturdySystem dependency) {
    return guarded([&]() -> SturdyResult {
        RemovableSystem entry{};
        RemovableSystem dependency_entry{};
        if (const SturdyResult resolved = resolve_system(engine, system, &entry); resolved != STURDY_OK) {
            return resolved;
        }
        if (const SturdyResult resolved = resolve_system(engine, dependency, &dependency_entry); resolved != STURDY_OK) {
            return resolved;
        }
        if (!entry.engine->update_schedule().order_after(entry.handle, dependency_entry.handle)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "a system cannot be ordered after itself");
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_commands_spawn(SturdyCommands commands,
                                                       const SturdyComponentInit *components,
                                                       uint32_t component_count) {
    return guarded([&]() -> SturdyResult {
        if (components == nullptr || component_count == 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "spawning an entity requires at least one component");
        }

        SFT::Ecs::Commands *queue = nullptr;
        const SturdyResult resolved = resolve_commands(commands, &queue);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        std::vector<SFT::Ecs::ComponentId> ids;
        std::vector<const void *> data;
        std::vector<usize> sizes;
        ids.reserve(component_count);
        data.reserve(component_count);
        sizes.reserve(component_count);
        for (uint32_t index = 0; index < component_count; ++index) {
            if (components[index].data == nullptr) {
                return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                                 "component data pointer must not be null");
            }
            ids.push_back(components[index].component);
            data.push_back(components[index].data);
            sizes.push_back(components[index].size);
        }

        // Sizes are not checked against the registry here: this runs mid-schedule where the
        // registry is being read by other systems, and the deferred apply validates against the
        // real component descriptor anyway.
        queue->spawn_erased(ids, data, sizes);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_commands_destroy(SturdyCommands commands, SturdyEntity entity) {
    return guarded([&]() -> SturdyResult {
        SFT::Ecs::Commands *queue = nullptr;
        const SturdyResult resolved = resolve_commands(commands, &queue);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        queue->destroy(SFT::Ecs::Entity{.index = entity.index, .generation = entity.generation});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_commands_add_component(SturdyCommands commands,
                                                               SturdyEntity entity,
                                                               SturdyComponentId component,
                                                               const void *data,
                                                               uint32_t size) {
    return guarded([&]() -> SturdyResult {
        if (data == nullptr && size != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "component data pointer must not be null");
        }

        SFT::Ecs::Commands *queue = nullptr;
        const SturdyResult resolved = resolve_commands(commands, &queue);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        queue->add_component_erased(SFT::Ecs::Entity{.index = entity.index, .generation = entity.generation},
                                    component, data, size);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_commands_remove_component(SturdyCommands commands,
                                                                  SturdyEntity entity,
                                                                  SturdyComponentId component) {
    return guarded([&]() -> SturdyResult {
        SFT::Ecs::Commands *queue = nullptr;
        const SturdyResult resolved = resolve_commands(commands, &queue);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        queue->remove_component_erased(
            SFT::Ecs::Entity{.index = entity.index, .generation = entity.generation}, component);
        return STURDY_OK;
    });
}

} // extern "C"
