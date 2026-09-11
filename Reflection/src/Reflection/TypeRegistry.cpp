#include <Reflection/TypeRegistry.hpp>

#include <format>

namespace SFT::Reflection {

    namespace {

        [[nodiscard]] TypeRegistryError registry_error(TypeRegistryErrorCode code, std::string message) {
            return TypeRegistryError{.code = code, .message = UString{message}};
        }

        /// Caps how many `base_type` hops `find_field`/`find_method`/`find_event` will follow,
        /// so a misconfigured (accidentally cyclic) base chain fails closed instead of hanging.
        constexpr usize max_base_chain_depth = 64;

    } // namespace

    TypeRegistry &TypeRegistry::instance() noexcept {
        static TypeRegistry registry;
        return registry;
    }

    TypeRegistryExpected<TypeId> TypeRegistry::register_type(TypeInfo info) {
        ZoneScopedN("TypeRegistry::register_type");

        if (!info.key || info.canonical_name.cpp_string_view().empty() ||
            info.move_construct == nullptr || info.destroy == nullptr) {
            return std::unexpected(registry_error(
                TypeRegistryErrorCode::InvalidDescriptor,
                "Reflection type descriptor is missing a key, canonical name, or required "
                "move-construct/destroy function pointer."));
        }

        std::unique_lock lock(mutex_);

        if (const auto existing_by_key = ids_by_key_.find(info.key); existing_by_key != ids_by_key_.end()) {
            const TypeInfo &existing = infos_[existing_by_key->second];
            if (existing.canonical_name.cpp_string_view() != info.canonical_name.cpp_string_view()) {
                return std::unexpected(registry_error(
                    TypeRegistryErrorCode::StableKeyCollision,
                    std::format("Reflection type key collision between '{}' and '{}'.",
                                existing.canonical_name.cpp_string_view(), info.canonical_name.cpp_string_view())));
            }
            return existing.key;
        }

        if (ids_by_name_.contains(info.canonical_name)) {
            return std::unexpected(registry_error(
                TypeRegistryErrorCode::CanonicalNameCollision,
                std::format("Reflection type name '{}' is already registered under a different key.",
                            info.canonical_name.cpp_string_view())));
        }

        const TypeId key = info.key;
        const UString canonical_name = info.canonical_name;
        const usize index = infos_.size();
        infos_.push_back(std::move(info));
        revoked_.push_back(false);
        ids_by_key_.emplace(key, index);
        ids_by_name_.emplace(canonical_name, index);
        return key;
    }

    bool TypeRegistry::unregister_type(TypeId key) noexcept {
        std::unique_lock lock(mutex_);
        const auto found = ids_by_key_.find(key);
        if (found == ids_by_key_.end()) {
            return false;
        }
        revoked_[found->second] = true;
        ids_by_name_.erase(infos_[found->second].canonical_name);
        ids_by_key_.erase(found);
        return true;
    }

    bool TypeRegistry::unregister_type(const ustr &canonical_name) noexcept {
        std::unique_lock lock(mutex_);
        const auto found = ids_by_name_.find(canonical_name);
        if (found == ids_by_name_.end()) {
            return false;
        }
        revoked_[found->second] = true;
        ids_by_key_.erase(infos_[found->second].key);
        ids_by_name_.erase(found);
        return true;
    }

    const TypeInfo *TypeRegistry::find(TypeId key) const noexcept {
        std::shared_lock lock(mutex_);
        const auto found = ids_by_key_.find(key);
        if (found == ids_by_key_.end()) {
            return nullptr;
        }
        return &infos_[found->second];
    }

    const TypeInfo *TypeRegistry::find(const ustr &canonical_name) const noexcept {
        std::shared_lock lock(mutex_);
        const auto found = ids_by_name_.find(canonical_name);
        if (found == ids_by_name_.end()) {
            return nullptr;
        }
        return &infos_[found->second];
    }

    const FieldInfo *TypeRegistry::find_field(const TypeInfo &type, TypeId field_key) const noexcept {
        const TypeInfo *current = &type;
        for (usize depth = 0; current != nullptr && depth < max_base_chain_depth; ++depth) {
            if (const FieldInfo *field = current->find_field(field_key); field != nullptr) {
                return field;
            }
            current = current->base_type ? find(current->base_type) : nullptr;
        }
        return nullptr;
    }

    const FieldInfo *TypeRegistry::find_field(const TypeInfo &type, std::string_view field_name) const noexcept {
        return find_field(type, TypeId::from_name(field_name));
    }

    const MethodInfo *TypeRegistry::find_method(const TypeInfo &type, TypeId method_key) const noexcept {
        const TypeInfo *current = &type;
        for (usize depth = 0; current != nullptr && depth < max_base_chain_depth; ++depth) {
            if (const MethodInfo *method = current->find_method(method_key); method != nullptr) {
                return method;
            }
            current = current->base_type ? find(current->base_type) : nullptr;
        }
        return nullptr;
    }

    const MethodInfo *TypeRegistry::find_method(const TypeInfo &type, std::string_view method_name) const noexcept {
        // Unlike `find_field`/`find_event`'s by-name overloads, this cannot delegate to the
        // `TypeId` overload via `TypeId::from_name(method_name)` — a method's key is now derived
        // from its name *and* parameter-type signature (see `Detail::compute_method_key`), not
        // the name alone, so that would almost never match. Walk the base chain comparing names
        // directly instead, exactly mirroring the `TypeId` overload's structure.
        const TypeInfo *current = &type;
        for (usize depth = 0; current != nullptr && depth < max_base_chain_depth; ++depth) {
            if (const MethodInfo *method = current->find_method(method_name); method != nullptr) {
                return method;
            }
            current = current->base_type ? find(current->base_type) : nullptr;
        }
        return nullptr;
    }

    const MethodInfo *TypeRegistry::find_method(const TypeInfo &type, std::string_view method_name, std::span<const TypeId> param_types) const noexcept {
        const TypeInfo *current = &type;
        for (usize depth = 0; current != nullptr && depth < max_base_chain_depth; ++depth) {
            if (const MethodInfo *method = current->find_method(method_name, param_types); method != nullptr) {
                return method;
            }
            current = current->base_type ? find(current->base_type) : nullptr;
        }
        return nullptr;
    }

    const EventInfo *TypeRegistry::find_event(const TypeInfo &type, TypeId event_key) const noexcept {
        const TypeInfo *current = &type;
        for (usize depth = 0; current != nullptr && depth < max_base_chain_depth; ++depth) {
            if (const EventInfo *event = current->find_event(event_key); event != nullptr) {
                return event;
            }
            current = current->base_type ? find(current->base_type) : nullptr;
        }
        return nullptr;
    }

    const EventInfo *TypeRegistry::find_event(const TypeInfo &type, std::string_view event_name) const noexcept {
        return find_event(type, TypeId::from_name(event_name));
    }

    bool TypeRegistry::set_method_override(TypeId type_key, TypeId method_key, MethodInvokeFn override_fn, void *user_data) noexcept {
        std::shared_lock lock(mutex_);
        const auto found = ids_by_key_.find(type_key);
        if (found == ids_by_key_.end()) {
            return false;
        }
        MethodInfo *method = infos_[found->second].find_method(method_key);
        if (method == nullptr) {
            return false;
        }
        method->override_user_data = user_data;
        method->override_fn.store(override_fn, std::memory_order_release);
        return true;
    }

    bool TypeRegistry::clear_method_override(TypeId type_key, TypeId method_key) noexcept {
        std::shared_lock lock(mutex_);
        const auto found = ids_by_key_.find(type_key);
        if (found == ids_by_key_.end()) {
            return false;
        }
        MethodInfo *method = infos_[found->second].find_method(method_key);
        if (method == nullptr) {
            return false;
        }
        method->override_fn.store(nullptr, std::memory_order_release);
        method->override_user_data = nullptr;
        return true;
    }

    std::optional<Multicast::Subscription> TypeRegistry::add_method_before_hook(TypeId type_key, TypeId method_key, Multicast::ListenerFn hook, void *user_data) noexcept {
        std::shared_lock lock(mutex_);
        const auto found = ids_by_key_.find(type_key);
        if (found == ids_by_key_.end()) {
            return std::nullopt;
        }
        MethodInfo *method = infos_[found->second].find_method(method_key);
        if (method == nullptr) {
            return std::nullopt;
        }
        return method->before_invoke.subscribe(hook, user_data);
    }

    bool TypeRegistry::remove_method_before_hook(TypeId type_key, TypeId method_key, Multicast::Subscription subscription) noexcept {
        std::shared_lock lock(mutex_);
        const auto found = ids_by_key_.find(type_key);
        if (found == ids_by_key_.end()) {
            return false;
        }
        MethodInfo *method = infos_[found->second].find_method(method_key);
        if (method == nullptr) {
            return false;
        }
        return method->before_invoke.unsubscribe(subscription);
    }

    std::optional<Multicast::Subscription> TypeRegistry::add_method_after_hook(TypeId type_key, TypeId method_key, Multicast::ListenerFn hook, void *user_data) noexcept {
        std::shared_lock lock(mutex_);
        const auto found = ids_by_key_.find(type_key);
        if (found == ids_by_key_.end()) {
            return std::nullopt;
        }
        MethodInfo *method = infos_[found->second].find_method(method_key);
        if (method == nullptr) {
            return std::nullopt;
        }
        return method->after_invoke.subscribe(hook, user_data);
    }

    bool TypeRegistry::remove_method_after_hook(TypeId type_key, TypeId method_key, Multicast::Subscription subscription) noexcept {
        std::shared_lock lock(mutex_);
        const auto found = ids_by_key_.find(type_key);
        if (found == ids_by_key_.end()) {
            return false;
        }
        MethodInfo *method = infos_[found->second].find_method(method_key);
        if (method == nullptr) {
            return false;
        }
        return method->after_invoke.unsubscribe(subscription);
    }

    std::optional<Multicast::Subscription> TypeRegistry::subscribe_event(TypeId type_key, TypeId event_key, Multicast::ListenerFn listener, void *user_data) noexcept {
        std::shared_lock lock(mutex_);
        const auto found = ids_by_key_.find(type_key);
        if (found == ids_by_key_.end()) {
            return std::nullopt;
        }
        // `events` is declared-only storage (unlike methods, events are never looked up
        // mutably elsewhere), so a const find here is sufficient; `EventInfo::listeners` is a
        // `Multicast` handle whose subscribe/unsubscribe are themselves const.
        const EventInfo *event = infos_[found->second].find_event(event_key);
        if (event == nullptr) {
            return std::nullopt;
        }
        return event->listeners.subscribe(listener, user_data);
    }

    bool TypeRegistry::unsubscribe_event(TypeId type_key, TypeId event_key, Multicast::Subscription subscription) noexcept {
        std::shared_lock lock(mutex_);
        const auto found = ids_by_key_.find(type_key);
        if (found == ids_by_key_.end()) {
            return false;
        }
        const EventInfo *event = infos_[found->second].find_event(event_key);
        if (event == nullptr) {
            return false;
        }
        return event->listeners.unsubscribe(subscription);
    }

    bool TypeRegistry::is_assignable_from(TypeId base, TypeId derived) const noexcept {
        std::shared_lock lock(mutex_);
        // Inlines `find`'s lookup (rather than calling it) since `std::shared_mutex` is not
        // recursive — re-locking `mutex_` while already holding `lock` here would be undefined
        // behavior even for two shared (read) locks on the same thread.
        TypeId current_key = derived;
        for (usize depth = 0; depth < max_base_chain_depth; ++depth) {
            if (current_key == base) {
                return true;
            }
            const auto found = ids_by_key_.find(current_key);
            if (found == ids_by_key_.end()) {
                return false;
            }
            const TypeInfo &current = infos_[found->second];
            if (!current.base_type) {
                return false;
            }
            current_key = current.base_type;
        }
        return false;
    }

    std::vector<const TypeInfo *> TypeRegistry::find_types_with_attribute(TypeId attribute_key) const {
        std::vector<const TypeInfo *> result;
        std::shared_lock lock(mutex_);
        for (usize index = 0; index < infos_.size(); ++index) {
            if (!revoked_[index] && find_attribute(infos_[index], attribute_key) != nullptr) {
                result.push_back(&infos_[index]);
            }
        }
        return result;
    }

    std::vector<const TypeInfo *> TypeRegistry::find_types_with_attribute(std::string_view attribute_name) const {
        return find_types_with_attribute(TypeId::from_name(attribute_name));
    }

    usize TypeRegistry::size() const noexcept {
        std::shared_lock lock(mutex_);
        // Live/reachable count, not `infos_.size()` — a revoked type's slot stays physically
        // present (see `unregister_type`) but is no longer "registered" in any meaningful sense.
        return ids_by_key_.size();
    }

    TypeRegistryExpected<TypeId> TypeRegistry::register_enum(EnumInfo info) {
        ZoneScopedN("TypeRegistry::register_enum");

        if (!info.key || info.canonical_name.cpp_string_view().empty()) {
            return std::unexpected(registry_error(
                TypeRegistryErrorCode::InvalidDescriptor,
                "Reflection enum descriptor is missing a key or canonical name."));
        }

        std::unique_lock lock(mutex_);

        if (const auto existing_by_key = enum_ids_by_key_.find(info.key); existing_by_key != enum_ids_by_key_.end()) {
            const EnumInfo &existing = enum_infos_[existing_by_key->second];
            if (existing.canonical_name.cpp_string_view() != info.canonical_name.cpp_string_view()) {
                return std::unexpected(registry_error(
                    TypeRegistryErrorCode::StableKeyCollision,
                    std::format("Reflection enum key collision between '{}' and '{}'.",
                                existing.canonical_name.cpp_string_view(), info.canonical_name.cpp_string_view())));
            }
            return existing.key;
        }

        if (enum_ids_by_name_.contains(info.canonical_name)) {
            return std::unexpected(registry_error(
                TypeRegistryErrorCode::CanonicalNameCollision,
                std::format("Reflection enum name '{}' is already registered under a different key.",
                            info.canonical_name.cpp_string_view())));
        }

        const TypeId key = info.key;
        const UString canonical_name = info.canonical_name;
        const usize index = enum_infos_.size();
        enum_infos_.push_back(std::move(info));
        enum_ids_by_key_.emplace(key, index);
        enum_ids_by_name_.emplace(canonical_name, index);
        return key;
    }

    const EnumInfo *TypeRegistry::find_enum(TypeId key) const noexcept {
        std::shared_lock lock(mutex_);
        const auto found = enum_ids_by_key_.find(key);
        if (found == enum_ids_by_key_.end()) {
            return nullptr;
        }
        return &enum_infos_[found->second];
    }

    const EnumInfo *TypeRegistry::find_enum(const ustr &canonical_name) const noexcept {
        std::shared_lock lock(mutex_);
        const auto found = enum_ids_by_name_.find(canonical_name);
        if (found == enum_ids_by_name_.end()) {
            return nullptr;
        }
        return &enum_infos_[found->second];
    }

} // namespace SFT::Reflection
