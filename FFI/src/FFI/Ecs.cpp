/// C ABI implementation of the ECS surface.
///
/// Everything here routes through `World`'s type-erased API rather than its templated one. That is
/// not a convenience: the templated API resolves components from C++ types this caller does not
/// have, and treats a dead entity or duplicate component as a contract violation that calls
/// `std::terminate`. A foreign caller holding an entity from a previous frame would take the
/// process down. The erased API reports those conditions instead, and this layer maps them onto
/// result codes a binding can branch on.
///
/// The other thing this layer owns is reentrancy. `World::for_each_erased` holds the world's
/// direct-access lock for the duration of the visit, and the engine treats any structural change
/// made while that lock is held as a contract violation — verified by removing the guard below, at
/// which point a visitor calling `sturdy_ecs_spawn` terminates the process with "Cannot spawn
/// entities while a direct ECS query or component borrow is active". A thread-local guard turns
/// that into `STURDY_ERROR_BUSY` instead: the callback gets direct pointers for the reads and
/// writes it actually needs, and structural changes wait until iteration is done.

#include <Foundation/Foundation.hpp>

#include <cstring>
#include <string_view>
#include <vector>

#include <Ecs/World.hpp>
#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::copy_string_out;
    using SFT::Ffi::guarded;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::set_error;
    using SFT::u32;
    using SFT::usize;

    /// Whether this thread is inside a `sturdy_ecs_for_each` visit.
    ///
    /// Thread-local rather than global: two threads may legitimately iterate different parts of the
    /// world at once under the shared lock, and only reentrancy on the *same* thread is the
    /// deadlock hazard.
    thread_local bool g_iterating = false;

    /// Guards the iteration flag so it is cleared even if the visit exits early.
    class IterationGuard {
      public:
        /// Marks this thread as iterating.
        ///
        /// @note This function does not throw exceptions.
        IterationGuard() noexcept { g_iterating = true; }

        /// Clears the iterating mark.
        ///
        /// @note This function does not throw exceptions.
        ~IterationGuard() { g_iterating = false; }

        /// Disables this construction form for `IterationGuard`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        IterationGuard(const IterationGuard &) = delete;
        /// Assigns a new value to this `IterationGuard`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        IterationGuard &operator=(const IterationGuard &) = delete;
    };

    /// Rejects a call made from inside a visitor callback.
    ///
    /// @return `STURDY_OK` when the call may proceed, `STURDY_ERROR_BUSY` otherwise.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyResult reject_if_iterating() noexcept {
        // Without this the engine's own reentrancy check fires instead, and that one terminates
        // the process rather than returning a status a foreign caller could handle.
        if (g_iterating) {
            return set_error(STURDY_ERROR_BUSY,
                             "the world is locked for iteration; read and write through the pointers "
                             "the visitor was given, and make structural changes after it returns");
        }
        return STURDY_OK;
    }

    /// Translates an erased-world failure into its ABI result code.
    ///
    /// @param error Engine-side failure.
    ///
    /// @return The mapped result, with the engine's own message recorded as the error detail.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyResult translate_error(const SFT::Ecs::WorldErasedError &error) noexcept {
        SturdyResult result = STURDY_ERROR_INVALID_ARGUMENT;
        switch (error.code) {
        case SFT::Ecs::WorldErasedErrorCode::DeadEntity:
            result = STURDY_ERROR_ENTITY_NOT_ALIVE;
            break;
        case SFT::Ecs::WorldErasedErrorCode::MissingComponent:
            result = STURDY_ERROR_COMPONENT_MISSING;
            break;
        case SFT::Ecs::WorldErasedErrorCode::DuplicateComponent:
            result = STURDY_ERROR_COMPONENT_PRESENT;
            break;
        case SFT::Ecs::WorldErasedErrorCode::NotTriviallyCopyable:
            result = STURDY_ERROR_COMPONENT_NOT_BLITTABLE;
            break;
        case SFT::Ecs::WorldErasedErrorCode::ScheduleRunning:
            result = STURDY_ERROR_BUSY;
            break;
        case SFT::Ecs::WorldErasedErrorCode::UnknownComponent:
        case SFT::Ecs::WorldErasedErrorCode::SizeMismatch:
        case SFT::Ecs::WorldErasedErrorCode::NoComponents:
        case SFT::Ecs::WorldErasedErrorCode::InvalidArgument:
        default:
            result = STURDY_ERROR_INVALID_ARGUMENT;
            break;
        }
        return set_error(result, error.message.cpp_string_view());
    }

    /// Resolves an engine handle to its ECS world, refusing while a visit is in progress.
    ///
    /// @param engine Handle supplied to a game-logic callback.
    /// @param out_world Receives the borrowed world on success.
    ///
    /// @return `STURDY_OK`, or the handle/reentrancy failure encountered.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyResult resolve_world(SturdyEngine engine, SFT::Ecs::World **out_world) noexcept {
        if (const SturdyResult busy = reject_if_iterating(); busy != STURDY_OK) {
            return busy;
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        *out_world = &resolved_engine->ecs_world();
        return STURDY_OK;
    }

    /// Converts an ABI entity to its engine representation.
    ///
    /// @param entity Value received from the caller.
    ///
    /// @return The engine-side entity.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SFT::Ecs::Entity to_engine_entity(SturdyEntity entity) noexcept {
        return SFT::Ecs::Entity{.index = entity.index, .generation = entity.generation};
    }

    /// Converts an engine entity to its ABI representation.
    ///
    /// @param entity Engine-side entity.
    ///
    /// @return The ABI entity.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyEntity to_abi_entity(SFT::Ecs::Entity entity) noexcept {
        SturdyEntity result;
        result.index = entity.index;
        result.generation = entity.generation;
        return result;
    }

    /// Context threaded through `World::for_each_erased` so the engine-side visitor can forward to
    /// the caller's C function pointer.
    struct VisitContext {
        SturdyEcsVisitFn visit;
        void *user_data;
    };

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_ecs_register_component(SturdyEngine engine,
                                                           const char *name,
                                                           uint32_t size,
                                                           uint32_t align,
                                                           SturdyComponentId *out_component) {
    return guarded([&]() -> SturdyResult {
        if (name == nullptr || out_component == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name and output pointer must not be null");
        }
        const std::string_view canonical_name{name};
        if (canonical_name.empty()) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "component name must not be empty");
        }
        // Zero-sized components would need archetype columns with no storage, a shape the rest of
        // the ECS does not currently produce. Refusing is better than being the first to create it.
        if (size == 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "component size must be nonzero; tag components are not supported yet");
        }
        if (align == 0 || (align & (align - 1)) != 0 || align > 64) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "component alignment must be a power of two no greater than 64");
        }

        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        SFT::Ecs::ComponentRegistry &registry = world->registry();

        // Registering the same component twice is expected — a binding will call this on every run,
        // and possibly from more than one place. Returning the existing id keeps that idempotent,
        // but only when the layout matches: a different size or alignment under the same name means
        // live entities are already laid out the other way.
        if (const auto existing = registry.find(SFT::ustr{canonical_name})) {
            const SFT::Ecs::ComponentInfo *descriptor = registry.info(*existing);
            if (descriptor != nullptr &&
                (descriptor->size != size || descriptor->align != align)) {
                return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                                 "a component with this name is already registered with a different layout");
            }
            *out_component = *existing;
            return STURDY_OK;
        }

        SFT::Ecs::ComponentInfo info{};
        info.key = SFT::Ecs::ComponentKey::from_name(canonical_name);
        info.canonical_name = SFT::UString{std::string{canonical_name}};
        info.schema_version = 1;
        info.size = size;
        info.align = align;
        // FfiBlittable records where this component came from; the trivially-* flags are what the
        // erased World API actually checks before copying bytes.
        info.flags = SFT::Ecs::ComponentFlags::TriviallyCopyable |
                     SFT::Ecs::ComponentFlags::TriviallyDestructible |
                     SFT::Ecs::ComponentFlags::FfiBlittable;
        info.default_construct = [](void *destination, void *user_data) noexcept {
            // Size travels through user_data because these are plain function pointers with no
            // capture; zeroing gives a foreign component a defined initial value.
            std::memset(destination, 0, reinterpret_cast<usize>(user_data));
        };
        info.copy_construct = [](void *destination, const void *source, void *user_data) noexcept {
            std::memcpy(destination, source, reinterpret_cast<usize>(user_data));
        };
        info.move_construct = [](void *destination, void *source, void *user_data) noexcept {
            std::memcpy(destination, source, reinterpret_cast<usize>(user_data));
        };
        info.destroy = [](void *, void *) noexcept {};
        info.user_data = reinterpret_cast<void *>(static_cast<usize>(size));

        auto registered = registry.register_component(std::move(info));
        if (!registered) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, registered.error().message.cpp_string_view());
        }
        *out_component = *registered;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_find_component(SturdyEngine engine,
                                                       const char *name,
                                                       SturdyComponentId *out_component) {
    return guarded([&]() -> SturdyResult {
        if (name == nullptr || out_component == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name and output pointer must not be null");
        }

        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const auto found = world->registry().find(SFT::ustr{std::string_view{name}});
        if (!found) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no component is registered under that name");
        }
        *out_component = *found;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_component_info(SturdyEngine engine,
                                                       SturdyComponentId component,
                                                       SturdyComponentInfo *out_info) {
    return guarded([&]() -> SturdyResult {
        if (out_info == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const SFT::Ecs::ComponentInfo *descriptor = world->registry().info(component);
        if (descriptor == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "no component is registered under that id");
        }

        *out_info = SturdyComponentInfo{};
        out_info->struct_size = static_cast<uint32_t>(sizeof(SturdyComponentInfo));
        out_info->component = component;
        out_info->size = static_cast<uint32_t>(descriptor->size);
        out_info->align = static_cast<uint32_t>(descriptor->align);
        out_info->schema_version = descriptor->schema_version;
        out_info->blittable =
            has_flag(descriptor->flags, SFT::Ecs::ComponentFlags::TriviallyCopyable) ? STURDY_TRUE
                                                                                     : STURDY_FALSE;
        out_info->is_tag =
            has_flag(descriptor->flags, SFT::Ecs::ComponentFlags::Tag) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_component_name(SturdyEngine engine,
                                                       SturdyComponentId component,
                                                       char *buffer,
                                                       size_t capacity,
                                                       size_t *out_length) {
    return guarded([&]() -> SturdyResult {
        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const SFT::Ecs::ComponentInfo *descriptor = world->registry().info(component);
        if (descriptor == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "no component is registered under that id");
        }
        return copy_string_out(descriptor->canonical_name.cpp_string_view(), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_spawn(SturdyEngine engine,
                                              const SturdyComponentInit *components,
                                              uint32_t component_count,
                                              SturdyEntity *out_entity) {
    return guarded([&]() -> SturdyResult {
        if (out_entity == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        if (components == nullptr || component_count == 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "spawning an entity requires at least one component");
        }

        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        // Sizes are checked here rather than in the erased World API: the ABI carries a per-entry
        // size precisely so a layout disagreement between a binding and the engine is caught before
        // any bytes are copied.
        std::vector<SFT::Ecs::ComponentId> ids;
        std::vector<const void *> data;
        ids.reserve(component_count);
        data.reserve(component_count);
        for (uint32_t index = 0; index < component_count; ++index) {
            const SFT::Ecs::ComponentInfo *descriptor = world->registry().info(components[index].component);
            if (descriptor == nullptr) {
                return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                                 "no component is registered under one of the supplied ids");
            }
            if (components[index].size != descriptor->size) {
                return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                                 "a supplied component size does not match its registered size");
            }
            ids.push_back(components[index].component);
            data.push_back(components[index].data);
        }

        auto spawned = world->spawn_erased(ids, data);
        if (!spawned) {
            return translate_error(spawned.error());
        }
        *out_entity = to_abi_entity(*spawned);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_destroy(SturdyEngine engine, SturdyEntity entity) {
    return guarded([&]() -> SturdyResult {
        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        // Already-dead entities are deliberately not an error: destroy is the one operation a
        // caller may reasonably invoke without knowing whether it already happened.
        world->destroy(to_engine_entity(entity));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_is_alive(SturdyEngine engine,
                                                 SturdyEntity entity,
                                                 SturdyBool *out_alive) {
    return guarded([&]() -> SturdyResult {
        if (out_alive == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        *out_alive = world->is_alive(to_engine_entity(entity)) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_has_component(SturdyEngine engine,
                                                      SturdyEntity entity,
                                                      SturdyComponentId component,
                                                      SturdyBool *out_present) {
    return guarded([&]() -> SturdyResult {
        if (out_present == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        *out_present = world->has_component_erased(to_engine_entity(entity), component) ? STURDY_TRUE
                                                                                        : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_add_component(SturdyEngine engine,
                                                      SturdyEntity entity,
                                                      SturdyComponentId component,
                                                      const void *data,
                                                      uint32_t size) {
    return guarded([&]() -> SturdyResult {
        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const SFT::Ecs::ComponentInfo *descriptor = world->registry().info(component);
        if (descriptor == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "no component is registered under that id");
        }
        if (size != descriptor->size) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "supplied size does not match the component's registered size");
        }

        auto added = world->add_component_erased(to_engine_entity(entity), component, data);
        if (!added) {
            return translate_error(added.error());
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_remove_component(SturdyEngine engine,
                                                         SturdyEntity entity,
                                                         SturdyComponentId component) {
    return guarded([&]() -> SturdyResult {
        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        auto removed = world->remove_component_erased(to_engine_entity(entity), component);
        if (!removed) {
            return translate_error(removed.error());
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_get_component(SturdyEngine engine,
                                                      SturdyEntity entity,
                                                      SturdyComponentId component,
                                                      void *out_data,
                                                      uint32_t size) {
    return guarded([&]() -> SturdyResult {
        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        auto read = world->read_component_erased(to_engine_entity(entity), component, out_data, size);
        if (!read) {
            return translate_error(read.error());
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_set_component(SturdyEngine engine,
                                                      SturdyEntity entity,
                                                      SturdyComponentId component,
                                                      const void *data,
                                                      uint32_t size) {
    return guarded([&]() -> SturdyResult {
        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        auto written = world->write_component_erased(to_engine_entity(entity), component, data, size);
        if (!written) {
            return translate_error(written.error());
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_for_each(SturdyEngine engine,
                                                 const SturdyComponentId *components,
                                                 uint32_t component_count,
                                                 SturdyEcsVisitFn visit,
                                                 void *user_data,
                                                 uint32_t *out_visited) {
    return guarded([&]() -> SturdyResult {
        if (visit == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "visitor must not be null");
        }
        if (components == nullptr || component_count == 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "iteration requires at least one component to match on");
        }

        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        VisitContext context{visit, user_data};
        const std::span<const SFT::Ecs::ComponentId> ids{components, component_count};

        // The guard is taken around the call rather than inside the visitor so that it also covers
        // the engine-side setup, and is released even if for_each_erased exits early.
        const IterationGuard guard;
        auto visited = world->for_each_erased(
            ids,
            [](SFT::Ecs::Entity entity, void **pointers, void *context_pointer) noexcept {
                auto *visit_context = static_cast<VisitContext *>(context_pointer);
                visit_context->visit(to_abi_entity(entity), pointers, visit_context->user_data);
            },
            &context);

        if (!visited) {
            return translate_error(visited.error());
        }
        if (out_visited != nullptr) {
            *out_visited = static_cast<uint32_t>(*visited);
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_register_tag_component(SturdyEngine engine,
                                                               const char *name,
                                                               SturdyComponentId *out_component) {
    // A tag is a one-byte component carrying the Tag flag. One byte rather than zero because that
    // is exactly what an empty struct occupies in C++ — verified against the archetype, which gives
    // each row its own distinct byte rather than aliasing them — so a tag registered here is laid
    // out identically to one declared with SFT_ECS_COMPONENT on an empty struct.
    const SturdyResult registered = sturdy_ecs_register_component(engine, name, 1, 1, out_component);
    if (registered != STURDY_OK) {
        return registered;
    }
    return guarded([&]() -> SturdyResult {
        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        world->registry().mark_component_as_tag(*out_component);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_add_tag(SturdyEngine engine,
                                                SturdyEntity entity,
                                                SturdyComponentId component) {
    // The byte is zero rather than uninitialized so two entities carrying the same tag compare
    // equal if anything ever does compare them.
    const uint8_t marker = 0;
    return sturdy_ecs_add_component(engine, entity, component, &marker, 1);
}

} // extern "C"
