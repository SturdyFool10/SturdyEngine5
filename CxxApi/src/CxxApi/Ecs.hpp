#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <Ecs/World.hpp>
#include <Engine/Engine.hpp>

#include <CxxApi/Types/Ecs.hpp>
#include <CxxApi/Types/Scene.hpp>

/// Components, entities and systems by id, for languages that cannot name a C++ type. A component is raw bytes whose layout the
/// registrant describes; two languages registering one name must agree on that layout (see `Engine::register_foreign_component`).
///
/// Direct calls on the world (`ecs_spawn`, `ecs_add_component`, ...) are for game-logic callbacks, never from inside a system
/// while the schedule runs (the ECS aborts on that misuse rather than racing); a system queues its structural changes through
/// the `SystemCommands` it is handed, and the engine applies them when the stage ends.
namespace SFT::CxxApi {

    /// The deferred-change queue handed to a system for one dispatch. Valid only during that call.
    class SystemCommands {
      public:
        SystemCommands(Ecs::Commands &commands, const Ecs::ComponentRegistry &registry) noexcept : commands(commands), registry(registry) {}

        Ecs::Commands &commands;
        const Ecs::ComponentRegistry &registry;
    };

    /// A system written in another language, called once per archetype with that archetype's contiguous component arrays (so a
    /// language that pays per call to cross into the engine pays once per archetype, not once per entity). The binding
    /// implements this once, forwarding to its own closures.
    class ChunkSystemCallback {
      public:
        virtual ~ChunkSystemCallback() = default;

        /// `entities` and every `columns[i]` are `count` long and parallel: element `r` of each belongs to `entities[r]`.
        /// `columns[i]` is the first element of the array for the i-th entry of the access list the system was registered
        /// with, each element as large as that component. Write through the arrays declared `AccessMode::Write`. Every pointer
        /// is valid only for this call. Must not unwind.
        virtual void run(const Entity *entities, std::size_t count, void *const *columns, SystemCommands &commands) noexcept = 0;
    };

    // ---- components ----

    /// Registers a plain-data component, or returns the existing id when the name is already registered with the same layout.
    /// Throws when the name is empty, the size is zero, the alignment is not a power of two no greater than 64, the fields do not
    /// fit or overlap, or the name exists with a different size, alignment or field layout (the message names the first field
    /// that differs).
    ///
    /// @param field_names Diagnostic names for `fields`, same length, or empty. Not part of the layout.
    /// @param fields The field layout, or empty to register without describing it (accepted against any layout).
    [[nodiscard]] std::uint32_t ecs_register_component(Ecs::World &world, const std::string &name, std::uint32_t size, std::uint32_t align,
                                                       const std::vector<ComponentFieldDesc> &fields,
                                                       const std::vector<std::string> &field_names);
    /// Fills `out_component` and returns true when a component is registered under `name`.
    [[nodiscard]] bool ecs_find_component(const Ecs::World &world, const std::string &name, std::uint32_t &out_component) noexcept;
    /// The component's layout fingerprint. Throws for an unknown id.
    [[nodiscard]] LayoutFingerprint ecs_component_layout(const Ecs::World &world, std::uint32_t component);
    /// The component's size in bytes. Throws for an unknown id.
    [[nodiscard]] std::uint32_t ecs_component_size(const Ecs::World &world, std::uint32_t component);

    // ---- entities ----

    /// Spawns an entity carrying `components` (at least one, no duplicates). `data` is each component's initial bytes laid out one
    /// after another in the same order, exactly the sum of the components' sizes. Throws when the world cannot be changed right
    /// now or an argument is wrong.
    [[nodiscard]] Entity ecs_spawn(Ecs::World &world, const std::vector<std::uint32_t> &components, const std::vector<std::uint8_t> &data);
    /// Destroys the entity (a dead entity is a no-op).
    void ecs_destroy(Ecs::World &world, Entity entity);
    [[nodiscard]] bool ecs_has_component(const Ecs::World &world, Entity entity, std::uint32_t component) noexcept;
    /// Attaches a component. `data` is exactly the component's size. Throws when the entity is dead, already has it, or the
    /// world cannot be changed right now.
    void ecs_add_component(Ecs::World &world, Entity entity, std::uint32_t component, const std::uint8_t *data, std::size_t size);
    void ecs_remove_component(Ecs::World &world, Entity entity, std::uint32_t component);
    /// Copies the component's bytes into `out` (exactly its size) and returns true; false when the entity is dead or lacks it.
    /// Throws for an unknown component or a wrong size.
    [[nodiscard]] bool ecs_get_component(const Ecs::World &world, Entity entity, std::uint32_t component, std::uint8_t *out, std::size_t size);
    /// Overwrites the component's bytes in place. Throws when the entity is dead or lacks it, or the size is wrong.
    void ecs_set_component(Ecs::World &world, Entity entity, std::uint32_t component, const std::uint8_t *data, std::size_t size);

    // ---- resources and event channels ----
    //
    // The engine allocates and owns the bytes. Two languages sharing one must agree on its size and, when both describe their
    // fields, its layout (see `Engine/ForeignResources.hpp`); a mismatch throws and names the first field that differs.

    /// The id for a name. Pure: the same name always gives the same id. Throws for an empty name.
    [[nodiscard]] ResourceId ecs_resource_id(const std::string &name);
    /// Creates a resource of `size` bytes, zeroed or copied from `initial` (empty, or exactly `size` bytes), and returns its id.
    /// Creating it again with the same size and layout returns the same id.
    [[nodiscard]] ResourceId ecs_create_resource(Ecs::World &world, const std::string &name, std::uint32_t size,
                                                 const std::vector<std::uint8_t> &initial, const std::vector<ComponentFieldDesc> &fields,
                                                 const std::vector<std::string> &field_names);
    /// Destroys a resource or channel created through this API. Throws for anything else (the engine's own resources included).
    void ecs_destroy_resource(Ecs::World &world, ResourceId resource);
    [[nodiscard]] bool ecs_has_resource(const Ecs::World &world, ResourceId resource) noexcept;
    /// Copies a bound resource's bytes into `out` (exactly its size). Throws when nothing is bound or the size is wrong.
    void ecs_read_resource(Ecs::World &world, ResourceId resource, std::uint8_t *out, std::size_t size);
    void ecs_write_resource(Ecs::World &world, ResourceId resource, const std::uint8_t *data, std::size_t size);

    /// Creates an event channel of fixed-size records. Events sent during a frame are readable for the rest of it and gone by the
    /// next, which makes a channel the way systems and modules in different languages pass messages without either owning them.
    [[nodiscard]] ResourceId ecs_create_event_channel(Ecs::World &world, const std::string &name, std::uint32_t event_size,
                                                      const std::vector<ComponentFieldDesc> &fields,
                                                      const std::vector<std::string> &field_names);
    /// Appends one event; `size` must equal the channel's event size. Throws for an unknown channel or a wrong size.
    void ecs_send_event(Ecs::World &world, ResourceId channel, const std::uint8_t *event, std::size_t size);
    [[nodiscard]] std::uint32_t ecs_event_count(Ecs::World &world, ResourceId channel);
    /// Copies event `index` into `out`. Throws when there is no such event, no such channel, or the size is wrong.
    void ecs_read_event(Ecs::World &world, ResourceId channel, std::uint32_t index, std::uint8_t *out, std::size_t size);
    void ecs_clear_events(Ecs::World &world, ResourceId channel);

    // ---- systems ----

    /// Registers a chunk system over the components in `access` (1 to 16, no duplicates) and the resources or event channels in
    /// `resources`. The declarations drive scheduling: a system that writes a component or resource never overlaps one that touches
    /// it, so declare everything the callback reads or writes. Returns a handle for the functions below, and keeps `callback`
    /// alive until the system is removed. Throws on a bad declaration or null callback.
    [[nodiscard]] std::uint64_t ecs_add_chunk_system(Engine::Engine &engine, const std::vector<ComponentAccess> &access,
                                                     const std::vector<ResourceAccess> &resources,
                                                     std::unique_ptr<ChunkSystemCallback> callback);
    /// Unregisters the system and destroys its callback. Must not be called while the schedule runs. Throws for an unknown handle.
    void ecs_remove_system(Engine::Engine &engine, std::uint64_t system);
    /// Skips (or resumes) the system without unregistering it. Throws for an unknown handle.
    void ecs_set_system_enabled(Engine::Engine &engine, std::uint64_t system, bool enabled);
    /// Forces `system` to run in a later stage than `dependency`, on top of what their declared access implies. Throws for an
    /// unknown handle.
    void ecs_order_system_after(Engine::Engine &engine, std::uint64_t system, std::uint64_t dependency);

    // ---- from inside a system: deferred changes ----

    /// Queues spawning an entity (`data` as for `ecs_spawn`). Throws when the arguments are wrong.
    void commands_spawn(SystemCommands &commands, const std::vector<std::uint32_t> &components, const std::vector<std::uint8_t> &data);
    void commands_destroy(SystemCommands &commands, Entity entity) noexcept;
    /// Queues attaching a component. `data` is exactly the component's size.
    void commands_add_component(SystemCommands &commands, Entity entity, std::uint32_t component, const std::uint8_t *data, std::size_t size);
    void commands_remove_component(SystemCommands &commands, Entity entity, std::uint32_t component) noexcept;

} // namespace SFT::CxxApi
