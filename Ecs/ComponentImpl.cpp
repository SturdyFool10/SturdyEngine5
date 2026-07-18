#include <Ecs/Component.hpp>

#include <bit>
#include <format>
#include <mutex>

namespace SFT::Ecs {

    namespace {

        [[nodiscard]] ComponentRegistryError registry_error(ComponentRegistryErrorCode code,
                                                            std::string message) {
            return ComponentRegistryError{.code = code, .message = UString{message}};
        }

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

    ComponentRegistryExpected<ComponentId> ComponentRegistry::register_component(ComponentInfo info) {
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

    std::optional<ComponentId> ComponentRegistry::find(ComponentKey key) const noexcept {
        std::shared_lock lock{mutex_};
        const auto found = ids_by_key_.find(key);
        return found == ids_by_key_.end() ? std::nullopt : std::optional<ComponentId>{found->second};
    }

    std::optional<ComponentId> ComponentRegistry::find(const ustr &canonical_name) const noexcept {
        std::shared_lock lock{mutex_};
        for (ComponentId id = 0; id < infos_.size(); ++id) {
            if (infos_[id].canonical_name == canonical_name) {
                return id;
            }
        }
        return std::nullopt;
    }

    const ComponentInfo *ComponentRegistry::info(ComponentId id) const noexcept {
        std::shared_lock lock{mutex_};
        return id < infos_.size() ? &infos_[id] : nullptr;
    }

    usize ComponentRegistry::size() const noexcept {
        std::shared_lock lock{mutex_};
        return infos_.size();
    }

} // namespace SFT::Ecs
