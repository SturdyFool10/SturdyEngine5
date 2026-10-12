/// C ABI implementation of ECS resources and event channels.
///
/// The world borrows resource storage rather than owning it: `bind_resource` takes a reference and
/// expects whoever bound it to keep it alive. A foreign caller has no comparable place to put it, so
/// the engine allocates and owns the storage (`Engine/ForeignResources.hpp`, shared with CxxApi so
/// every binding behaves identically) and this layer only translates arguments and error classes.
///
/// Event channels are resources with a drain hook. Binding an `ErasedEvents` with
/// `clear_erased_events` is what makes `ScheduleConfig::clear_events_on_run` empty a foreign
/// channel on the same cadence it empties a typed `Events<T>`; without the hook the buffer would
/// look like an ordinary resource and grow forever.

#include <Foundation/Foundation.hpp>

#include <string_view>
#include <utility>

#include <Ecs/World.hpp>
#include <Engine/Engine.hpp>
#include <Engine/ForeignResources.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::guarded;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::set_error;
    namespace E = SFT::Engine;

    /// Converts an ABI resource id to the engine's key type.
    [[nodiscard]] SFT::Ecs::ResourceKey to_engine_key(SturdyResourceId resource) noexcept {
        return SFT::Ecs::ResourceKey{.high = resource.high, .low = resource.low};
    }

    /// Resolves an engine handle to its ECS world.
    [[nodiscard]] SturdyResult resolve_world(SturdyEngine engine, SFT::Ecs::World **out_world) noexcept {
        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        *out_world = &resolved_engine->ecs_world();
        return STURDY_OK;
    }

    /// Maps the shared store's error class onto this ABI's result codes.
    [[nodiscard]] SturdyResult to_result(const E::ForeignResourceError &failure) {
        switch (failure.kind) {
            case E::ForeignResourceErrorKind::NotAvailable: return set_error(STURDY_ERROR_NOT_AVAILABLE, failure.message);
            case E::ForeignResourceErrorKind::OutOfRange: return set_error(STURDY_ERROR_OUT_OF_RANGE, failure.message);
            case E::ForeignResourceErrorKind::InvalidArgument: break;
        }
        return set_error(STURDY_ERROR_INVALID_ARGUMENT, failure.message);
    }

    /// Reads a `SturdyComponentDesc` into a schema, or reports why it cannot be.
    [[nodiscard]] SturdyResult read_desc(const SturdyComponentDesc *desc, SFT::Ecs::ComponentSchema *out_schema) {
        if (desc == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc must not be null");
        }
        if (desc->struct_size != sizeof(SturdyComponentDesc)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyComponentDesc size does not match this engine build");
        }
        if (desc->field_count != 0 && desc->fields == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "fields must not be null when field_count is nonzero");
        }
        for (uint32_t i = 0; i < desc->field_count; ++i) {
            const SturdyComponentField &field = desc->fields[i];
            if (field.struct_size != sizeof(SturdyComponentField)) {
                return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyComponentField size does not match this engine build");
            }
            out_schema->fields.push_back(SFT::Ecs::ComponentField{.name = field.name != nullptr ? field.name : "",
                                                                  .kind = static_cast<SFT::Ecs::FieldKind>(field.kind),
                                                                  .offset = field.offset,
                                                                  .count = field.count});
        }
        return STURDY_OK;
    }

    void store_id(SturdyResourceId *out_id, SFT::Ecs::ResourceKey key) noexcept {
        if (out_id != nullptr) {
            out_id->high = key.high;
            out_id->low = key.low;
        }
    }

    [[nodiscard]] SturdyResult create_resource_impl(SturdyEngine engine, const char *name, uint32_t size, const void *initial_data,
                                                    const SFT::Ecs::ComponentSchema *schema, SturdyResourceId *out_id) {
        return guarded([&]() -> SturdyResult {
            if (name == nullptr) {
                return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name must not be null");
            }
            // Bad arguments are reported before the engine handle is consulted.
            if (const auto invalid = E::validate_foreign_resource(name, size, schema)) {
                return set_error(STURDY_ERROR_INVALID_ARGUMENT, *invalid);
            }
            SFT::Ecs::World *world = nullptr;
            if (const SturdyResult resolved = resolve_world(engine, &world); resolved != STURDY_OK) {
                return resolved;
            }
            // The id is reported even when the resource already exists or conflicts, as before.
            store_id(out_id, SFT::Ecs::ResourceKey::from_name(name));
            const auto created = E::create_foreign_resource(*world, name, size, initial_data, schema);
            return created ? STURDY_OK : to_result(created.error());
        });
    }

    [[nodiscard]] SturdyResult create_event_channel_impl(SturdyEngine engine, const char *name, uint32_t event_size,
                                                         const SFT::Ecs::ComponentSchema *schema, SturdyResourceId *out_id) {
        return guarded([&]() -> SturdyResult {
            if (name == nullptr) {
                return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name must not be null");
            }
            if (const auto invalid = E::validate_foreign_event_channel(name, event_size, schema)) {
                return set_error(STURDY_ERROR_INVALID_ARGUMENT, *invalid);
            }
            SFT::Ecs::World *world = nullptr;
            if (const SturdyResult resolved = resolve_world(engine, &world); resolved != STURDY_OK) {
                return resolved;
            }
            store_id(out_id, SFT::Ecs::ResourceKey::from_name(name));
            const auto created = E::create_foreign_event_channel(*world, name, event_size, schema);
            return created ? STURDY_OK : to_result(created.error());
        });
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
        store_id(out_id, SFT::Ecs::ResourceKey::from_name(canonical_name));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_create_resource(SturdyEngine engine,
                                                        const char *name,
                                                        uint32_t size,
                                                        const void *initial_data,
                                                        SturdyResourceId *out_id) {
    return create_resource_impl(engine, name, size, initial_data, nullptr, out_id);
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_create_resource_ex(SturdyEngine engine,
                                                           const SturdyComponentDesc *desc,
                                                           const void *initial_data,
                                                           SturdyResourceId *out_id) {
    return guarded([&]() -> SturdyResult {
        SFT::Ecs::ComponentSchema schema;
        if (const SturdyResult read = read_desc(desc, &schema); read != STURDY_OK) {
            return read;
        }
        return create_resource_impl(engine, desc->name, desc->size, initial_data, &schema, out_id);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_destroy_resource(SturdyEngine engine, SturdyResourceId resource) {
    return guarded([&]() -> SturdyResult {
        SFT::Ecs::World *world = nullptr;
        if (const SturdyResult resolved = resolve_world(engine, &world); resolved != STURDY_OK) {
            return resolved;
        }
        const auto destroyed = E::destroy_foreign_resource(*world, to_engine_key(resource));
        return destroyed ? STURDY_OK : to_result(destroyed.error());
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
        if (const SturdyResult resolved = resolve_world(engine, &world); resolved != STURDY_OK) {
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
        if (const SturdyResult resolved = resolve_world(engine, &world); resolved != STURDY_OK) {
            return resolved;
        }
        const auto read = E::read_resource(*world, to_engine_key(resource), out_data, size);
        return read ? STURDY_OK : to_result(read.error());
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
        if (const SturdyResult resolved = resolve_world(engine, &world); resolved != STURDY_OK) {
            return resolved;
        }
        const auto written = E::write_resource(*world, to_engine_key(resource), data, size);
        return written ? STURDY_OK : to_result(written.error());
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_create_event_channel(SturdyEngine engine,
                                                             const char *name,
                                                             uint32_t event_size,
                                                             SturdyResourceId *out_id) {
    return create_event_channel_impl(engine, name, event_size, nullptr, out_id);
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_create_event_channel_ex(SturdyEngine engine,
                                                                const SturdyComponentDesc *desc,
                                                                SturdyResourceId *out_id) {
    return guarded([&]() -> SturdyResult {
        SFT::Ecs::ComponentSchema schema;
        if (const SturdyResult read = read_desc(desc, &schema); read != STURDY_OK) {
            return read;
        }
        return create_event_channel_impl(engine, desc->name, desc->size, &schema, out_id);
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
        if (const SturdyResult resolved = resolve_world(engine, &world); resolved != STURDY_OK) {
            return resolved;
        }
        const auto sent = E::send_foreign_event(*world, to_engine_key(channel), event, size);
        return sent ? STURDY_OK : to_result(sent.error());
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
        if (const SturdyResult resolved = resolve_world(engine, &world); resolved != STURDY_OK) {
            return resolved;
        }
        const auto count = E::foreign_event_count(*world, to_engine_key(channel));
        if (!count) {
            return to_result(count.error());
        }
        *out_count = *count;
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
        if (const SturdyResult resolved = resolve_world(engine, &world); resolved != STURDY_OK) {
            return resolved;
        }
        const auto read = E::read_foreign_event(*world, to_engine_key(channel), index, out_event, size);
        return read ? STURDY_OK : to_result(read.error());
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ecs_clear_events(SturdyEngine engine, SturdyResourceId channel) {
    return guarded([&]() -> SturdyResult {
        SFT::Ecs::World *world = nullptr;
        if (const SturdyResult resolved = resolve_world(engine, &world); resolved != STURDY_OK) {
            return resolved;
        }
        const auto cleared = E::clear_foreign_events(*world, to_engine_key(channel));
        return cleared ? STURDY_OK : to_result(cleared.error());
    });
}

} // extern "C"
