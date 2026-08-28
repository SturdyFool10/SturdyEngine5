/// Type-erased resource binding: the non-template siblings of `World::bind_resource`,
/// `unbind_resource` and `has_resource`.
///
/// Resources differ from components in one way that matters here: the world never owns them. It
/// stores a borrowed pointer, and whoever bound it keeps the storage alive. That is why these
/// return the pointer directly rather than copying bytes the way the component API does — resource
/// storage does not move when the world is restructured, so there is no relocation hazard to guard
/// against.
///
/// As with the erased component API, the point of these is that they report failures the templated
/// versions treat as contract violations and terminate on.

#include <Ecs/World.hpp>

namespace SFT::Ecs {

    namespace {

        /// Builds a failure result.
        ///
        /// @param code Machine-readable reason.
        /// @param message Human-readable detail.
        ///
        /// @return The populated error.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] std::unexpected<WorldErasedError> resource_error(WorldErasedErrorCode code,
                                                                       std::string_view message) {
            return std::unexpected(WorldErasedError{.code = code, .message = UString{std::string{message}}});
        }

    } // namespace

    /// Binds a resource addressed by key rather than by C++ type.
    ///
    /// @param key Stable key, normally derived from `name`.
    /// @param name Canonical name, used to report key collisions intelligibly.
    /// @param object Borrowed storage.
    /// @param size Byte size of the resource.
    /// @param align Alignment of the resource.
    /// @param clear Optional hook invoked when the schedule clears events.
    ///
    /// @return Success, or why the resource could not be bound.
    /// @note Normal failures are returned through the type-specific error/status state.
    WorldErasedExpected<void> World::bind_resource_erased(ResourceKey key,
                                                          std::string_view name,
                                                          void *object,
                                                          usize size,
                                                          usize align,
                                                          void (*clear)(void *) noexcept) {
        ZoneScopedN("World::bind_resource_erased");

        if (object == nullptr) {
            return resource_error(WorldErasedErrorCode::InvalidArgument,
                                  "resource storage pointer must not be null");
        }
        if (size == 0) {
            return resource_error(WorldErasedErrorCode::InvalidArgument, "resource size must be nonzero");
        }
        if (scheduled_execution_.load(std::memory_order_acquire)) {
            return resource_error(WorldErasedErrorCode::ScheduleRunning,
                                  "cannot bind a resource while a schedule is running");
        }

        auto access = acquire_direct_mutation("bind an ECS resource");
        if (scheduled_execution_.load(std::memory_order_acquire)) {
            return resource_error(WorldErasedErrorCode::ScheduleRunning,
                                  "cannot bind a resource while a schedule is running");
        }

        const ResourceRecord incoming{
            .canonical_name = name,
            .size = size,
            .align = align,
            .object = object,
            .clear = clear,
        };

        if (auto existing = resources_.find(key); existing != resources_.end()) {
            // Rebinding the same resource to new storage is legitimate; rebinding a *different*
            // shape under the same key means two resources hashed together, which the templated
            // path treats as fatal. Reported here so a foreign caller sees the collision instead.
            if (existing->second.canonical_name != name || existing->second.size != size ||
                existing->second.align != align) {
                return resource_error(WorldErasedErrorCode::DuplicateComponent,
                                      "a different resource is already bound under that key");
            }
            existing->second.object = object;
            existing->second.clear = clear;
            return {};
        }

        resources_.emplace(key, incoming);
        return {};
    }

    /// Unbinds a resource addressed by key.
    ///
    /// @param key Resource to unbind.
    ///
    /// @return Success, or why it could not be unbound.
    /// @note Normal failures are returned through the type-specific error/status state.
    WorldErasedExpected<void> World::unbind_resource_erased(ResourceKey key) {
        ZoneScopedN("World::unbind_resource_erased");

        if (scheduled_execution_.load(std::memory_order_acquire)) {
            return resource_error(WorldErasedErrorCode::ScheduleRunning,
                                  "cannot unbind a resource while a schedule is running");
        }

        auto access = acquire_direct_mutation("unbind an ECS resource");
        if (scheduled_execution_.load(std::memory_order_acquire)) {
            return resource_error(WorldErasedErrorCode::ScheduleRunning,
                                  "cannot unbind a resource while a schedule is running");
        }
        if (resources_.erase(key) == 0) {
            return resource_error(WorldErasedErrorCode::MissingComponent,
                                  "no resource is bound under that key");
        }
        return {};
    }

    /// Reports whether a resource is bound.
    ///
    /// @param key Resource to look for.
    ///
    /// @return `true` when something is bound under that key.
    /// @note This function does not throw exceptions.
    bool World::has_resource_erased(ResourceKey key) const noexcept {
        ZoneScopedN("World::has_resource_erased");
        std::shared_lock access{direct_access_mutex_};
        return resources_.contains(key);
    }

    /// Returns the storage bound under a key, and its size.
    ///
    /// @param key Resource to resolve.
    /// @param out_size Receives the resource's byte size. May be null.
    ///
    /// @return The bound storage, or null when nothing is bound under that key.
    /// @note This function does not throw exceptions.
    void *World::resource_pointer_erased(ResourceKey key, usize *out_size) noexcept {
        ZoneScopedN("World::resource_pointer_erased");
        // Deliberately takes no lock and makes no schedule check: resource storage is owned
        // elsewhere and never relocated by the world, and systems must be able to reach their
        // declared resources while a schedule is running. The scheduler is what keeps two systems
        // from touching the same resource concurrently, using the access they declared.
        const auto found = resources_.find(key);
        if (found == resources_.end() || found->second.object == nullptr) {
            return nullptr;
        }
        if (out_size != nullptr) {
            *out_size = found->second.size;
        }
        return found->second.object;
    }

} // namespace SFT::Ecs
