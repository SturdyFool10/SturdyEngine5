/// Type-erased deferred commands: the non-template siblings of `Commands::spawn`,
/// `add_component` and `remove_component`.
///
/// These exist so a system whose body is not C++ — one registered through the C FFI — can still
/// make structural changes. A schedule holds the world's direct-access lock for its whole run, so
/// the only safe way to spawn or destroy from inside a system is to queue the work and let the
/// scheduler apply it at the stage boundary, which is exactly what `Commands` already does for
/// typed systems.
///
/// The component bytes are copied into the buffer at queue time rather than referenced. A foreign
/// caller's stack frame is gone by the time the command applies, so holding its pointer would be a
/// use-after-free that only shows up under load.

#include <Ecs/Commands.hpp>

#include <cstring>
#include <vector>

namespace SFT::Ecs {

    /// Queues creation of an entity carrying the supplied components.
    ///
    /// @param component_ids Components to attach.
    /// @param component_data Initial value for each, copied immediately.
    /// @param component_sizes Byte count for each component.
    ///
    /// @note This function does not throw exceptions.
    void Commands::spawn_erased(std::span<const ComponentId> component_ids,
                                std::span<const void *const> component_data,
                                std::span<const usize> component_sizes) noexcept {
        ZoneScopedN("Commands::spawn_erased");
        if (component_ids.empty() || component_ids.size() != component_data.size() ||
            component_ids.size() != component_sizes.size()) {
            return;
        }

        std::vector<ComponentId> ids{component_ids.begin(), component_ids.end()};
        std::vector<usize> offsets;
        std::vector<usize> sizes{component_sizes.begin(), component_sizes.end()};
        std::vector<std::byte> storage;
        offsets.reserve(ids.size());

        // Packed into one buffer with per-component offsets rather than a vector of vectors: this
        // runs once per queued spawn on a hot path, and one allocation beats N.
        usize total = 0;
        for (const usize size : sizes) {
            offsets.push_back(total);
            total += size;
        }
        storage.resize(total);
        for (usize index = 0; index < ids.size(); ++index) {
            if (sizes[index] != 0 && component_data[index] != nullptr) {
                std::memcpy(storage.data() + offsets[index], component_data[index], sizes[index]);
            }
        }

        buffer_->operations.emplace_back([ids = std::move(ids), offsets = std::move(offsets),
                                          storage = std::move(storage)](World &world) mutable noexcept {
            std::vector<const void *> pointers;
            pointers.reserve(ids.size());
            for (const usize offset : offsets) {
                pointers.push_back(storage.data() + offset);
            }
            // Routed through WorldAccess rather than the public spawn_erased: this runs from inside
            // Schedule::run, which holds the world lock and still has the schedule flag set, so the
            // public entry point would correctly refuse its own scheduler.
            //
            // Failures are dropped: a deferred command has no caller left to report to, and the
            // world's own validation is what keeps a bad request from corrupting anything.
            (void)Detail::WorldAccess::spawn_erased_deferred(world, ids, pointers);
        });
    }

    /// Queues attaching one component to an existing entity.
    ///
    /// @param entity Entity to modify.
    /// @param component Component to attach.
    /// @param data Initial value, copied immediately.
    /// @param size Byte count of `data`.
    ///
    /// @note This function does not throw exceptions.
    void Commands::add_component_erased(Entity entity,
                                        ComponentId component,
                                        const void *data,
                                        usize size) noexcept {
        ZoneScopedN("Commands::add_component_erased");

        std::vector<std::byte> storage(size);
        if (size != 0 && data != nullptr) {
            std::memcpy(storage.data(), data, size);
        }

        buffer_->operations.emplace_back(
            [entity, component, storage = std::move(storage)](World &world) mutable noexcept {
                // Dropped on failure for the same reason as spawn_erased: by the time this runs the
                // entity may have been destroyed by an earlier command in the same buffer, which is
                // ordinary rather than exceptional.
                (void)Detail::WorldAccess::add_component_erased_deferred(world, entity, component,
                                                                        storage.data());
            });
    }

    /// Queues detaching one component from an entity.
    ///
    /// @param entity Entity to modify.
    /// @param component Component to detach.
    ///
    /// @note This function does not throw exceptions.
    void Commands::remove_component_erased(Entity entity, ComponentId component) noexcept {
        ZoneScopedN("Commands::remove_component_erased");
        buffer_->operations.emplace_back([entity, component](World &world) noexcept {
            (void)Detail::WorldAccess::remove_component_erased_deferred(world, entity, component);
        });
    }

} // namespace SFT::Ecs
