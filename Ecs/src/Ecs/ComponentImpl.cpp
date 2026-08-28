#include <Ecs/Component.hpp>

#include <bit>
#include <format>
#include <mutex>

#include <tracy/Tracy.hpp>

namespace SFT::Ecs {

    namespace {

        /// Creates an error result describing the supplied registry failure.
        ///
        /// @param code `code` value used by the operation.
        /// @param message Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] ComponentRegistryError registry_error(ComponentRegistryErrorCode code,
                                                            std::string message) {
            return ComponentRegistryError{.code = code, .message = UString{message}};
        }

        /// Performs the compatible descriptor operation for `Ecs` using the supplied arguments.
        ///
        /// @param existing `existing` value used by the operation.
        /// @param incoming `incoming` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool compatible_descriptor(const ComponentInfo &existing,
                                                 const ComponentInfo &incoming) noexcept {
            return existing.key == incoming.key &&
                   existing.canonical_name == incoming.canonical_name &&
                   existing.schema_version == incoming.schema_version &&
                   existing.size == incoming.size &&
                   existing.align == incoming.align &&
                   existing.flags == incoming.flags;
        }

    } // namespace

    /// Registers component using the supplied arguments and current state.
    ///
    /// @param info Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `ComponentRegistryErrorCode::InvalidDescriptor`, `ComponentRegistryErrorCode::UnsupportedStoragePolicy`, `ComponentRegistryErrorCode::StableKeyCollision`, `ComponentRegistryErrorCode::CanonicalNameCollision`, `ComponentRegistryErrorCode::ComponentLimitReached`.
    ComponentRegistryExpected<ComponentId> ComponentRegistry::register_component(ComponentInfo info) {
        ZoneScopedN("ComponentRegistry::register_component");
        if (!info.key || info.canonical_name.empty() || info.schema_version == 0 || info.size == 0 ||
            info.align == 0 || !std::has_single_bit(info.align) ||
            (info.move_construct == nullptr && !has_flag(info.flags, ComponentFlags::TriviallyCopyable)) ||
            (info.destroy == nullptr && !has_flag(info.flags, ComponentFlags::TriviallyDestructible))) {
            return std::unexpected(registry_error(
                ComponentRegistryErrorCode::InvalidDescriptor,
                "ECS component registration requires a key, canonical name, non-zero schema/size, power-of-two alignment, and lifecycle operations for non-trivial data."));
        }
        if (has_flag(info.flags, ComponentFlags::Tag) || has_flag(info.flags, ComponentFlags::Pinned)) {
            return std::unexpected(registry_error(
                ComponentRegistryErrorCode::UnsupportedStoragePolicy,
                "Tag and pinned component storage will be enabled by the Phase 2 world kernel and cannot be registered yet."));
        }

        std::unique_lock lock{mutex_};
        if (const auto by_key = ids_by_key_.find(info.key); by_key != ids_by_key_.end()) {
            const ComponentInfo &existing = infos_[by_key->second];
            if (compatible_descriptor(existing, info)) {
                return by_key->second;
            }
            return std::unexpected(registry_error(
                ComponentRegistryErrorCode::StableKeyCollision,
                std::format("Component key collision: '{}' conflicts with already registered '{}'.",
                            info.canonical_name,
                            existing.canonical_name)));
        }
        if (const auto by_name = ids_by_name_.find(info.canonical_name); by_name != ids_by_name_.end()) {
            return std::unexpected(registry_error(
                ComponentRegistryErrorCode::CanonicalNameCollision,
                std::format("Component canonical name '{}' is already registered with a different stable key.",
                            info.canonical_name)));
        }
        if (infos_.size() >= invalid_component_id) {
            return std::unexpected(registry_error(
                ComponentRegistryErrorCode::ComponentLimitReached,
                "The ECS component registry exhausted its dense 32-bit ID space."));
        }

        const ComponentId id = static_cast<ComponentId>(infos_.size());
        infos_.push_back(std::move(info));
        ids_by_key_.emplace(infos_.back().key, id);
        ids_by_name_.emplace(infos_.back().canonical_name, id);
        return id;
    }

    /// Finds the requested entry in the available state.
    ///
    /// @param key Key used to identify the requested entry.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    /// @note This function does not throw exceptions.
    std::optional<ComponentId> ComponentRegistry::find(ComponentKey key) const noexcept {
        ZoneScopedN("ComponentRegistry::find");
        std::shared_lock lock{mutex_};
        const auto found = ids_by_key_.find(key);
        return found == ids_by_key_.end() ? std::nullopt : std::optional<ComponentId>{found->second};
    }

    /// Finds the requested entry in the available state.
    ///
    /// @param canonical_name Name used to identify or label the target.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    /// @note This function does not throw exceptions.
    std::optional<ComponentId> ComponentRegistry::find(const ustr &canonical_name) const noexcept {
        ZoneScopedN("ComponentRegistry::find");
        std::shared_lock lock{mutex_};
        for (ComponentId id = 0; id < infos_.size(); ++id) {
            if (infos_[id].canonical_name == canonical_name) {
                return id;
            }
        }
        return std::nullopt;
    }

    /// Performs the info operation for `Ecs` using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    const ComponentInfo *ComponentRegistry::info(ComponentId id) const noexcept {
        ZoneScopedN("ComponentRegistry::info");
        std::shared_lock lock{mutex_};
        return id < infos_.size() ? &infos_[id] : nullptr;
    }

    /// Marks an already-registered component as a data-less tag.
    ///
    /// @param id Component to mark. Unknown ids are ignored.
    ///
    /// @note This function does not throw exceptions.
    void ComponentRegistry::mark_component_as_tag(ComponentId id) noexcept {
        ZoneScopedN("ComponentRegistry::mark_component_as_tag");
        std::unique_lock lock{mutex_};
        if (id >= infos_.size()) {
            return;
        }
        infos_[id].flags = infos_[id].flags | ComponentFlags::Tag;
    }

    /// Returns the size for this `Ecs`.
    ///
    /// @return Returns the current size value.
    /// @note This function does not throw exceptions.
    usize ComponentRegistry::size() const noexcept {
        ZoneScopedN("ComponentRegistry::size");
        std::shared_lock lock{mutex_};
        return infos_.size();
    }

} // namespace SFT::Ecs
