#include <CxxApi/Ecs.hpp>

#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <span>
#include <string_view>

#include <Ecs/Commands.hpp>
#include <Ecs/System.hpp>
#include <Engine/ForeignEcs.hpp>
#include <Engine/ForeignResources.hpp>
#include <Engine/ModuleLedger.hpp>

#include <CxxApi/Conversions.hpp>
#include <CxxApi/Error.hpp>

namespace SFT::CxxApi {

    namespace {

        // CxxApi's plain-data entity is handed to a chunk system as an array without conversion, so the layouts must agree.
        static_assert(sizeof(Entity) == sizeof(Ecs::Entity) && alignof(Entity) == alignof(Ecs::Entity));
        static_assert(offsetof(Entity, index) == offsetof(Ecs::Entity, index) && offsetof(Entity, generation) == offsetof(Ecs::Entity, generation));
        static_assert(static_cast<std::uint32_t>(FieldKind::F64) == static_cast<std::uint32_t>(Ecs::FieldKind::F64));
        static_assert(static_cast<std::uint32_t>(FieldKind::Bool) == static_cast<std::uint32_t>(Ecs::FieldKind::Bool));

        [[nodiscard]] Ecs::Entity to_engine(Entity e) noexcept { return Ecs::Entity{.index = e.index, .generation = e.generation}; }

        [[nodiscard]] ResourceId to_id(Ecs::ResourceKey key) noexcept { return ResourceId{.high = key.high, .low = key.low}; }
        [[nodiscard]] Ecs::ResourceKey to_key(ResourceId id) noexcept { return Ecs::ResourceKey{.high = id.high, .low = id.low}; }

        /// A schema from parallel field descriptions and diagnostic names (the names may be empty, or one per field).
        [[nodiscard]] Ecs::ComponentSchema to_schema(const std::vector<ComponentFieldDesc> &fields, const std::vector<std::string> &field_names) {
            if (!field_names.empty() && field_names.size() != fields.size()) {
                fail("field_names must be empty or the same length as fields");
            }
            Ecs::ComponentSchema schema;
            schema.fields.reserve(fields.size());
            for (std::size_t i = 0; i < fields.size(); ++i) {
                schema.fields.push_back(Ecs::ComponentField{.name = field_names.empty() ? std::string{} : field_names[i],
                                                            .kind = static_cast<Ecs::FieldKind>(fields[i].kind),
                                                            .offset = fields[i].offset,
                                                            .count = fields[i].count});
            }
            return schema;
        }

        void unwrap_resource(const Engine::ForeignResourceResult<void> &result) {
            if (!result) {
                fail(result.error().message);
            }
        }

        template <typename T> T unwrap(Ecs::WorldErasedExpected<T> result) {
            if (!result) {
                fail(result.error().message.cpp_string());
            }
            if constexpr (!std::is_void_v<T>) {
                return std::move(*result);
            }
        }

        [[nodiscard]] const Ecs::ComponentInfo &component_info(const Ecs::ComponentRegistry &registry, std::uint32_t component) {
            const Ecs::ComponentInfo *info = registry.info(component);
            if (info == nullptr) {
                fail("no component is registered under that id");
            }
            return *info;
        }

        /// Splits packed spawn data into one pointer per component, checking the total against the component sizes.
        struct SpawnData {
            std::vector<Ecs::ComponentId> ids;
            std::vector<const void *> pointers;
            std::vector<usize> sizes;
        };
        [[nodiscard]] SpawnData split_spawn_data(const Ecs::ComponentRegistry &registry, const std::vector<std::uint32_t> &components,
                                                 const std::vector<std::uint8_t> &data) {
            if (components.empty()) {
                fail("an entity must carry at least one component");
            }
            SpawnData out;
            usize offset = 0;
            for (const std::uint32_t component : components) {
                const usize size = component_info(registry, component).size;
                if (offset + size > data.size()) {
                    fail("spawn data is shorter than the components' sizes");
                }
                out.ids.push_back(component);
                out.pointers.push_back(data.data() + offset);
                out.sizes.push_back(size);
                offset += size;
            }
            if (offset != data.size()) {
                fail("spawn data is longer than the components' sizes");
            }
            return out;
        }

        // ---- chunk systems ----

        /// Everything one registered chunk system needs at dispatch time. Owned by `g_systems` until removed.
        struct Registered {
            std::unique_ptr<ChunkSystemCallback> callback;
            const Ecs::ComponentRegistry *registry = nullptr;
        };

        struct SystemEntry {
            Engine::Engine *engine = nullptr;
            Ecs::SystemHandle handle{};
            std::unique_ptr<Registered> registered;
        };

        std::mutex g_systems_mutex;
        std::map<std::uint64_t, SystemEntry> g_systems;
        std::uint64_t g_next_system = 1;

        /// Per-dispatch state: the commands queue wrapped for the callback. Created by prepare, freed by finish.
        void *prepare_dispatch(Ecs::Commands *commands, void *user_data) noexcept {
            const auto *registered = static_cast<const Registered *>(user_data);
            try {
                return new SystemCommands(*commands, *registered->registry);
            } catch (...) {
                return nullptr;
            }
        }

        void finish_dispatch(void *dispatch_context, void *) noexcept { delete static_cast<SystemCommands *>(dispatch_context); }

        void invoke_chunk(const Ecs::Entity *entities, usize count, void *const *columns, void *dispatch_context, void *user_data) noexcept {
            auto *registered = static_cast<Registered *>(user_data);
            auto *commands = static_cast<SystemCommands *>(dispatch_context);
            if (commands == nullptr) {
                return; // allocation failed in prepare: skip this dispatch rather than hand the callback nothing to queue into
            }
            registered->callback->run(reinterpret_cast<const Entity *>(entities), count, columns, *commands);
        }

        [[nodiscard]] SystemEntry &find_system(Engine::Engine &engine, std::uint64_t system) {
            const auto found = g_systems.find(system);
            if (found == g_systems.end() || found->second.engine != &engine) {
                fail("no such system is registered on this engine");
            }
            return found->second;
        }

    } // namespace

    std::uint32_t ecs_register_component(Ecs::World &world, const std::string &name, std::uint32_t size, std::uint32_t align,
                                         const std::vector<ComponentFieldDesc> &fields, const std::vector<std::string> &field_names) {
        const Ecs::ComponentSchema schema = to_schema(fields, field_names);
        const auto registered = Engine::register_foreign_component(world.registry(), name, size, align, &schema);
        if (!registered) {
            fail(registered.error());
        }
        return *registered;
    }

    bool ecs_find_component(const Ecs::World &world, const std::string &name, std::uint32_t &out_component) noexcept {
        const UString canonical{std::string_view{name}};
        const auto found = world.registry().find(canonical.as_ustr());
        if (!found) {
            return false;
        }
        out_component = *found;
        return true;
    }

    LayoutFingerprint ecs_component_layout(const Ecs::World &world, std::uint32_t component) {
        const Ecs::ComponentInfo &info = component_info(world.registry(), component);
        return LayoutFingerprint{.high = info.layout.high, .low = info.layout.low};
    }

    std::uint32_t ecs_component_size(const Ecs::World &world, std::uint32_t component) {
        return static_cast<std::uint32_t>(component_info(world.registry(), component).size);
    }

    Entity ecs_spawn(Ecs::World &world, const std::vector<std::uint32_t> &components, const std::vector<std::uint8_t> &data) {
        const SpawnData split = split_spawn_data(world.registry(), components, data);
        const Ecs::Entity entity = unwrap(world.spawn_erased(split.ids, split.pointers));
        return Entity{.index = entity.index, .generation = entity.generation};
    }

    void ecs_destroy(Ecs::World &world, Entity entity) { world.destroy(to_engine(entity)); }

    bool ecs_has_component(const Ecs::World &world, Entity entity, std::uint32_t component) noexcept {
        return world.has_component_erased(to_engine(entity), component);
    }

    void ecs_add_component(Ecs::World &world, Entity entity, std::uint32_t component, const std::uint8_t *data, std::size_t size) {
        if (size != component_info(world.registry(), component).size) {
            fail("data size does not match the component's size");
        }
        unwrap(world.add_component_erased(to_engine(entity), component, data));
    }

    void ecs_remove_component(Ecs::World &world, Entity entity, std::uint32_t component) {
        unwrap(world.remove_component_erased(to_engine(entity), component));
    }

    bool ecs_get_component(const Ecs::World &world, Entity entity, std::uint32_t component, std::uint8_t *out, std::size_t size) {
        if (size != component_info(world.registry(), component).size) {
            fail("buffer size does not match the component's size");
        }
        if (!world.is_alive(to_engine(entity)) || !world.has_component_erased(to_engine(entity), component)) {
            return false;
        }
        unwrap(world.read_component_erased(to_engine(entity), component, out, size));
        return true;
    }

    void ecs_set_component(Ecs::World &world, Entity entity, std::uint32_t component, const std::uint8_t *data, std::size_t size) {
        if (size != component_info(world.registry(), component).size) {
            fail("data size does not match the component's size");
        }
        unwrap(world.write_component_erased(to_engine(entity), component, data, size));
    }

    ResourceId ecs_resource_id(const std::string &name) {
        if (name.empty()) {
            fail("resource name must not be empty");
        }
        return to_id(Ecs::ResourceKey::from_name(name));
    }

    ResourceId ecs_create_resource(Ecs::World &world, const std::string &name, std::uint32_t size, const std::vector<std::uint8_t> &initial,
                                   const std::vector<ComponentFieldDesc> &fields, const std::vector<std::string> &field_names) {
        if (!initial.empty() && initial.size() != size) {
            fail("initial data must be empty or exactly the resource's size");
        }
        const Ecs::ComponentSchema schema = to_schema(fields, field_names);
        const auto created = Engine::create_foreign_resource(world, name, size, initial.empty() ? nullptr : initial.data(), &schema);
        if (!created) {
            fail(created.error().message);
        }
        return to_id(*created);
    }

    void ecs_destroy_resource(Ecs::World &world, ResourceId resource) {
        unwrap_resource(Engine::destroy_foreign_resource(world, to_key(resource)));
    }

    bool ecs_has_resource(const Ecs::World &world, ResourceId resource) noexcept { return world.has_resource_erased(to_key(resource)); }

    void ecs_read_resource(Ecs::World &world, ResourceId resource, std::uint8_t *out, std::size_t size) {
        unwrap_resource(Engine::read_resource(world, to_key(resource), out, static_cast<std::uint32_t>(size)));
    }

    void ecs_write_resource(Ecs::World &world, ResourceId resource, const std::uint8_t *data, std::size_t size) {
        unwrap_resource(Engine::write_resource(world, to_key(resource), data, static_cast<std::uint32_t>(size)));
    }

    ResourceId ecs_create_event_channel(Ecs::World &world, const std::string &name, std::uint32_t event_size,
                                        const std::vector<ComponentFieldDesc> &fields, const std::vector<std::string> &field_names) {
        const Ecs::ComponentSchema schema = to_schema(fields, field_names);
        const auto created = Engine::create_foreign_event_channel(world, name, event_size, &schema);
        if (!created) {
            fail(created.error().message);
        }
        return to_id(*created);
    }

    void ecs_send_event(Ecs::World &world, ResourceId channel, const std::uint8_t *event, std::size_t size) {
        unwrap_resource(Engine::send_foreign_event(world, to_key(channel), event, static_cast<std::uint32_t>(size)));
    }

    std::uint32_t ecs_event_count(Ecs::World &world, ResourceId channel) {
        const auto count = Engine::foreign_event_count(world, to_key(channel));
        if (!count) {
            fail(count.error().message);
        }
        return *count;
    }

    void ecs_read_event(Ecs::World &world, ResourceId channel, std::uint32_t index, std::uint8_t *out, std::size_t size) {
        unwrap_resource(Engine::read_foreign_event(world, to_key(channel), index, out, static_cast<std::uint32_t>(size)));
    }

    void ecs_clear_events(Ecs::World &world, ResourceId channel) { unwrap_resource(Engine::clear_foreign_events(world, to_key(channel))); }

    std::uint64_t ecs_add_chunk_system(Engine::Engine &engine, const std::vector<ComponentAccess> &access,
                                       const std::vector<ResourceAccess> &resources, std::unique_ptr<ChunkSystemCallback> callback) {
        if (!callback) {
            fail("a chunk system needs its callback");
        }
        if (access.empty()) {
            fail("a chunk system must declare at least one component to iterate");
        }
        if (access.size() > Ecs::max_erased_visit_components) {
            fail("a system may declare at most 16 components");
        }
        Ecs::World &world = engine.ecs_world();
        Ecs::SystemAccess system_access;
        std::vector<Ecs::ComponentId> ids;
        for (std::size_t i = 0; i < access.size(); ++i) {
            const Ecs::ComponentInfo &info = component_info(world.registry(), access[i].component);
            for (std::size_t other = 0; other < i; ++other) {
                if (access[other].component == access[i].component) {
                    fail("the same component was declared more than once");
                }
            }
            switch (access[i].mode) {
                case AccessMode::Read: system_access.reads.push_back(info.key); break;
                case AccessMode::Write: system_access.writes.push_back(info.key); break;
                default: fail("unrecognized access mode");
            }
            ids.push_back(access[i].component);
        }
        for (std::size_t i = 0; i < resources.size(); ++i) {
            const Ecs::ResourceKey key = to_key(resources[i].resource);
            for (std::size_t other = 0; other < i; ++other) {
                if (resources[other].resource.high == resources[i].resource.high && resources[other].resource.low == resources[i].resource.low) {
                    fail("the same resource was declared more than once");
                }
            }
            // Declared as a plain resource even when it is an event channel: a foreign channel is drained explicitly and reads
            // whatever is buffered, so plain read/write conflict detection is the honest description (as in the C ABI).
            switch (resources[i].mode) {
                case AccessMode::Read: system_access.resource_reads.push_back(key); break;
                case AccessMode::Write: system_access.resource_writes.push_back(key); break;
                default: fail("unrecognized access mode");
            }
        }

        auto registered = std::make_unique<Registered>();
        registered->callback = std::move(callback);
        registered->registry = &world.registry();
        const Ecs::SystemHandle handle = engine.update_schedule().add_erased_chunk_system(
            std::move(system_access), std::move(ids), invoke_chunk, registered.get(), prepare_dispatch, finish_dispatch);
        if (!handle) {
            fail("the system could not be registered");
        }
        std::uint64_t token = 0;
        {
            const std::lock_guard<std::mutex> lock{g_systems_mutex};
            token = g_next_system++;
            g_systems.emplace(token, SystemEntry{.engine = &engine, .handle = handle, .registered = std::move(registered)});
        }
        // A system registered while a module is starting up belongs to that module and goes away when it is unloaded. The
        // owner may remove it first, so a missing handle is fine.
        Engine::ModuleLedger::note(world, [&engine, token] {
            try {
                ecs_remove_system(engine, token);
            } catch (const Error &) {
            }
        });
        return token;
    }

    void ecs_remove_system(Engine::Engine &engine, std::uint64_t system) {
        const std::lock_guard<std::mutex> lock{g_systems_mutex};
        SystemEntry &entry = find_system(engine, system);
        if (!engine.update_schedule().remove_system(entry.handle)) {
            fail("the system is no longer registered");
        }
        // Nothing can be running it any more (removal is refused mid-run), so its callback can go.
        g_systems.erase(system);
    }

    void ecs_set_system_enabled(Engine::Engine &engine, std::uint64_t system, bool enabled) {
        const std::lock_guard<std::mutex> lock{g_systems_mutex};
        (void)engine.update_schedule().set_system_enabled(find_system(engine, system).handle, enabled);
    }

    void ecs_order_system_after(Engine::Engine &engine, std::uint64_t system, std::uint64_t dependency) {
        const std::lock_guard<std::mutex> lock{g_systems_mutex};
        const Ecs::SystemHandle a = find_system(engine, system).handle;
        const Ecs::SystemHandle b = find_system(engine, dependency).handle;
        if (!engine.update_schedule().order_after(a, b)) {
            fail("a system cannot be ordered after itself or after a system that is no longer registered");
        }
    }

    void commands_spawn(SystemCommands &commands, const std::vector<std::uint32_t> &components, const std::vector<std::uint8_t> &data) {
        const SpawnData split = split_spawn_data(commands.registry, components, data);
        commands.commands.spawn_erased(split.ids, split.pointers, split.sizes);
    }

    void commands_destroy(SystemCommands &commands, Entity entity) noexcept { commands.commands.destroy(to_engine(entity)); }

    void commands_add_component(SystemCommands &commands, Entity entity, std::uint32_t component, const std::uint8_t *data, std::size_t size) {
        if (size != component_info(commands.registry, component).size) {
            fail("data size does not match the component's size");
        }
        commands.commands.add_component_erased(to_engine(entity), component, data, size);
    }

    void commands_remove_component(SystemCommands &commands, Entity entity, std::uint32_t component) noexcept {
        commands.commands.remove_component_erased(to_engine(entity), component);
    }

} // namespace SFT::CxxApi
