#include <Reflection/TypeRegistry.hpp>

#include <algorithm>
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
        generations_.push_back(1);
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
        ++generations_[found->second];
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
        ++generations_[found->second];
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

    std::pair<const FieldInfo *, void *> TypeRegistry::find_field_adjusted(const TypeInfo &type, TypeId field_key, void *object) const noexcept {
        return find_field_adjusted_impl(type, field_key, object, 0);
    }

    std::pair<const FieldInfo *, void *> TypeRegistry::find_field_adjusted_impl(const TypeInfo &type, TypeId field_key, void *object, usize depth) const noexcept {
        if (const FieldInfo *field = type.find_field(field_key); field != nullptr) {
            return {field, object};
        }
        if (depth >= max_base_chain_depth) {
            return {nullptr, nullptr};
        }
        if (type.base_type) {
            if (const TypeInfo *base = find(type.base_type); base != nullptr) {
                // No adjustment for the primary base — its subobject is at the same address.
                if (auto [field, adjusted] = find_field_adjusted_impl(*base, field_key, object, depth + 1); field != nullptr) {
                    return {field, adjusted};
                }
            }
        }
        for (const BaseInfo &secondary : type.secondary_bases) {
            const TypeInfo *base = find(secondary.type);
            if (base == nullptr) {
                continue;
            }
            void *adjusted_object = const_cast<void *>(secondary.cast(object));
            if (auto [field, adjusted] = find_field_adjusted_impl(*base, field_key, adjusted_object, depth + 1); field != nullptr) {
                return {field, adjusted};
            }
        }
        return {nullptr, nullptr};
    }

    std::pair<const FieldInfo *, void *> TypeRegistry::find_field_adjusted(const TypeInfo &type, std::string_view field_name, void *object) const noexcept {
        return find_field_adjusted_impl(type, TypeId::from_name(field_name), object, 0);
    }

    std::pair<const MethodInfo *, void *> TypeRegistry::find_method_adjusted(const TypeInfo &type, TypeId method_key, void *object) const noexcept {
        return find_method_adjusted_impl(type, method_key, object, 0);
    }

    std::pair<const MethodInfo *, void *> TypeRegistry::find_method_adjusted_impl(const TypeInfo &type, TypeId method_key, void *object, usize depth) const noexcept {
        if (const MethodInfo *method = type.find_method(method_key); method != nullptr) {
            return {method, object};
        }
        if (depth >= max_base_chain_depth) {
            return {nullptr, nullptr};
        }
        if (type.base_type) {
            if (const TypeInfo *base = find(type.base_type); base != nullptr) {
                if (auto [method, adjusted] = find_method_adjusted_impl(*base, method_key, object, depth + 1); method != nullptr) {
                    return {method, adjusted};
                }
            }
        }
        for (const BaseInfo &secondary : type.secondary_bases) {
            const TypeInfo *base = find(secondary.type);
            if (base == nullptr) {
                continue;
            }
            void *adjusted_object = const_cast<void *>(secondary.cast(object));
            if (auto [method, adjusted] = find_method_adjusted_impl(*base, method_key, adjusted_object, depth + 1); method != nullptr) {
                return {method, adjusted};
            }
        }
        return {nullptr, nullptr};
    }

    std::pair<const MethodInfo *, void *> TypeRegistry::find_method_adjusted(const TypeInfo &type, std::string_view method_name, void *object) const noexcept {
        // Method keys are name+signature, not name alone (see find_method(name)'s doc comment),
        // so — unlike find_field_adjusted's name overload — this cannot delegate to the TypeId
        // overload; it needs its own depth-capped by-name walk, mirroring the TypeId one exactly.
        return find_method_adjusted_by_name(type, method_name, object, 0);
    }

    std::pair<const MethodInfo *, void *> TypeRegistry::find_method_adjusted_by_name(const TypeInfo &type, std::string_view method_name, void *object, usize depth) const noexcept {
        if (const MethodInfo *method = type.find_method(method_name); method != nullptr) {
            return {method, object};
        }
        if (depth >= max_base_chain_depth) {
            return {nullptr, nullptr};
        }
        if (type.base_type) {
            if (const TypeInfo *base = find(type.base_type); base != nullptr) {
                if (auto [method, adjusted] = find_method_adjusted_by_name(*base, method_name, object, depth + 1); method != nullptr) {
                    return {method, adjusted};
                }
            }
        }
        for (const BaseInfo &secondary : type.secondary_bases) {
            const TypeInfo *base = find(secondary.type);
            if (base == nullptr) {
                continue;
            }
            void *adjusted_object = const_cast<void *>(secondary.cast(object));
            if (auto [method, adjusted] = find_method_adjusted_by_name(*base, method_name, adjusted_object, depth + 1); method != nullptr) {
                return {method, adjusted};
            }
        }
        return {nullptr, nullptr};
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
        return is_assignable_from_locked(base, derived, 0);
    }

    bool TypeRegistry::is_assignable_from_locked(TypeId base, TypeId derived, usize depth) const noexcept {
        // Inlines `find`'s lookup (rather than calling it) since `std::shared_mutex` is not
        // recursive — re-locking `mutex_` while already holding it (as every caller of this
        // function does) would be undefined behavior even for two shared (read) locks on the same
        // thread.
        if (derived == base) {
            return true;
        }
        if (depth >= max_base_chain_depth) {
            return false;
        }
        const auto found = ids_by_key_.find(derived);
        if (found == ids_by_key_.end()) {
            return false;
        }
        const TypeInfo &current = infos_[found->second];
        // Single inheritance's exact previous behavior, generalized to a real DFS: try the
        // primary chain, then every secondary base, depth-first, so a type that reaches `base`
        // through *any* reflected base (not just the first one checked) is still found.
        if (current.base_type && is_assignable_from_locked(base, current.base_type, depth + 1)) {
            return true;
        }
        for (const BaseInfo &secondary : current.secondary_bases) {
            if (is_assignable_from_locked(base, secondary.type, depth + 1)) {
                return true;
            }
        }
        return false;
    }

    TypeHandle TypeRegistry::handle_for(TypeId key) const noexcept {
        std::shared_lock lock(mutex_);
        const auto found = ids_by_key_.find(key);
        if (found == ids_by_key_.end()) {
            return TypeHandle{};
        }
        return TypeHandle{.index = static_cast<u32>(found->second), .generation = generations_[found->second]};
    }

    const TypeInfo *TypeRegistry::resolve(TypeHandle handle) const noexcept {
        std::shared_lock lock(mutex_);
        if (!handle || handle.index >= infos_.size() || revoked_[handle.index] || generations_[handle.index] != handle.generation) {
            return nullptr;
        }
        return &infos_[handle.index];
    }

    namespace {

        /// Shared by `field_handle`/`method_handle`: finds `key`'s position in `entries` (declared
        /// order — `FieldHandle`/`MethodHandle::index` is just that position), or
        /// `TypeHandle::invalid_index` when absent.
        template <class Entry>
        [[nodiscard]] u32 index_of_key(const std::vector<Entry> &entries, TypeId key) noexcept {
            for (usize i = 0; i < entries.size(); ++i) {
                if (entries[i].key == key) {
                    return static_cast<u32>(i);
                }
            }
            return TypeHandle::invalid_index;
        }

    } // namespace

    FieldHandle TypeRegistry::field_handle(TypeHandle owner, TypeId field_key) const noexcept {
        const TypeInfo *type = resolve(owner);
        if (type == nullptr) {
            return FieldHandle{};
        }
        if (const u32 index = index_of_key(type->fields, field_key); index != TypeHandle::invalid_index) {
            return FieldHandle{.owner = owner, .index = index, .is_static = false};
        }
        if (const u32 index = index_of_key(type->static_fields, field_key); index != TypeHandle::invalid_index) {
            return FieldHandle{.owner = owner, .index = index, .is_static = true};
        }
        return FieldHandle{};
    }

    const FieldInfo *TypeRegistry::resolve(FieldHandle handle) const noexcept {
        if (!handle) {
            return nullptr;
        }
        const TypeInfo *type = resolve(handle.owner);
        if (type == nullptr) {
            return nullptr;
        }
        const std::vector<FieldInfo> &entries = handle.is_static ? type->static_fields : type->fields;
        if (handle.index >= entries.size()) {
            return nullptr;
        }
        return &entries[handle.index];
    }

    MethodHandle TypeRegistry::method_handle(TypeHandle owner, TypeId method_key) const noexcept {
        const TypeInfo *type = resolve(owner);
        if (type == nullptr) {
            return MethodHandle{};
        }
        if (const u32 index = index_of_key(type->methods, method_key); index != TypeHandle::invalid_index) {
            return MethodHandle{.owner = owner, .index = index, .is_static = false};
        }
        if (const u32 index = index_of_key(type->static_methods, method_key); index != TypeHandle::invalid_index) {
            return MethodHandle{.owner = owner, .index = index, .is_static = true};
        }
        return MethodHandle{};
    }

    const MethodInfo *TypeRegistry::resolve(MethodHandle handle) const noexcept {
        if (!handle) {
            return nullptr;
        }
        const TypeInfo *type = resolve(handle.owner);
        if (type == nullptr) {
            return nullptr;
        }
        const std::vector<MethodInfo> &entries = handle.is_static ? type->static_methods : type->methods;
        if (handle.index >= entries.size()) {
            return nullptr;
        }
        return &entries[handle.index];
    }

    bool TypeRegistry::set_field_name_override(FieldHandle handle, UString name) noexcept {
        if (resolve(handle) == nullptr) {
            return false;
        }
        std::unique_lock lock(mutex_);
        const FieldOverlayKey key{.owner_index = handle.owner.index, .field_index = handle.index, .is_static = handle.is_static};
        field_overlays_[key].name = std::move(name);
        return true;
    }

    bool TypeRegistry::clear_field_name_override(FieldHandle handle) noexcept {
        if (resolve(handle) == nullptr) {
            return false;
        }
        std::unique_lock lock(mutex_);
        const FieldOverlayKey key{.owner_index = handle.owner.index, .field_index = handle.index, .is_static = handle.is_static};
        const auto found = field_overlays_.find(key);
        if (found == field_overlays_.end() || !found->second.name.has_value()) {
            return false;
        }
        found->second.name.reset();
        if (found->second.is_empty()) {
            field_overlays_.erase(found);
        }
        return true;
    }

    bool TypeRegistry::add_field_attribute_override(FieldHandle handle, Attribute attribute) noexcept {
        if (resolve(handle) == nullptr) {
            return false;
        }
        std::unique_lock lock(mutex_);
        const FieldOverlayKey key{.owner_index = handle.owner.index, .field_index = handle.index, .is_static = handle.is_static};
        field_overlays_[key].extra_attributes.push_back(std::move(attribute));
        return true;
    }

    bool TypeRegistry::clear_field_overlay(FieldHandle handle) noexcept {
        if (resolve(handle) == nullptr) {
            return false;
        }
        std::unique_lock lock(mutex_);
        const FieldOverlayKey key{.owner_index = handle.owner.index, .field_index = handle.index, .is_static = handle.is_static};
        return field_overlays_.erase(key) != 0;
    }

    UString TypeRegistry::effective_field_name(FieldHandle handle) const {
        const FieldInfo *field = resolve(handle);
        if (field == nullptr) {
            return UString{};
        }
        std::shared_lock lock(mutex_);
        const FieldOverlayKey key{.owner_index = handle.owner.index, .field_index = handle.index, .is_static = handle.is_static};
        const auto found = field_overlays_.find(key);
        if (found != field_overlays_.end() && found->second.name.has_value()) {
            return *found->second.name;
        }
        return field->name;
    }

    std::vector<Attribute> TypeRegistry::effective_field_attributes(FieldHandle handle) const {
        const FieldInfo *field = resolve(handle);
        if (field == nullptr) {
            return {};
        }
        std::vector<Attribute> result = field->attributes;
        std::shared_lock lock(mutex_);
        const FieldOverlayKey key{.owner_index = handle.owner.index, .field_index = handle.index, .is_static = handle.is_static};
        if (const auto found = field_overlays_.find(key); found != field_overlays_.end()) {
            result.insert(result.end(), found->second.extra_attributes.begin(), found->second.extra_attributes.end());
        }
        return result;
    }

    bool TypeRegistry::set_type_name_override(TypeHandle handle, UString name) noexcept {
        if (resolve(handle) == nullptr) {
            return false;
        }
        std::unique_lock lock(mutex_);
        type_overlays_[handle.index].name = std::move(name);
        return true;
    }

    bool TypeRegistry::clear_type_name_override(TypeHandle handle) noexcept {
        if (resolve(handle) == nullptr) {
            return false;
        }
        std::unique_lock lock(mutex_);
        const auto found = type_overlays_.find(handle.index);
        if (found == type_overlays_.end() || !found->second.name.has_value()) {
            return false;
        }
        found->second.name.reset();
        if (found->second.is_empty()) {
            type_overlays_.erase(found);
        }
        return true;
    }

    bool TypeRegistry::add_type_attribute_override(TypeHandle handle, Attribute attribute) noexcept {
        if (resolve(handle) == nullptr) {
            return false;
        }
        std::unique_lock lock(mutex_);
        type_overlays_[handle.index].extra_attributes.push_back(std::move(attribute));
        return true;
    }

    bool TypeRegistry::clear_type_overlay(TypeHandle handle) noexcept {
        if (resolve(handle) == nullptr) {
            return false;
        }
        std::unique_lock lock(mutex_);
        return type_overlays_.erase(handle.index) != 0;
    }

    UString TypeRegistry::effective_type_name(TypeHandle handle) const {
        const TypeInfo *type = resolve(handle);
        if (type == nullptr) {
            return UString{};
        }
        std::shared_lock lock(mutex_);
        const auto found = type_overlays_.find(handle.index);
        if (found != type_overlays_.end() && found->second.name.has_value()) {
            return *found->second.name;
        }
        return type->canonical_name;
    }

    std::vector<Attribute> TypeRegistry::effective_type_attributes(TypeHandle handle) const {
        const TypeInfo *type = resolve(handle);
        if (type == nullptr) {
            return {};
        }
        std::vector<Attribute> result = type->attributes;
        std::shared_lock lock(mutex_);
        if (const auto found = type_overlays_.find(handle.index); found != type_overlays_.end()) {
            result.insert(result.end(), found->second.extra_attributes.begin(), found->second.extra_attributes.end());
        }
        return result;
    }

    bool TypeRegistry::set_method_name_override(MethodHandle handle, UString name) noexcept {
        if (resolve(handle) == nullptr) {
            return false;
        }
        std::unique_lock lock(mutex_);
        const MethodOverlayKey key{.owner_index = handle.owner.index, .method_index = handle.index, .is_static = handle.is_static};
        method_overlays_[key].name = std::move(name);
        return true;
    }

    bool TypeRegistry::clear_method_name_override(MethodHandle handle) noexcept {
        if (resolve(handle) == nullptr) {
            return false;
        }
        std::unique_lock lock(mutex_);
        const MethodOverlayKey key{.owner_index = handle.owner.index, .method_index = handle.index, .is_static = handle.is_static};
        const auto found = method_overlays_.find(key);
        if (found == method_overlays_.end() || !found->second.name.has_value()) {
            return false;
        }
        found->second.name.reset();
        if (found->second.is_empty()) {
            method_overlays_.erase(found);
        }
        return true;
    }

    bool TypeRegistry::add_method_attribute_override(MethodHandle handle, Attribute attribute) noexcept {
        if (resolve(handle) == nullptr) {
            return false;
        }
        std::unique_lock lock(mutex_);
        const MethodOverlayKey key{.owner_index = handle.owner.index, .method_index = handle.index, .is_static = handle.is_static};
        method_overlays_[key].extra_attributes.push_back(std::move(attribute));
        return true;
    }

    bool TypeRegistry::clear_method_overlay(MethodHandle handle) noexcept {
        if (resolve(handle) == nullptr) {
            return false;
        }
        std::unique_lock lock(mutex_);
        const MethodOverlayKey key{.owner_index = handle.owner.index, .method_index = handle.index, .is_static = handle.is_static};
        return method_overlays_.erase(key) != 0;
    }

    UString TypeRegistry::effective_method_name(MethodHandle handle) const {
        const MethodInfo *method = resolve(handle);
        if (method == nullptr) {
            return UString{};
        }
        std::shared_lock lock(mutex_);
        const MethodOverlayKey key{.owner_index = handle.owner.index, .method_index = handle.index, .is_static = handle.is_static};
        const auto found = method_overlays_.find(key);
        if (found != method_overlays_.end() && found->second.name.has_value()) {
            return *found->second.name;
        }
        return method->name;
    }

    std::vector<Attribute> TypeRegistry::effective_method_attributes(MethodHandle handle) const {
        const MethodInfo *method = resolve(handle);
        if (method == nullptr) {
            return {};
        }
        std::vector<Attribute> result = method->attributes;
        std::shared_lock lock(mutex_);
        const MethodOverlayKey key{.owner_index = handle.owner.index, .method_index = handle.index, .is_static = handle.is_static};
        if (const auto found = method_overlays_.find(key); found != method_overlays_.end()) {
            result.insert(result.end(), found->second.extra_attributes.begin(), found->second.extra_attributes.end());
        }
        return result;
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
            // A repeat registration of the *same* definition is a no-op; a different one must not
            // be silently dropped in favour of the first, or the caller keeps using an enum that
            // doesn't match what it thinks it registered.
            const bool same_definition = existing.underlying_type == info.underlying_type && existing.size == info.size && existing.align == info.align &&
                                         existing.enumerators.size() == info.enumerators.size() &&
                                         std::equal(existing.enumerators.begin(), existing.enumerators.end(), info.enumerators.begin(),
                                                    [](const EnumeratorInfo &lhs, const EnumeratorInfo &rhs) {
                                                        return lhs.value == rhs.value && lhs.name.cpp_string_view() == rhs.name.cpp_string_view();
                                                    });
            if (!same_definition) {
                return std::unexpected(registry_error(
                    TypeRegistryErrorCode::StableKeyCollision,
                    std::format("Reflection enum '{}' is already registered with a different definition (size, alignment, or enumerators differ).",
                                existing.canonical_name.cpp_string_view())));
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

    bool TypeRegistry::unregister_enum(TypeId key) noexcept {
        std::unique_lock lock(mutex_);
        const auto found = enum_ids_by_key_.find(key);
        if (found == enum_ids_by_key_.end()) {
            return false;
        }
        enum_ids_by_name_.erase(enum_infos_[found->second].canonical_name);
        enum_ids_by_key_.erase(found);
        return true;
    }

    bool TypeRegistry::unregister_enum(const ustr &canonical_name) noexcept {
        std::unique_lock lock(mutex_);
        const auto found = enum_ids_by_name_.find(canonical_name);
        if (found == enum_ids_by_name_.end()) {
            return false;
        }
        enum_ids_by_key_.erase(enum_infos_[found->second].key);
        enum_ids_by_name_.erase(found);
        return true;
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
