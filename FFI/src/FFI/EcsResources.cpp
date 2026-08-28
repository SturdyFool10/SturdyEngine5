/// C ABI implementation of ECS resources and event channels.
///
/// The world borrows resource storage rather than owning it: `bind_resource` takes a reference and
/// expects whoever bound it to keep it alive. Engine C++ satisfies that by holding the resource as
/// a member somewhere. A foreign caller has no comparable place to put it — its own allocation
/// would have to outlive a boundary the engine knows nothing about — so this layer allocates and
/// owns resource storage itself, keyed by world and resource id, and frees it on destroy.
///
/// Event channels are resources with a drain hook. Binding an `ErasedEvents` with
/// `clear_erased_events` is what makes `ScheduleConfig::clear_events_on_run` empty a foreign
/// channel on the same cadence it empties a typed `Events<T>`; without the hook the buffer would
/// look like an ordinary resource and grow forever.

#include <Foundation/Foundation.hpp>

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <Ecs/ErasedEvents.hpp>
#include <Ecs/World.hpp>
#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::guarded;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::set_error;
    using SFT::usize;

    /// Storage this ABI owns on a caller's behalf.
    ///
    /// Exactly one of the two is engaged. A plain resource is a flat byte buffer; an event channel
    /// is an `ErasedEvents`, which manages its own growth and is drained by the scheduler.
    struct OwnedResource {
        std::unique_ptr<std::byte[]> bytes;
        std::unique_ptr<SFT::Ecs::ErasedEvents> events;
        usize size = 0;
    };

    /// Every resource this ABI owns, keyed by the world it is bound into and its resource key.
    ///
    /// Keyed by world as well as id so two engines in one process — the test harness constructs one
    /// directly while a runtime owns another — cannot collide or free each other's storage.
    struct OwnedKey {
        const SFT::Ecs::World *world;
        SFT::u64 high;
        SFT::u64 low;

        /// Orders keys so they can live in a `std::map`.
        ///
        /// @param other Right-hand operand.
        ///
        /// @return Returns the ordering of the operands.
        /// @note This function does not throw exceptions.
        [[nodiscard]] auto operator<=>(const OwnedKey &other) const noexcept = default;
    };

    std::mutex g_owned_mutex;
    std::map<OwnedKey, OwnedResource> g_owned;

    /// Converts an ABI resource id to the engine's key type.
    ///
    /// @param resource Value received from the caller.
    ///
    /// @return The engine-side key.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SFT::Ecs::ResourceKey to_engine_key(SturdyResourceId resource) noexcept {
        return SFT::Ecs::ResourceKey{.high = resource.high, .low = resource.low};
    }

    /// Resolves an engine handle to its ECS world.
    ///
    /// @param engine Handle supplied to a game-logic callback.
    /// @param out_world Receives the borrowed world on success.
    ///
    /// @return `STURDY_OK`, or the handle failure encountered.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyResult resolve_world(SturdyEngine engine, SFT::Ecs::World **out_world) noexcept {
        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        *out_world = &resolved_engine->ecs_world();
        return STURDY_OK;
    }

    /// Resolves an event channel bound under `resource`.
    ///
    /// @param world World to look in.
    /// @param resource Channel identifier.
    /// @param out_events Receives the borrowed channel on success.
    ///
    /// @return `STURDY_OK`, or why the channel could not be resolved.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyResult resolve_channel(SFT::Ecs::World &world,
                                               SturdyResourceId resource,
                                               SFT::Ecs::ErasedEvents **out_events) noexcept {
        const OwnedKey key{&world, resource.high, resource.low};
        const std::lock_guard<std::mutex> lock{g_owned_mutex};
        const auto found = g_owned.find(key);
        if (found == g_owned.end() || found->second.events == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE,
                             "no event channel is bound under that id");
        }
        *out_events = found->second.events.get();
        return STURDY_OK;
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_ecs_resource_id(const char *name, SturdyResourceId *out_id) {
    return guarded([&]() -> SturdyResult {
        if (name == nullptr || out_id == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name and output pointer must not be null");
        }
        const std::string_view canonical_name{name};
        if (canonical_name.empty()) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "resource name must not be empty");
        }

        const SFT::Ecs::ResourceKey key = SFT::Ecs::ResourceKey::from_name(canonical_name);
        out_id->high = key.high;
        out_id->low = key.low;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_create_resource(SturdyEngine engine,
                                                        const char *name,
                                                        uint32_t size,
                                                        const void *initial_data,
                                                        SturdyResourceId *out_id) {
    return guarded([&]() -> SturdyResult {
        if (name == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name must not be null");
        }
        const std::string_view canonical_name{name};
        if (canonical_name.empty()) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "resource name must not be empty");
        }
        if (size == 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "resource size must be nonzero");
        }

        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const SFT::Ecs::ResourceKey key = SFT::Ecs::ResourceKey::from_name(canonical_name);
        if (out_id != nullptr) {
            out_id->high = key.high;
            out_id->low = key.low;
        }

        const OwnedKey owned_key{world, key.high, key.low};
        void *object = nullptr;
        {
            const std::lock_guard<std::mutex> lock{g_owned_mutex};
            if (const auto existing = g_owned.find(owned_key); existing != g_owned.end()) {
                // Re-creating is expected — a binding may call this on every run. Same size is a
                // no-op; a different size would silently reinterpret whatever already reads it.
                if (existing->second.size != size || existing->second.events != nullptr) {
                    return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                                     "a different resource is already registered under that name");
                }
                return STURDY_OK;
            }

            OwnedResource owned;
            owned.bytes = std::make_unique<std::byte[]>(size);
            owned.size = size;
            if (initial_data != nullptr) {
                std::memcpy(owned.bytes.get(), initial_data, size);
            } else {
                std::memset(owned.bytes.get(), 0, size);
            }
            object = owned.bytes.get();
            g_owned.emplace(owned_key, std::move(owned));
        }

        // Alignment is the strictest the allocator guarantees rather than something the caller
        // chooses: `new std::byte[]` is suitably aligned for any scalar, which covers every layout a
        // C struct can have short of over-aligned SIMD types.
        auto bound = world->bind_resource_erased(key, canonical_name, object, size,
                                                 alignof(std::max_align_t), nullptr);
        if (!bound) {
            const std::lock_guard<std::mutex> lock{g_owned_mutex};
            g_owned.erase(owned_key);
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, bound.error().message.cpp_string_view());
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_destroy_resource(SturdyEngine engine, SturdyResourceId resource) {
    return guarded([&]() -> SturdyResult {
        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const OwnedKey owned_key{world, resource.high, resource.low};
        {
            const std::lock_guard<std::mutex> lock{g_owned_mutex};
            if (g_owned.find(owned_key) == g_owned.end()) {
                // Either nothing is bound, or it is a C++ resource whose storage belongs to the
                // engine. Freeing that would be a use-after-free for its real owner.
                return set_error(STURDY_ERROR_NOT_AVAILABLE,
                                 "no resource owned by this ABI is bound under that id");
            }
        }

        auto unbound = world->unbind_resource_erased(to_engine_key(resource));
        if (!unbound) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, unbound.error().message.cpp_string_view());
        }

        // Freed only after the world has let go of the pointer, so nothing can read it in between.
        const std::lock_guard<std::mutex> lock{g_owned_mutex};
        g_owned.erase(owned_key);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_has_resource(SturdyEngine engine,
                                                     SturdyResourceId resource,
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
        *out_present = world->has_resource_erased(to_engine_key(resource)) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_get_resource(SturdyEngine engine,
                                                     SturdyResourceId resource,
                                                     void *out_data,
                                                     uint32_t size) {
    return guarded([&]() -> SturdyResult {
        if (out_data == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        usize bound_size = 0;
        void *object = world->resource_pointer_erased(to_engine_key(resource), &bound_size);
        if (object == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no resource is bound under that id");
        }
        if (bound_size != size) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "supplied size does not match the resource's size");
        }
        std::memcpy(out_data, object, size);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_set_resource(SturdyEngine engine,
                                                     SturdyResourceId resource,
                                                     const void *data,
                                                     uint32_t size) {
    return guarded([&]() -> SturdyResult {
        if (data == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "data pointer must not be null");
        }

        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        usize bound_size = 0;
        void *object = world->resource_pointer_erased(to_engine_key(resource), &bound_size);
        if (object == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no resource is bound under that id");
        }
        if (bound_size != size) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "supplied size does not match the resource's size");
        }
        std::memcpy(object, data, size);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_create_event_channel(SturdyEngine engine,
                                                             const char *name,
                                                             uint32_t event_size,
                                                             SturdyResourceId *out_id) {
    return guarded([&]() -> SturdyResult {
        if (name == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name must not be null");
        }
        const std::string_view canonical_name{name};
        if (canonical_name.empty()) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "channel name must not be empty");
        }
        if (event_size == 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "event size must be nonzero");
        }

        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const SFT::Ecs::ResourceKey key = SFT::Ecs::ResourceKey::from_name(canonical_name);
        if (out_id != nullptr) {
            out_id->high = key.high;
            out_id->low = key.low;
        }

        const OwnedKey owned_key{world, key.high, key.low};
        void *object = nullptr;
        {
            const std::lock_guard<std::mutex> lock{g_owned_mutex};
            if (const auto existing = g_owned.find(owned_key); existing != g_owned.end()) {
                if (existing->second.events == nullptr ||
                    existing->second.events->element_size() != event_size) {
                    return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                                     "a different resource is already registered under that name");
                }
                return STURDY_OK;
            }

            OwnedResource owned;
            owned.events = std::make_unique<SFT::Ecs::ErasedEvents>(event_size);
            owned.size = sizeof(SFT::Ecs::ErasedEvents);
            object = owned.events.get();
            g_owned.emplace(owned_key, std::move(owned));
        }

        // The clear hook is what distinguishes an event channel from a plain resource: it is how
        // the scheduler knows to drain this buffer between frames.
        auto bound = world->bind_resource_erased(key, canonical_name, object,
                                                 sizeof(SFT::Ecs::ErasedEvents),
                                                 alignof(SFT::Ecs::ErasedEvents),
                                                 SFT::Ecs::clear_erased_events);
        if (!bound) {
            const std::lock_guard<std::mutex> lock{g_owned_mutex};
            g_owned.erase(owned_key);
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, bound.error().message.cpp_string_view());
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_send_event(SturdyEngine engine,
                                                   SturdyResourceId channel,
                                                   const void *event,
                                                   uint32_t size) {
    return guarded([&]() -> SturdyResult {
        if (event == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "event pointer must not be null");
        }

        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        SFT::Ecs::ErasedEvents *events = nullptr;
        const SturdyResult found = resolve_channel(*world, channel, &events);
        if (found != STURDY_OK) {
            return found;
        }
        if (events->element_size() != size) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "supplied size does not match the channel's event size");
        }
        events->send(event);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_event_count(SturdyEngine engine,
                                                    SturdyResourceId channel,
                                                    uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (out_count == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        SFT::Ecs::ErasedEvents *events = nullptr;
        const SturdyResult found = resolve_channel(*world, channel, &events);
        if (found != STURDY_OK) {
            return found;
        }
        *out_count = static_cast<uint32_t>(events->size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_read_event(SturdyEngine engine,
                                                   SturdyResourceId channel,
                                                   uint32_t index,
                                                   void *out_event,
                                                   uint32_t size) {
    return guarded([&]() -> SturdyResult {
        if (out_event == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        SFT::Ecs::ErasedEvents *events = nullptr;
        const SturdyResult found = resolve_channel(*world, channel, &events);
        if (found != STURDY_OK) {
            return found;
        }
        if (events->element_size() != size) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "supplied size does not match the channel's event size");
        }

        const void *stored = events->at(index);
        if (stored == nullptr) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "event index is out of range");
        }
        std::memcpy(out_event, stored, size);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_clear_events(SturdyEngine engine, SturdyResourceId channel) {
    return guarded([&]() -> SturdyResult {
        SFT::Ecs::World *world = nullptr;
        const SturdyResult resolved = resolve_world(engine, &world);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        SFT::Ecs::ErasedEvents *events = nullptr;
        const SturdyResult found = resolve_channel(*world, channel, &events);
        if (found != STURDY_OK) {
            return found;
        }
        events->clear();
        return STURDY_OK;
    });
}

} // extern "C"
