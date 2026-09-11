/// C ABI implementation of the reflection/modding surface.
///
/// Unlike every other section of this ABI, none of this routes through a `SturdyEngine` handle —
/// `SFT::Reflection::TypeRegistry` is a process-wide singleton, so every function here resolves
/// directly against `TypeRegistry::instance()`. Ids are plain 128-bit name hashes
/// (`SturdyReflectionId`/`SFT::Reflection::TypeId`), not scope-bound tokens, except for
/// `SturdyReflectionTypeBuilder`, which — like `SturdyGltfScene` — is a caller-owned handle backed
/// by a heap-allocated `TypeInfoBuilder` this file keeps alive until `_finish`/`_discard`.

#include <Foundation/Foundation.hpp>
#include <Reflection/Reflection.hpp>

#include <mutex>
#include <string_view>
#include <unordered_map>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Reflection::Attribute;
    using SFT::Reflection::ConstructorInfo;
    using SFT::Reflection::EventInfo;
    using SFT::Reflection::FieldFlags;
    using SFT::Reflection::FieldInfo;
    using SFT::Reflection::MethodInfo;
    using SFT::Reflection::Multicast;
    using SFT::Reflection::TypeId;
    using SFT::Reflection::TypeInfo;
    using SFT::Reflection::TypeInfoBuilder;
    using SFT::Reflection::TypeRegistry;

    using SFT::Ffi::clear_error;
    using SFT::Ffi::copy_string_out;
    using SFT::Ffi::guarded;
    using SFT::Ffi::HandleKind;
    using SFT::Ffi::mint_handle;
    using SFT::Ffi::resolve_handle;
    using SFT::Ffi::revoke_handle;
    using SFT::Ffi::set_error;

    [[nodiscard]] TypeId to_type_id(SturdyReflectionId id) noexcept {
        return TypeId{.hash = {.high = id.high, .low = id.low}};
    }

    [[nodiscard]] SturdyReflectionId to_reflection_id(TypeId id) noexcept {
        return SturdyReflectionId{.high = id.hash.high, .low = id.hash.low};
    }

    [[nodiscard]] Multicast::Subscription to_subscription(SturdyReflectionSubscription subscription) noexcept {
        return Multicast::Subscription{.id = subscription.id};
    }

    [[nodiscard]] SturdyReflectionSubscription to_reflection_subscription(Multicast::Subscription subscription) noexcept {
        return SturdyReflectionSubscription{.id = subscription.id};
    }

    /// Resolves a type id to its `TypeInfo`, translating a miss into the ABI's error convention.
    [[nodiscard]] SturdyResult resolve_type(SturdyReflectionId type, const TypeInfo **out_type) noexcept {
        const TypeInfo *found = TypeRegistry::instance().find(to_type_id(type));
        if (found == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no reflected type is registered under that id");
        }
        *out_type = found;
        return STURDY_OK;
    }

    // ---- Owned type-builder storage, mirroring Gltf.cpp's g_scenes exactly ----

    std::mutex g_builder_mutex;
    std::unordered_map<SFT::u64, std::unique_ptr<TypeInfoBuilder>> g_builders;

    [[nodiscard]] SturdyResult resolve_builder(SturdyReflectionTypeBuilder builder, TypeInfoBuilder **out_builder) noexcept {
        void *pointer = nullptr;
        const SturdyResult resolved = resolve_handle(builder.token, HandleKind::ReflectionTypeBuilder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        *out_builder = static_cast<TypeInfoBuilder *>(pointer);
        return STURDY_OK;
    }

} // namespace

extern "C" {

// ---- Type lookup ----

SturdyResult STURDY_ABI_CALL sturdy_reflection_find_type(const char *name, SturdyReflectionId *out_type) {
    return guarded([&]() -> SturdyResult {
        if (name == nullptr || out_type == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name and output pointer must not be null");
        }
        const TypeInfo *found = TypeRegistry::instance().find(SFT::ustr{std::string_view{name}});
        if (found == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no reflected type is registered under that name");
        }
        *out_type = to_reflection_id(found->key);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_type_name(SturdyReflectionId type, char *buffer, size_t capacity, size_t *out_length) {
    return guarded([&]() -> SturdyResult {
        const TypeInfo *found = nullptr;
        const SturdyResult resolved = resolve_type(type, &found);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        return copy_string_out(found->canonical_name.cpp_string_view(), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_type_size(SturdyReflectionId type, uint32_t *out_size, uint32_t *out_align) {
    return guarded([&]() -> SturdyResult {
        const TypeInfo *found = nullptr;
        const SturdyResult resolved = resolve_type(type, &found);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (out_size != nullptr) {
            *out_size = static_cast<uint32_t>(found->size);
        }
        if (out_align != nullptr) {
            *out_align = static_cast<uint32_t>(found->align);
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_unregister_type(SturdyReflectionId type) {
    return guarded([&]() -> SturdyResult {
        if (!TypeRegistry::instance().unregister_type(to_type_id(type))) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no reflected type is registered under that id");
        }
        return STURDY_OK;
    });
}

// ---- Field/method/event lookup ----

SturdyResult STURDY_ABI_CALL sturdy_reflection_find_field(SturdyReflectionId type, const char *name, SturdyReflectionId *out_field) {
    return guarded([&]() -> SturdyResult {
        if (name == nullptr || out_field == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name and output pointer must not be null");
        }
        const TypeInfo *found_type = nullptr;
        const SturdyResult resolved = resolve_type(type, &found_type);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const FieldInfo *field = TypeRegistry::instance().find_field(*found_type, std::string_view{name});
        if (field == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such field on this type or its reflected base chain");
        }
        *out_field = to_reflection_id(field->key);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_field_info(SturdyReflectionId type, SturdyReflectionId field, SturdyReflectionFieldInfo *out_info) {
    return guarded([&]() -> SturdyResult {
        if (out_info == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        const TypeInfo *found_type = nullptr;
        const SturdyResult resolved = resolve_type(type, &found_type);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const FieldInfo *found_field = TypeRegistry::instance().find_field(*found_type, to_type_id(field));
        if (found_field == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such field on this type or its reflected base chain");
        }
        *out_info = SturdyReflectionFieldInfo{};
        out_info->struct_size = static_cast<uint32_t>(sizeof(SturdyReflectionFieldInfo));
        out_info->field = field;
        out_info->field_type = to_reflection_id(found_field->field_type);
        out_info->size = static_cast<uint32_t>(found_field->size);
        out_info->align = static_cast<uint32_t>(found_field->align);
        out_info->is_static = has_flag(found_field->flags, FieldFlags::Static) ? STURDY_TRUE : STURDY_FALSE;
        out_info->is_read_only = has_flag(found_field->flags, FieldFlags::ReadOnly) ? STURDY_TRUE : STURDY_FALSE;
        out_info->is_container = found_field->container != nullptr ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_find_method(SturdyReflectionId type, const char *name, SturdyReflectionId *out_method) {
    return guarded([&]() -> SturdyResult {
        if (name == nullptr || out_method == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name and output pointer must not be null");
        }
        const TypeInfo *found_type = nullptr;
        const SturdyResult resolved = resolve_type(type, &found_type);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const MethodInfo *method = TypeRegistry::instance().find_method(*found_type, std::string_view{name});
        if (method == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such method on this type or its reflected base chain");
        }
        *out_method = to_reflection_id(method->key);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_method_info(SturdyReflectionId type, SturdyReflectionId method, SturdyReflectionMethodInfo *out_info) {
    return guarded([&]() -> SturdyResult {
        if (out_info == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        const TypeInfo *found_type = nullptr;
        const SturdyResult resolved = resolve_type(type, &found_type);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const MethodInfo *found_method = TypeRegistry::instance().find_method(*found_type, to_type_id(method));
        if (found_method == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such method on this type or its reflected base chain");
        }
        *out_info = SturdyReflectionMethodInfo{};
        out_info->struct_size = static_cast<uint32_t>(sizeof(SturdyReflectionMethodInfo));
        out_info->method = method;
        out_info->return_type = to_reflection_id(found_method->return_type);
        out_info->param_count = static_cast<uint32_t>(found_method->param_types.size());
        out_info->is_static = found_method->is_static ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_find_event(SturdyReflectionId type, const char *name, SturdyReflectionId *out_event) {
    return guarded([&]() -> SturdyResult {
        if (name == nullptr || out_event == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name and output pointer must not be null");
        }
        const TypeInfo *found_type = nullptr;
        const SturdyResult resolved = resolve_type(type, &found_type);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const EventInfo *event = TypeRegistry::instance().find_event(*found_type, std::string_view{name});
        if (event == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such event on this type or its reflected base chain");
        }
        *out_event = to_reflection_id(event->key);
        return STURDY_OK;
    });
}

// ---- Field access ----

SturdyResult STURDY_ABI_CALL sturdy_reflection_get_field(SturdyReflectionId type, void *object, SturdyReflectionId field, void *out_data, uint32_t size) {
    return guarded([&]() -> SturdyResult {
        const TypeInfo *found_type = nullptr;
        const SturdyResult resolved = resolve_type(type, &found_type);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const FieldInfo *found_field = TypeRegistry::instance().find_field(*found_type, to_type_id(field));
        if (found_field == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such field on this type or its reflected base chain");
        }
        if (object == nullptr || out_data == nullptr || !copy_field_out(*found_field, object, out_data, size)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "null object/output, a size mismatch, or the field has no readable representation");
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_set_field(SturdyReflectionId type, void *object, SturdyReflectionId field, const void *data, uint32_t size) {
    return guarded([&]() -> SturdyResult {
        const TypeInfo *found_type = nullptr;
        const SturdyResult resolved = resolve_type(type, &found_type);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const FieldInfo *found_field = TypeRegistry::instance().find_field(*found_type, to_type_id(field));
        if (found_field == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such field on this type or its reflected base chain");
        }
        if (object == nullptr || data == nullptr || !copy_field_in(*found_field, object, data, size)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "null object/data, a size mismatch, the field is ReadOnly, or it has no writable representation");
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_get_static_field(SturdyReflectionId type, SturdyReflectionId field, void *out_data, uint32_t size) {
    return guarded([&]() -> SturdyResult {
        const TypeInfo *found_type = nullptr;
        const SturdyResult resolved = resolve_type(type, &found_type);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const FieldInfo *found_field = TypeRegistry::instance().find_field(*found_type, to_type_id(field));
        if (found_field == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such field on this type or its reflected base chain");
        }
        if (out_data == nullptr || !copy_static_field_out(*found_field, out_data, size)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "null output, a size mismatch, or a not-actually-static field");
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_set_static_field(SturdyReflectionId type, SturdyReflectionId field, const void *data, uint32_t size) {
    return guarded([&]() -> SturdyResult {
        const TypeInfo *found_type = nullptr;
        const SturdyResult resolved = resolve_type(type, &found_type);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const FieldInfo *found_field = TypeRegistry::instance().find_field(*found_type, to_type_id(field));
        if (found_field == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such field on this type or its reflected base chain");
        }
        if (data == nullptr || !copy_static_field_in(*found_field, data, size)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "null data, a size mismatch, ReadOnly, or a not-actually-static field");
        }
        return STURDY_OK;
    });
}

// ---- Container access ----

SturdyResult STURDY_ABI_CALL sturdy_reflection_container_size(SturdyReflectionId type, void *object, SturdyReflectionId field, uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        const TypeInfo *found_type = nullptr;
        const SturdyResult resolved = resolve_type(type, &found_type);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const FieldInfo *found_field = TypeRegistry::instance().find_field(*found_type, to_type_id(field));
        if (found_field == nullptr || found_field->container == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such container field on this type or its reflected base chain");
        }
        if (object == nullptr || out_count == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "object and output pointer must not be null");
        }
        *out_count = static_cast<uint32_t>(SFT::Reflection::container_size(found_field->container, object));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_container_get_element(SturdyReflectionId type, void *object, SturdyReflectionId field, uint32_t index, void *out_data, uint32_t element_size) {
    return guarded([&]() -> SturdyResult {
        const TypeInfo *found_type = nullptr;
        const SturdyResult resolved = resolve_type(type, &found_type);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const FieldInfo *found_field = TypeRegistry::instance().find_field(*found_type, to_type_id(field));
        if (found_field == nullptr || found_field->container == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such container field on this type or its reflected base chain");
        }
        if (object == nullptr || out_data == nullptr || found_field->container->element_size != element_size) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "null object/output, or element_size mismatch");
        }
        if (!SFT::Reflection::container_get_element(found_field->container, object, index, out_data)) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "index is out of range for this container's current size");
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_container_set_element(SturdyReflectionId type, void *object, SturdyReflectionId field, uint32_t index, const void *data, uint32_t element_size) {
    return guarded([&]() -> SturdyResult {
        const TypeInfo *found_type = nullptr;
        const SturdyResult resolved = resolve_type(type, &found_type);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const FieldInfo *found_field = TypeRegistry::instance().find_field(*found_type, to_type_id(field));
        if (found_field == nullptr || found_field->container == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such container field on this type or its reflected base chain");
        }
        if (object == nullptr || data == nullptr || found_field->container->element_size != element_size) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "null object/data, or element_size mismatch");
        }
        if (!SFT::Reflection::container_set_element(found_field->container, object, index, data)) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "index is out of range for this container's current size");
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_container_resize(SturdyReflectionId type, void *object, SturdyReflectionId field, uint32_t new_size) {
    return guarded([&]() -> SturdyResult {
        const TypeInfo *found_type = nullptr;
        const SturdyResult resolved = resolve_type(type, &found_type);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const FieldInfo *found_field = TypeRegistry::instance().find_field(*found_type, to_type_id(field));
        if (found_field == nullptr || found_field->container == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such container field on this type or its reflected base chain");
        }
        if (object == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "object must not be null");
        }
        if (!SFT::Reflection::container_resize(found_field->container, object, new_size)) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "this container's element type has no default constructor");
        }
        return STURDY_OK;
    });
}

// ---- Method invocation ----

SturdyResult STURDY_ABI_CALL sturdy_reflection_invoke_method(SturdyReflectionId type, void *object, SturdyReflectionId method, const void *const *args, uint32_t arg_count, void *out_return, uint32_t out_return_size) {
    (void)out_return_size;
    return guarded([&]() -> SturdyResult {
        const TypeInfo *found_type = nullptr;
        const SturdyResult resolved = resolve_type(type, &found_type);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const MethodInfo *found_method = TypeRegistry::instance().find_method(*found_type, to_type_id(method));
        if (found_method == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such method on this type or its reflected base chain");
        }
        if (object == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "object must not be null");
        }
        SFT::Reflection::InvokeException thrown{};
        if (!SFT::Reflection::invoke_method(*found_method, object, args, arg_count, out_return, &thrown)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "arg_count did not match the method's declared arity");
        }
        if (thrown.threw) {
            return set_error(STURDY_ERROR_CALLBACK_FAILED, thrown.message.cpp_string_view());
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_invoke_static_method(SturdyReflectionId type, SturdyReflectionId method, const void *const *args, uint32_t arg_count, void *out_return, uint32_t out_return_size) {
    (void)out_return_size;
    return guarded([&]() -> SturdyResult {
        const TypeInfo *found_type = nullptr;
        const SturdyResult resolved = resolve_type(type, &found_type);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const MethodInfo *found_method = TypeRegistry::instance().find_method(*found_type, to_type_id(method));
        if (found_method == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such method on this type or its reflected base chain");
        }
        SFT::Reflection::InvokeException thrown{};
        if (!SFT::Reflection::invoke_static_method(*found_method, args, arg_count, out_return, &thrown)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "arg_count did not match the method's declared arity");
        }
        if (thrown.threw) {
            return set_error(STURDY_ERROR_CALLBACK_FAILED, thrown.message.cpp_string_view());
        }
        return STURDY_OK;
    });
}

// ---- Overrides and hooks ----

SturdyResult STURDY_ABI_CALL sturdy_reflection_register_override(SturdyReflectionId type, SturdyReflectionId method, SturdyReflectionMethodFn override_fn, void *user_data) {
    return guarded([&]() -> SturdyResult {
        if (override_fn == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "override_fn must not be null");
        }
        if (!TypeRegistry::instance().set_method_override(
                to_type_id(type), to_type_id(method), reinterpret_cast<SFT::Reflection::MethodInvokeFn>(override_fn), user_data)) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such type/method");
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_clear_override(SturdyReflectionId type, SturdyReflectionId method) {
    return guarded([&]() -> SturdyResult {
        if (!TypeRegistry::instance().clear_method_override(to_type_id(type), to_type_id(method))) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such type/method");
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_add_before_hook(SturdyReflectionId type, SturdyReflectionId method, SturdyReflectionObserverFn hook, void *user_data, SturdyReflectionSubscription *out_subscription) {
    return guarded([&]() -> SturdyResult {
        if (hook == nullptr || out_subscription == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "hook and output pointer must not be null");
        }
        auto subscription = TypeRegistry::instance().add_method_before_hook(
            to_type_id(type), to_type_id(method), reinterpret_cast<Multicast::ListenerFn>(hook), user_data);
        if (!subscription.has_value()) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such type/method");
        }
        *out_subscription = to_reflection_subscription(*subscription);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_remove_before_hook(SturdyReflectionId type, SturdyReflectionId method, SturdyReflectionSubscription subscription) {
    return guarded([&]() -> SturdyResult {
        if (!TypeRegistry::instance().remove_method_before_hook(to_type_id(type), to_type_id(method), to_subscription(subscription))) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such type/method/subscription");
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_add_after_hook(SturdyReflectionId type, SturdyReflectionId method, SturdyReflectionObserverFn hook, void *user_data, SturdyReflectionSubscription *out_subscription) {
    return guarded([&]() -> SturdyResult {
        if (hook == nullptr || out_subscription == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "hook and output pointer must not be null");
        }
        auto subscription = TypeRegistry::instance().add_method_after_hook(
            to_type_id(type), to_type_id(method), reinterpret_cast<Multicast::ListenerFn>(hook), user_data);
        if (!subscription.has_value()) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such type/method");
        }
        *out_subscription = to_reflection_subscription(*subscription);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_remove_after_hook(SturdyReflectionId type, SturdyReflectionId method, SturdyReflectionSubscription subscription) {
    return guarded([&]() -> SturdyResult {
        if (!TypeRegistry::instance().remove_method_after_hook(to_type_id(type), to_type_id(method), to_subscription(subscription))) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such type/method/subscription");
        }
        return STURDY_OK;
    });
}

// ---- Events ----

SturdyResult STURDY_ABI_CALL sturdy_reflection_subscribe_event(SturdyReflectionId type, SturdyReflectionId event, SturdyReflectionObserverFn listener, void *user_data, SturdyReflectionSubscription *out_subscription) {
    return guarded([&]() -> SturdyResult {
        if (listener == nullptr || out_subscription == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "listener and output pointer must not be null");
        }
        auto subscription = TypeRegistry::instance().subscribe_event(
            to_type_id(type), to_type_id(event), reinterpret_cast<Multicast::ListenerFn>(listener), user_data);
        if (!subscription.has_value()) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such type/event");
        }
        *out_subscription = to_reflection_subscription(*subscription);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_unsubscribe_event(SturdyReflectionId type, SturdyReflectionId event, SturdyReflectionSubscription subscription) {
    return guarded([&]() -> SturdyResult {
        if (!TypeRegistry::instance().unsubscribe_event(to_type_id(type), to_type_id(event), to_subscription(subscription))) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such type/event/subscription");
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_fire_event(SturdyReflectionId type, void *object, SturdyReflectionId event, const void *const *args, uint32_t arg_count) {
    (void)arg_count;
    return guarded([&]() -> SturdyResult {
        const TypeInfo *found_type = nullptr;
        const SturdyResult resolved = resolve_type(type, &found_type);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const EventInfo *found_event = TypeRegistry::instance().find_event(*found_type, to_type_id(event));
        if (found_event == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such event on this type or its reflected base chain");
        }
        SFT::Reflection::fire_event(*found_event, object, args);
        return STURDY_OK;
    });
}

// ---- Dynamic type registration ----

SturdyResult STURDY_ABI_CALL sturdy_reflection_type_builder_create(const char *canonical_name, uint32_t size, uint32_t align, SturdyReflectionTypeBuilder *out_builder) {
    return guarded([&]() -> SturdyResult {
        if (canonical_name == nullptr || out_builder == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "canonical_name and output pointer must not be null");
        }
        auto owned = std::make_unique<TypeInfoBuilder>(std::string_view{canonical_name}, size, align);
        void *pointer = owned.get();
        const SFT::u64 token = mint_handle(HandleKind::ReflectionTypeBuilder, pointer);
        {
            const std::lock_guard<std::mutex> lock{g_builder_mutex};
            g_builders.emplace(token, std::move(owned));
        }
        out_builder->token = token;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_type_builder_set_constructors(SturdyReflectionTypeBuilder builder,
                                                                             SturdyReflectionMoveConstructFn move_construct,
                                                                             SturdyReflectionDestroyFn destroy,
                                                                             SturdyReflectionDefaultConstructFn default_construct,
                                                                             SturdyReflectionCopyConstructFn copy_construct,
                                                                             void *user_data) {
    return guarded([&]() -> SturdyResult {
        TypeInfoBuilder *resolved_builder = nullptr;
        const SturdyResult resolved = resolve_builder(builder, &resolved_builder);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (move_construct == nullptr || destroy == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "move_construct and destroy must not be null");
        }
        resolved_builder->constructors(
            reinterpret_cast<SFT::Reflection::TypeMoveConstructFn>(move_construct),
            reinterpret_cast<SFT::Reflection::TypeDestroyFn>(destroy),
            reinterpret_cast<SFT::Reflection::TypeDefaultConstructFn>(default_construct),
            reinterpret_cast<SFT::Reflection::TypeCopyConstructFn>(copy_construct),
            user_data);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_type_builder_add_field(SturdyReflectionTypeBuilder builder,
                                                                      const char *name,
                                                                      uint32_t offset,
                                                                      uint32_t size,
                                                                      uint32_t align,
                                                                      SturdyReflectionId field_type,
                                                                      SturdyBool is_trivial,
                                                                      SturdyReflectionFieldGetFn copy_get,
                                                                      SturdyReflectionFieldSetFn copy_set) {
    return guarded([&]() -> SturdyResult {
        TypeInfoBuilder *resolved_builder = nullptr;
        const SturdyResult resolved = resolve_builder(builder, &resolved_builder);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (name == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name must not be null");
        }
        const FieldFlags flags = (is_trivial != STURDY_FALSE) ? FieldFlags::Trivial : FieldFlags::None;
        resolved_builder->field(std::string_view{name}, offset, size, align, to_type_id(field_type), flags,
                                reinterpret_cast<SFT::Reflection::FieldCopyGetFn>(copy_get),
                                reinterpret_cast<SFT::Reflection::FieldCopySetFn>(copy_set));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_type_builder_add_method(SturdyReflectionTypeBuilder builder,
                                                                       const char *name,
                                                                       SturdyReflectionId return_type,
                                                                       const SturdyReflectionId *param_types,
                                                                       uint32_t param_count,
                                                                       SturdyReflectionMethodFn invoke) {
    return guarded([&]() -> SturdyResult {
        TypeInfoBuilder *resolved_builder = nullptr;
        const SturdyResult resolved = resolve_builder(builder, &resolved_builder);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (name == nullptr || invoke == nullptr || (param_count != 0 && param_types == nullptr)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name, invoke, and param_types (if param_count != 0) must not be null");
        }
        std::vector<TypeId> params;
        params.reserve(param_count);
        for (uint32_t i = 0; i < param_count; ++i) {
            params.push_back(to_type_id(param_types[i]));
        }
        resolved_builder->method(std::string_view{name}, to_type_id(return_type), std::move(params),
                                 reinterpret_cast<SFT::Reflection::MethodInvokeFn>(invoke));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_type_builder_add_event(SturdyReflectionTypeBuilder builder,
                                                                      const char *name,
                                                                      const SturdyReflectionId *param_types,
                                                                      uint32_t param_count) {
    return guarded([&]() -> SturdyResult {
        TypeInfoBuilder *resolved_builder = nullptr;
        const SturdyResult resolved = resolve_builder(builder, &resolved_builder);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (name == nullptr || (param_count != 0 && param_types == nullptr)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name and param_types (if param_count != 0) must not be null");
        }
        std::vector<TypeId> params;
        params.reserve(param_count);
        for (uint32_t i = 0; i < param_count; ++i) {
            params.push_back(to_type_id(param_types[i]));
        }
        resolved_builder->event(std::string_view{name}, std::move(params));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_type_builder_finish(SturdyReflectionTypeBuilder builder, SturdyReflectionId *out_type) {
    return guarded([&]() -> SturdyResult {
        TypeInfoBuilder *resolved_builder = nullptr;
        const SturdyResult resolved = resolve_builder(builder, &resolved_builder);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (out_type == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        TypeInfo info = resolved_builder->build();
        // Revoked/erased regardless of outcome: a finished builder (successfully or not) is
        // consumed, matching the C++ TypeInfoBuilder::build() contract of "don't reuse this".
        revoke_handle(builder.token);
        {
            const std::lock_guard<std::mutex> lock{g_builder_mutex};
            g_builders.erase(builder.token);
        }

        auto registered = TypeRegistry::instance().register_type(std::move(info));
        if (!registered) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, registered.error().message.cpp_string_view());
        }
        *out_type = to_reflection_id(*registered);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_reflection_type_builder_discard(SturdyReflectionTypeBuilder builder) {
    return guarded([&]() -> SturdyResult {
        void *pointer = nullptr;
        if (resolve_handle(builder.token, HandleKind::ReflectionTypeBuilder, &pointer) != STURDY_OK) {
            clear_error();
            return STURDY_OK; // already discarded/finished or never valid: a no-op, not an error
        }
        revoke_handle(builder.token);
        const std::lock_guard<std::mutex> lock{g_builder_mutex};
        g_builders.erase(builder.token);
        return STURDY_OK;
    });
}

} // extern "C"
