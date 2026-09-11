#include <Reflection/TypeInfo.hpp>

namespace SFT::Reflection {

    const FieldInfo *TypeInfo::find_field(TypeId field_key) const noexcept {
        for (const FieldInfo &field : fields) {
            if (field.key == field_key) {
                return &field;
            }
        }
        for (const FieldInfo &field : static_fields) {
            if (field.key == field_key) {
                return &field;
            }
        }
        return nullptr;
    }

    const FieldInfo *TypeInfo::find_field(std::string_view field_name) const noexcept {
        return find_field(TypeId::from_name(field_name));
    }

    const MethodInfo *TypeInfo::find_method(TypeId method_key) const noexcept {
        for (const MethodInfo &method : methods) {
            if (method.key == method_key) {
                return &method;
            }
        }
        for (const MethodInfo &method : static_methods) {
            if (method.key == method_key) {
                return &method;
            }
        }
        return nullptr;
    }

    MethodInfo *TypeInfo::find_method(TypeId method_key) noexcept {
        for (MethodInfo &method : methods) {
            if (method.key == method_key) {
                return &method;
            }
        }
        for (MethodInfo &method : static_methods) {
            if (method.key == method_key) {
                return &method;
            }
        }
        return nullptr;
    }

    const MethodInfo *TypeInfo::find_method(std::string_view method_name) const noexcept {
        for (const MethodInfo &method : methods) {
            if (method.name.cpp_string_view() == method_name) {
                return &method;
            }
        }
        for (const MethodInfo &method : static_methods) {
            if (method.name.cpp_string_view() == method_name) {
                return &method;
            }
        }
        return nullptr;
    }

    const MethodInfo *TypeInfo::find_method(std::string_view method_name, std::span<const TypeId> param_types) const noexcept {
        auto matches = [&](const MethodInfo &method) noexcept {
            if (method.name.cpp_string_view() != method_name || method.param_types.size() != param_types.size()) {
                return false;
            }
            for (usize i = 0; i < param_types.size(); ++i) {
                if (method.param_types[i] != param_types[i]) {
                    return false;
                }
            }
            return true;
        };
        for (const MethodInfo &method : methods) {
            if (matches(method)) {
                return &method;
            }
        }
        for (const MethodInfo &method : static_methods) {
            if (matches(method)) {
                return &method;
            }
        }
        return nullptr;
    }

    const EventInfo *TypeInfo::find_event(TypeId event_key) const noexcept {
        for (const EventInfo &event : events) {
            if (event.key == event_key) {
                return &event;
            }
        }
        return nullptr;
    }

    const EventInfo *TypeInfo::find_event(std::string_view event_name) const noexcept {
        return find_event(TypeId::from_name(event_name));
    }

    const ConstructorInfo *TypeInfo::find_constructor(std::span<const TypeId> param_types) const noexcept {
        for (const ConstructorInfo &ctor : constructors) {
            if (ctor.param_types.size() != param_types.size()) {
                continue;
            }
            bool matches = true;
            for (usize i = 0; i < param_types.size(); ++i) {
                if (ctor.param_types[i] != param_types[i]) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                return &ctor;
            }
        }
        return nullptr;
    }

} // namespace SFT::Reflection
