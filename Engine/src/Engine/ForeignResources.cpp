#include <Engine/ForeignResources.hpp>

#include <cstddef>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>

#include <Ecs/ErasedEvents.hpp>
#include <Engine/ModuleLedger.hpp>

namespace SFT::Engine {

    namespace {

        /// Storage this module owns on a caller's behalf. Exactly one of `bytes` / `events` is engaged: a plain resource is a flat
        /// byte buffer; an event channel is an `ErasedEvents`, which manages its own growth and is drained by the scheduler.
        struct OwnedResource {
            std::unique_ptr<std::byte[]> bytes;
            std::unique_ptr<Ecs::ErasedEvents> events;
            usize size = 0;
            /// The field layout the creator described, if any (fingerprinted at alignment 1).
            Ecs::ComponentSchema schema;
            Ecs::LayoutFingerprint layout{};
        };

        /// Keyed by world as well as resource key, so two engines in one process (the test harness builds one directly while a
        /// runtime owns another) cannot collide or free each other's storage.
        struct OwnedKey {
            const Ecs::World *world;
            u64 high;
            u64 low;
            [[nodiscard]] auto operator<=>(const OwnedKey &other) const noexcept = default;
        };

        std::mutex g_owned_mutex;
        std::map<OwnedKey, OwnedResource> g_owned;

        [[nodiscard]] std::unexpected<ForeignResourceError> error(ForeignResourceErrorKind kind, std::string message) {
            Foundation::note_trace_origin();
            return std::unexpected(ForeignResourceError{.kind = kind, .message = std::move(message)});
        }

        [[nodiscard]] OwnedKey owned_key(const Ecs::World &world, Ecs::ResourceKey key) noexcept {
            return OwnedKey{&world, key.high, key.low};
        }

        [[nodiscard]] std::optional<std::string> check_fields(std::string_view what, std::string_view name, usize size,
                                                              const Ecs::ComponentSchema *schema) {
            if (schema == nullptr || schema->fields.empty()) {
                return std::nullopt;
            }
            if (const auto reason = Ecs::validate_component_fields(size, schema->fields)) {
                return std::format("invalid field layout for {} '{}': {}", what, name, *reason);
            }
            return std::nullopt;
        }

        /// Why a re-creation conflicts with what was recorded, or nothing. See the header for what must agree.
        [[nodiscard]] std::optional<std::string> layout_conflict(const OwnedResource &existing, usize size, const Ecs::ComponentSchema *schema,
                                                                 std::string_view name) {
            if (schema == nullptr || schema->fields.empty() || !existing.layout) {
                return std::nullopt;
            }
            if (Ecs::fingerprint_component_layout(size, 1, schema->fields) == existing.layout) {
                return std::nullopt;
            }
            return std::format("'{}' is already registered with a different field layout: {}", name,
                               Ecs::describe_layout_difference(size, 1, existing.schema.fields, size, 1, schema->fields));
        }

        [[nodiscard]] ForeignResourceResult<Ecs::ErasedEvents *> channel(const Ecs::World &world, Ecs::ResourceKey key) {
            const std::lock_guard<std::mutex> lock{g_owned_mutex};
            const auto found = g_owned.find(owned_key(world, key));
            if (found == g_owned.end() || found->second.events == nullptr) {
                return error(ForeignResourceErrorKind::NotAvailable, "no event channel is bound under that id");
            }
            return found->second.events.get();
        }

    } // namespace

    std::optional<std::string> validate_foreign_resource(std::string_view name, u32 size, const Ecs::ComponentSchema *schema) {
        if (name.empty()) {
            return std::string{"resource name must not be empty"};
        }
        if (size == 0) {
            return std::string{"resource size must be nonzero"};
        }
        return check_fields("resource", name, size, schema);
    }

    std::optional<std::string> validate_foreign_event_channel(std::string_view name, u32 event_size, const Ecs::ComponentSchema *schema) {
        if (name.empty()) {
            return std::string{"channel name must not be empty"};
        }
        if (event_size == 0) {
            return std::string{"event size must be nonzero"};
        }
        return check_fields("event channel", name, event_size, schema);
    }

    ForeignResourceResult<Ecs::ResourceKey> create_foreign_resource(Ecs::World &world, std::string_view name, u32 size, const void *initial,
                                                                    const Ecs::ComponentSchema *schema) {
        if (auto invalid = validate_foreign_resource(name, size, schema)) {
            return error(ForeignResourceErrorKind::InvalidArgument, std::move(*invalid));
        }
        const Ecs::ResourceKey key = Ecs::ResourceKey::from_name(name);
        const OwnedKey owned = owned_key(world, key);
        void *object = nullptr;
        {
            const std::lock_guard<std::mutex> lock{g_owned_mutex};
            if (const auto existing = g_owned.find(owned); existing != g_owned.end()) {
                // Re-creating is expected: a binding may call this on every run. Same size is a no-op; a different size would
                // silently reinterpret whatever already reads it.
                if (existing->second.size != size || existing->second.events != nullptr) {
                    return error(ForeignResourceErrorKind::InvalidArgument, "a different resource is already registered under that name");
                }
                // Same size is not the same data: two languages must also agree on what is in it.
                if (const auto conflict = layout_conflict(existing->second, size, schema, name)) {
                    return error(ForeignResourceErrorKind::InvalidArgument, *conflict);
                }
                return key;
            }

            OwnedResource resource;
            resource.bytes = std::make_unique<std::byte[]>(size);
            resource.size = size;
            if (schema != nullptr && !schema->fields.empty()) {
                resource.schema = *schema;
                resource.layout = Ecs::fingerprint_component_layout(size, 1, schema->fields);
            }
            if (initial != nullptr) {
                std::memcpy(resource.bytes.get(), initial, size);
            } else {
                std::memset(resource.bytes.get(), 0, size);
            }
            object = resource.bytes.get();
            g_owned.emplace(owned, std::move(resource));
        }

        // Alignment is the strictest the allocator guarantees rather than something the caller chooses: `new std::byte[]` is
        // suitably aligned for any scalar, which covers every layout a C struct can have short of over-aligned SIMD types.
        auto bound = world.bind_resource_erased(key, name, object, size, alignof(std::max_align_t), nullptr);
        if (!bound) {
            const std::lock_guard<std::mutex> lock{g_owned_mutex};
            g_owned.erase(owned);
            return error(ForeignResourceErrorKind::InvalidArgument, bound.error().message.cpp_string());
        }
        // A module that creates a resource owns it: it is destroyed when the module is unloaded (already gone is fine).
        ModuleLedger::note(world, [&world, key] { (void)destroy_foreign_resource(world, key); });
        return key;
    }

    ForeignResourceResult<Ecs::ResourceKey> create_foreign_event_channel(Ecs::World &world, std::string_view name, u32 event_size,
                                                                         const Ecs::ComponentSchema *schema) {
        if (auto invalid = validate_foreign_event_channel(name, event_size, schema)) {
            return error(ForeignResourceErrorKind::InvalidArgument, std::move(*invalid));
        }
        const Ecs::ResourceKey key = Ecs::ResourceKey::from_name(name);
        const OwnedKey owned = owned_key(world, key);
        void *object = nullptr;
        {
            const std::lock_guard<std::mutex> lock{g_owned_mutex};
            if (const auto existing = g_owned.find(owned); existing != g_owned.end()) {
                if (existing->second.events == nullptr || existing->second.events->element_size() != event_size) {
                    return error(ForeignResourceErrorKind::InvalidArgument, "a different resource is already registered under that name");
                }
                // An event channel is how two languages talk: they must agree on the payload, not just its size.
                if (const auto conflict = layout_conflict(existing->second, event_size, schema, name)) {
                    return error(ForeignResourceErrorKind::InvalidArgument, *conflict);
                }
                return key;
            }

            OwnedResource resource;
            resource.events = std::make_unique<Ecs::ErasedEvents>(event_size);
            resource.size = sizeof(Ecs::ErasedEvents);
            if (schema != nullptr && !schema->fields.empty()) {
                resource.schema = *schema;
                resource.layout = Ecs::fingerprint_component_layout(event_size, 1, schema->fields);
            }
            object = resource.events.get();
            g_owned.emplace(owned, std::move(resource));
        }

        // The clear hook is what distinguishes an event channel from a plain resource: it is how the scheduler knows to drain
        // this buffer between frames.
        auto bound = world.bind_resource_erased(key, name, object, sizeof(Ecs::ErasedEvents), alignof(Ecs::ErasedEvents),
                                                Ecs::clear_erased_events);
        if (!bound) {
            const std::lock_guard<std::mutex> lock{g_owned_mutex};
            g_owned.erase(owned);
            return error(ForeignResourceErrorKind::InvalidArgument, bound.error().message.cpp_string());
        }
        ModuleLedger::note(world, [&world, key] { (void)destroy_foreign_resource(world, key); });
        return key;
    }

    ForeignResourceResult<void> destroy_foreign_resource(Ecs::World &world, Ecs::ResourceKey key) {
        const OwnedKey owned = owned_key(world, key);
        {
            const std::lock_guard<std::mutex> lock{g_owned_mutex};
            if (g_owned.find(owned) == g_owned.end()) {
                // Either nothing is bound, or it is a C++ resource whose storage belongs to the engine. Freeing that would be a
                // use-after-free for its real owner.
                return error(ForeignResourceErrorKind::NotAvailable, "no foreign-owned resource is bound under that id");
            }
        }
        auto unbound = world.unbind_resource_erased(key);
        if (!unbound) {
            return error(ForeignResourceErrorKind::NotAvailable, unbound.error().message.cpp_string());
        }
        // Freed only after the world has let go of the pointer, so nothing can read it in between.
        const std::lock_guard<std::mutex> lock{g_owned_mutex};
        g_owned.erase(owned);
        return {};
    }

    ForeignResourceResult<void> read_resource(Ecs::World &world, Ecs::ResourceKey key, void *out, u32 size) {
        usize bound_size = 0;
        void *object = world.resource_pointer_erased(key, &bound_size);
        if (object == nullptr) {
            return error(ForeignResourceErrorKind::NotAvailable, "no resource is bound under that id");
        }
        if (bound_size != size) {
            return error(ForeignResourceErrorKind::InvalidArgument, "supplied size does not match the resource's size");
        }
        std::memcpy(out, object, size);
        return {};
    }

    ForeignResourceResult<void> write_resource(Ecs::World &world, Ecs::ResourceKey key, const void *data, u32 size) {
        usize bound_size = 0;
        void *object = world.resource_pointer_erased(key, &bound_size);
        if (object == nullptr) {
            return error(ForeignResourceErrorKind::NotAvailable, "no resource is bound under that id");
        }
        if (bound_size != size) {
            return error(ForeignResourceErrorKind::InvalidArgument, "supplied size does not match the resource's size");
        }
        std::memcpy(object, data, size);
        return {};
    }

    ForeignResourceResult<void> send_foreign_event(Ecs::World &world, Ecs::ResourceKey key, const void *event, u32 size) {
        auto events = channel(world, key);
        if (!events) {
            return std::unexpected(events.error());
        }
        if ((*events)->element_size() != size) {
            return error(ForeignResourceErrorKind::InvalidArgument, "supplied size does not match the channel's event size");
        }
        (*events)->send(event);
        return {};
    }

    ForeignResourceResult<u32> foreign_event_count(Ecs::World &world, Ecs::ResourceKey key) {
        auto events = channel(world, key);
        if (!events) {
            return std::unexpected(events.error());
        }
        return static_cast<u32>((*events)->size());
    }

    ForeignResourceResult<void> read_foreign_event(Ecs::World &world, Ecs::ResourceKey key, u32 index, void *out, u32 size) {
        auto events = channel(world, key);
        if (!events) {
            return std::unexpected(events.error());
        }
        if ((*events)->element_size() != size) {
            return error(ForeignResourceErrorKind::InvalidArgument, "supplied size does not match the channel's event size");
        }
        const void *stored = (*events)->at(index);
        if (stored == nullptr) {
            return error(ForeignResourceErrorKind::OutOfRange, "event index is out of range");
        }
        std::memcpy(out, stored, size);
        return {};
    }

    ForeignResourceResult<void> clear_foreign_events(Ecs::World &world, Ecs::ResourceKey key) {
        auto events = channel(world, key);
        if (!events) {
            return std::unexpected(events.error());
        }
        (*events)->clear();
        return {};
    }

} // namespace SFT::Engine
