#include <Reflection/Document.hpp>

#include <cstring>

namespace SFT::Reflection {

    namespace {

        [[nodiscard]] DocumentError document_error(DocumentErrorCode code, std::string_view message) {
            return DocumentError{.code = code, .message = UString{message}};
        }

        [[nodiscard]] TypeId ustring_type_id() {
            static const TypeId id = type_id_for<UString>();
            return id;
        }

        /// Reads `size` bytes at `ptr` as `kind` and builds the matching `Value`.
        [[nodiscard]] Value value_from_primitive(PrimitiveKind kind, const void *ptr, usize size) noexcept {
            switch (kind) {
            case PrimitiveKind::Bool: {
                bool value = false;
                std::memcpy(&value, ptr, sizeof(value));
                return Value(value);
            }
            case PrimitiveKind::SignedInt: {
                switch (size) {
                case 1: {
                    i8 value = 0;
                    std::memcpy(&value, ptr, 1);
                    return Value(static_cast<i64>(value));
                }
                case 2: {
                    i16 value = 0;
                    std::memcpy(&value, ptr, 2);
                    return Value(static_cast<i64>(value));
                }
                case 4: {
                    i32 value = 0;
                    std::memcpy(&value, ptr, 4);
                    return Value(static_cast<i64>(value));
                }
                default: {
                    i64 value = 0;
                    std::memcpy(&value, ptr, 8);
                    return Value(value);
                }
                }
            }
            case PrimitiveKind::UnsignedInt: {
                switch (size) {
                case 1: {
                    u8 value = 0;
                    std::memcpy(&value, ptr, 1);
                    return Value(static_cast<i64>(value));
                }
                case 2: {
                    u16 value = 0;
                    std::memcpy(&value, ptr, 2);
                    return Value(static_cast<i64>(value));
                }
                case 4: {
                    u32 value = 0;
                    std::memcpy(&value, ptr, 4);
                    return Value(static_cast<i64>(value));
                }
                default: {
                    // A u64 above i64's range loses its top bit here — an accepted v1 limitation
                    // of representing every integer kind through one Value::Int (i64) slot.
                    u64 value = 0;
                    std::memcpy(&value, ptr, 8);
                    return Value(static_cast<i64>(value));
                }
                }
            }
            case PrimitiveKind::Float: {
                if (size == 4) {
                    float value = 0.0f;
                    std::memcpy(&value, ptr, 4);
                    return Value(static_cast<f64>(value));
                }
                double value = 0.0;
                std::memcpy(&value, ptr, 8);
                return Value(value);
            }
            case PrimitiveKind::None:
            default:
                return Value();
            }
        }

        /// Writes `value` into `size` bytes at `ptr`, interpreting them as `kind`.
        ///
        /// @return Returns `true` on success; `false` when `value`'s `ValueKind` doesn't match
        /// `kind` (a `Bool` field fed a JSON string, say).
        [[nodiscard]] bool write_primitive(PrimitiveKind kind, void *ptr, usize size, const Value &value) noexcept {
            switch (kind) {
            case PrimitiveKind::Bool: {
                if (!value.is_bool()) {
                    return false;
                }
                const bool typed_value = value.as_bool();
                std::memcpy(ptr, &typed_value, sizeof(typed_value));
                return true;
            }
            case PrimitiveKind::SignedInt: {
                if (!value.is_number()) {
                    return false;
                }
                const i64 wide_value = value.is_int() ? value.as_int() : static_cast<i64>(value.as_float());
                switch (size) {
                case 1: {
                    const auto narrow = static_cast<i8>(wide_value);
                    std::memcpy(ptr, &narrow, 1);
                    break;
                }
                case 2: {
                    const auto narrow = static_cast<i16>(wide_value);
                    std::memcpy(ptr, &narrow, 2);
                    break;
                }
                case 4: {
                    const auto narrow = static_cast<i32>(wide_value);
                    std::memcpy(ptr, &narrow, 4);
                    break;
                }
                default:
                    std::memcpy(ptr, &wide_value, 8);
                    break;
                }
                return true;
            }
            case PrimitiveKind::UnsignedInt: {
                if (!value.is_number()) {
                    return false;
                }
                const i64 wide_value = value.is_int() ? value.as_int() : static_cast<i64>(value.as_float());
                switch (size) {
                case 1: {
                    const auto narrow = static_cast<u8>(wide_value);
                    std::memcpy(ptr, &narrow, 1);
                    break;
                }
                case 2: {
                    const auto narrow = static_cast<u16>(wide_value);
                    std::memcpy(ptr, &narrow, 2);
                    break;
                }
                case 4: {
                    const auto narrow = static_cast<u32>(wide_value);
                    std::memcpy(ptr, &narrow, 4);
                    break;
                }
                default: {
                    const auto wide_unsigned = static_cast<u64>(wide_value);
                    std::memcpy(ptr, &wide_unsigned, 8);
                    break;
                }
                }
                return true;
            }
            case PrimitiveKind::Float: {
                if (!value.is_number()) {
                    return false;
                }
                const f64 wide_value = value.as_number();
                if (size == 4) {
                    const auto narrow = static_cast<float>(wide_value);
                    std::memcpy(ptr, &narrow, 4);
                } else {
                    std::memcpy(ptr, &wide_value, 8);
                }
                return true;
            }
            case PrimitiveKind::None:
            default:
                return false;
            }
        }

        [[nodiscard]] bool default_construct_scratch_value(TypeId value_type, void *destination) {
            if (value_type == ustring_type_id()) {
                ::new (destination) UString();
                return true;
            }
            const TypeInfo *nested = TypeRegistry::instance().find(value_type);
            return nested != nullptr && default_construct_instance(*nested, destination);
        }

        void destroy_scratch_value(TypeId value_type, void *object) noexcept {
            if (value_type == ustring_type_id()) {
                static_cast<UString *>(object)->~UString();
                return;
            }
            if (const TypeInfo *nested = TypeRegistry::instance().find(value_type); nested != nullptr) {
                destroy_instance(*nested, object);
            }
        }

        [[nodiscard]] DocumentExpected<Value> document_from_non_trivial_value(TypeId value_type, const void *value_ptr) {
            if (value_type == ustring_type_id()) {
                return Value(static_cast<const UString *>(value_ptr)->cpp_string_view());
            }
            const TypeInfo *nested = TypeRegistry::instance().find(value_type);
            if (nested == nullptr) {
                return std::unexpected(document_error(DocumentErrorCode::NestedTypeNotRegistered,
                                                       "value's declared type is not Trivial, not UString, and is not a registered TypeInfo"));
            }
            return to_document(*nested, value_ptr);
        }

        [[nodiscard]] DocumentExpected<void> non_trivial_value_from_document(TypeId value_type, void *value_ptr, const Value &document) {
            if (value_type == ustring_type_id()) {
                if (!document.is_string()) {
                    return std::unexpected(document_error(DocumentErrorCode::TypeMismatch, "expected a string"));
                }
                *static_cast<UString *>(value_ptr) = document.as_string();
                return {};
            }
            const TypeInfo *nested = TypeRegistry::instance().find(value_type);
            if (nested == nullptr) {
                return std::unexpected(document_error(DocumentErrorCode::NestedTypeNotRegistered,
                                                       "value's declared type is not Trivial, not UString, and is not a registered TypeInfo"));
            }
            if (!document.is_object()) {
                return std::unexpected(document_error(DocumentErrorCode::TypeMismatch, "expected an object"));
            }
            return from_document(*nested, value_ptr, document);
        }

        [[nodiscard]] DocumentExpected<Value> document_from_container(const FieldInfo &field, const void *field_ptr) {
            const ContainerInfo &container = *field.container;
            if (container.data == nullptr) {
                return std::unexpected(document_error(DocumentErrorCode::UnsupportedFieldType, "container field has no readable storage accessor"));
            }
            Value array = Value::make_array();
            const usize count = container_size(field.container, field_ptr);
            const auto *base = static_cast<const unsigned char *>(container.data(field_ptr));
            for (usize i = 0; i < count; ++i) {
                const void *element_ptr = base + i * container.element_size;
                if (container.element_trivial) {
                    array.push_back(value_from_primitive(container.element_primitive_kind, element_ptr, container.element_size));
                } else {
                    auto element_document = document_from_non_trivial_value(container.element_type, element_ptr);
                    if (!element_document) {
                        return std::unexpected(element_document.error());
                    }
                    array.push_back(std::move(*element_document));
                }
            }
            return array;
        }

        [[nodiscard]] DocumentExpected<void> container_from_document(const FieldInfo &field, void *field_ptr, const Value &document) {
            const ContainerInfo &container = *field.container;
            if (!document.is_array()) {
                return std::unexpected(document_error(DocumentErrorCode::TypeMismatch, "expected an array"));
            }
            if (!container_resize(field.container, field_ptr, document.size())) {
                return std::unexpected(document_error(DocumentErrorCode::NotDefaultConstructible,
                                                       "container field's element type has no default constructor"));
            }
            for (usize i = 0; i < document.size(); ++i) {
                const Value &element_document = document.at(i);
                if (container.element_trivial) {
                    std::vector<std::byte> scratch(container.element_size);
                    if (!write_primitive(container.element_primitive_kind, scratch.data(), container.element_size, element_document)) {
                        return std::unexpected(document_error(DocumentErrorCode::TypeMismatch, "container element type mismatch"));
                    }
                    if (!container_set_element(field.container, field_ptr, i, scratch.data())) {
                        return std::unexpected(document_error(DocumentErrorCode::UnsupportedFieldType, "container element could not be set"));
                    }
                } else {
                    std::vector<std::byte> scratch(container.element_size);
                    if (!default_construct_scratch_value(container.element_type, scratch.data())) {
                        return std::unexpected(document_error(DocumentErrorCode::NotDefaultConstructible, "container element type has no default constructor"));
                    }
                    auto result = non_trivial_value_from_document(container.element_type, scratch.data(), element_document);
                    if (!result) {
                        destroy_scratch_value(container.element_type, scratch.data());
                        return std::unexpected(result.error());
                    }
                    (void)container_set_element(field.container, field_ptr, i, scratch.data());
                    destroy_scratch_value(container.element_type, scratch.data());
                }
            }
            return {};
        }

        struct MapDocumentContext {
            Value *result;
            const MapInfo *map;
            bool string_keyed;
            DocumentError error{};
            bool failed = false;
        };

        [[nodiscard]] DocumentExpected<Value> document_from_map(const FieldInfo &field, const void *field_ptr) {
            const MapInfo &map = *field.map;
            if (map.for_each == nullptr) {
                return std::unexpected(document_error(DocumentErrorCode::UnsupportedFieldType, "map field has no readable representation"));
            }
            // Only a UString-keyed map becomes a real {"key": value, ...} object — a text
            // document's object keys must be strings. Any other key type becomes an array of
            // [key, value] pairs instead, which needs no key-to-string/string-to-key conversion
            // (and so no risk of two different keys stringifying to the same text).
            const bool string_keyed = map.key_type == ustring_type_id();
            Value result = string_keyed ? Value::make_object() : Value::make_array();

            MapDocumentContext context{.result = &result, .map = &map, .string_keyed = string_keyed};
            map.for_each(
                field_ptr,
                [](const void *key, const void *value, void *user_data) noexcept {
                    auto *ctx = static_cast<MapDocumentContext *>(user_data);
                    if (ctx->failed) {
                        return;
                    }
                    Value key_document;
                    if (ctx->map->key_trivial) {
                        key_document = value_from_primitive(ctx->map->key_primitive_kind, key, ctx->map->key_size);
                    } else {
                        auto result = document_from_non_trivial_value(ctx->map->key_type, key);
                        if (!result) {
                            ctx->failed = true;
                            ctx->error = result.error();
                            return;
                        }
                        key_document = std::move(*result);
                    }
                    Value value_document;
                    if (ctx->map->value_trivial) {
                        value_document = value_from_primitive(ctx->map->value_primitive_kind, value, ctx->map->value_size);
                    } else {
                        auto result = document_from_non_trivial_value(ctx->map->value_type, value);
                        if (!result) {
                            ctx->failed = true;
                            ctx->error = result.error();
                            return;
                        }
                        value_document = std::move(*result);
                    }
                    if (ctx->string_keyed) {
                        ctx->result->set(key_document.as_string().cpp_string_view(), std::move(value_document));
                    } else {
                        Value pair = Value::make_array();
                        pair.push_back(std::move(key_document));
                        pair.push_back(std::move(value_document));
                        ctx->result->push_back(std::move(pair));
                    }
                },
                &context);

            if (context.failed) {
                return std::unexpected(context.error);
            }
            return result;
        }

        [[nodiscard]] DocumentExpected<void> map_key_value_from_documents(const MapInfo &map, void *field_ptr, const Value &key_document, const Value &value_document) {
            std::vector<std::byte> key_storage(map.key_size);
            if (map.key_trivial) {
                if (!write_primitive(map.key_primitive_kind, key_storage.data(), map.key_size, key_document)) {
                    return std::unexpected(document_error(DocumentErrorCode::TypeMismatch, "map key type mismatch"));
                }
            } else {
                if (!default_construct_scratch_value(map.key_type, key_storage.data())) {
                    return std::unexpected(document_error(DocumentErrorCode::NotDefaultConstructible, "map field's key type has no default constructor"));
                }
                auto result = non_trivial_value_from_document(map.key_type, key_storage.data(), key_document);
                if (!result) {
                    destroy_scratch_value(map.key_type, key_storage.data());
                    return std::unexpected(result.error());
                }
            }

            std::vector<std::byte> value_storage(map.value_size);
            if (map.value_trivial) {
                if (!write_primitive(map.value_primitive_kind, value_storage.data(), map.value_size, value_document)) {
                    if (!map.key_trivial) {
                        destroy_scratch_value(map.key_type, key_storage.data());
                    }
                    return std::unexpected(document_error(DocumentErrorCode::TypeMismatch, "map value type mismatch"));
                }
            } else {
                if (!default_construct_scratch_value(map.value_type, value_storage.data())) {
                    if (!map.key_trivial) {
                        destroy_scratch_value(map.key_type, key_storage.data());
                    }
                    return std::unexpected(document_error(DocumentErrorCode::NotDefaultConstructible, "map field's value type has no default constructor"));
                }
                auto result = non_trivial_value_from_document(map.value_type, value_storage.data(), value_document);
                if (!result) {
                    destroy_scratch_value(map.value_type, value_storage.data());
                    if (!map.key_trivial) {
                        destroy_scratch_value(map.key_type, key_storage.data());
                    }
                    return std::unexpected(result.error());
                }
            }

            map.insert_or_assign(field_ptr, key_storage.data(), value_storage.data());
            if (!map.key_trivial) {
                destroy_scratch_value(map.key_type, key_storage.data());
            }
            if (!map.value_trivial) {
                destroy_scratch_value(map.value_type, value_storage.data());
            }
            return {};
        }

        [[nodiscard]] DocumentExpected<void> map_from_document(const FieldInfo &field, void *field_ptr, const Value &document) {
            const MapInfo &map = *field.map;
            if (map.clear == nullptr || map.insert_or_assign == nullptr) {
                return std::unexpected(document_error(DocumentErrorCode::UnsupportedFieldType, "map field has no writable representation"));
            }
            const bool string_keyed = map.key_type == ustring_type_id();
            map.clear(field_ptr);

            if (string_keyed) {
                if (!document.is_object()) {
                    return std::unexpected(document_error(DocumentErrorCode::TypeMismatch, "expected an object"));
                }
                for (usize i = 0; i < document.size(); ++i) {
                    const Value key_document{document.key_at(i).cpp_string_view()};
                    auto result = map_key_value_from_documents(map, field_ptr, key_document, document.value_at(i));
                    if (!result) {
                        return result;
                    }
                }
                return {};
            }

            if (!document.is_array()) {
                return std::unexpected(document_error(DocumentErrorCode::TypeMismatch, "expected an array of [key, value] pairs"));
            }
            for (usize i = 0; i < document.size(); ++i) {
                const Value &pair = document.at(i);
                if (!pair.is_array() || pair.size() != 2) {
                    return std::unexpected(document_error(DocumentErrorCode::TypeMismatch, "expected a two-element [key, value] pair"));
                }
                auto result = map_key_value_from_documents(map, field_ptr, pair.at(0), pair.at(1));
                if (!result) {
                    return result;
                }
            }
            return {};
        }

        [[nodiscard]] DocumentExpected<Value> document_from_optional(const FieldInfo &field, const void *field_ptr) {
            const OptionalInfo &optional = *field.optional;
            if (optional.has_value == nullptr || optional.data == nullptr) {
                return std::unexpected(document_error(DocumentErrorCode::UnsupportedFieldType, "optional field has no readable representation"));
            }
            if (!optional.has_value(field_ptr)) {
                return Value();
            }
            const void *value_ptr = optional.data(field_ptr);
            if (optional.value_trivial) {
                return value_from_primitive(optional.value_primitive_kind, value_ptr, optional.value_size);
            }
            return document_from_non_trivial_value(optional.value_type, value_ptr);
        }

        [[nodiscard]] DocumentExpected<void> optional_from_document(const FieldInfo &field, void *field_ptr, const Value &document) {
            const OptionalInfo &optional = *field.optional;
            if (optional.reset == nullptr || optional.emplace_copy == nullptr) {
                return std::unexpected(document_error(DocumentErrorCode::UnsupportedFieldType, "optional field has no writable representation"));
            }
            if (document.is_null()) {
                optional.reset(field_ptr);
                return {};
            }
            if (optional.value_trivial) {
                std::vector<std::byte> scratch(optional.value_size);
                if (!write_primitive(optional.value_primitive_kind, scratch.data(), optional.value_size, document)) {
                    return std::unexpected(document_error(DocumentErrorCode::TypeMismatch, "optional value type mismatch"));
                }
                optional.emplace_copy(field_ptr, scratch.data());
                return {};
            }
            std::vector<std::byte> scratch(optional.value_size);
            if (!default_construct_scratch_value(optional.value_type, scratch.data())) {
                return std::unexpected(document_error(DocumentErrorCode::NotDefaultConstructible, "optional field's value type has no default constructor"));
            }
            auto result = non_trivial_value_from_document(optional.value_type, scratch.data(), document);
            if (!result) {
                destroy_scratch_value(optional.value_type, scratch.data());
                return std::unexpected(result.error());
            }
            optional.emplace_copy(field_ptr, scratch.data());
            destroy_scratch_value(optional.value_type, scratch.data());
            return {};
        }

        struct SetDocumentContext {
            Value *result;
            const SetInfo *set;
            DocumentError error{};
            bool failed = false;
        };

        [[nodiscard]] DocumentExpected<Value> document_from_set(const FieldInfo &field, const void *field_ptr) {
            const SetInfo &set = *field.set;
            if (set.for_each == nullptr) {
                return std::unexpected(document_error(DocumentErrorCode::UnsupportedFieldType, "set field has no readable representation"));
            }
            Value array = Value::make_array();
            SetDocumentContext context{.result = &array, .set = &set};
            set.for_each(
                field_ptr,
                [](const void *element, void *user_data) noexcept {
                    auto *ctx = static_cast<SetDocumentContext *>(user_data);
                    if (ctx->failed) {
                        return;
                    }
                    if (ctx->set->element_trivial) {
                        ctx->result->push_back(value_from_primitive(ctx->set->element_primitive_kind, element, ctx->set->element_size));
                        return;
                    }
                    auto element_document = document_from_non_trivial_value(ctx->set->element_type, element);
                    if (!element_document) {
                        ctx->failed = true;
                        ctx->error = element_document.error();
                        return;
                    }
                    ctx->result->push_back(std::move(*element_document));
                },
                &context);
            if (context.failed) {
                return std::unexpected(context.error);
            }
            return array;
        }

        [[nodiscard]] DocumentExpected<void> set_from_document(const FieldInfo &field, void *field_ptr, const Value &document) {
            const SetInfo &set = *field.set;
            if (set.clear == nullptr || set.insert == nullptr) {
                return std::unexpected(document_error(DocumentErrorCode::UnsupportedFieldType, "set field has no writable representation"));
            }
            if (!document.is_array()) {
                return std::unexpected(document_error(DocumentErrorCode::TypeMismatch, "expected an array"));
            }
            set.clear(field_ptr);
            for (usize i = 0; i < document.size(); ++i) {
                const Value &element_document = document.at(i);
                std::vector<std::byte> scratch(set.element_size);
                if (set.element_trivial) {
                    if (!write_primitive(set.element_primitive_kind, scratch.data(), set.element_size, element_document)) {
                        return std::unexpected(document_error(DocumentErrorCode::TypeMismatch, "set element type mismatch"));
                    }
                } else {
                    if (!default_construct_scratch_value(set.element_type, scratch.data())) {
                        return std::unexpected(document_error(DocumentErrorCode::NotDefaultConstructible, "set field's element type has no default constructor"));
                    }
                    auto result = non_trivial_value_from_document(set.element_type, scratch.data(), element_document);
                    if (!result) {
                        destroy_scratch_value(set.element_type, scratch.data());
                        return std::unexpected(result.error());
                    }
                }
                set.insert(field_ptr, scratch.data());
                if (!set.element_trivial) {
                    destroy_scratch_value(set.element_type, scratch.data());
                }
            }
            return {};
        }

        [[nodiscard]] DocumentExpected<Value> document_from_field(const FieldInfo &field, const void *field_ptr) {
            if (has_flag(field.flags, FieldFlags::Trivial)) {
                return value_from_primitive(field.primitive_kind, field_ptr, field.size);
            }
            if (field.container != nullptr) {
                return document_from_container(field, field_ptr);
            }
            if (field.map != nullptr) {
                return document_from_map(field, field_ptr);
            }
            if (field.set != nullptr) {
                return document_from_set(field, field_ptr);
            }
            if (field.optional != nullptr) {
                return document_from_optional(field, field_ptr);
            }
            return document_from_non_trivial_value(field.field_type, field_ptr);
        }

        [[nodiscard]] DocumentExpected<void> field_from_document(const FieldInfo &field, void *field_ptr, const Value &document) {
            if (has_flag(field.flags, FieldFlags::Trivial)) {
                if (!write_primitive(field.primitive_kind, field_ptr, field.size, document)) {
                    return std::unexpected(document_error(DocumentErrorCode::TypeMismatch, "field type mismatch"));
                }
                return {};
            }
            if (field.container != nullptr) {
                return container_from_document(field, field_ptr, document);
            }
            if (field.map != nullptr) {
                return map_from_document(field, field_ptr, document);
            }
            if (field.set != nullptr) {
                return set_from_document(field, field_ptr, document);
            }
            if (field.optional != nullptr) {
                return optional_from_document(field, field_ptr, document);
            }
            return non_trivial_value_from_document(field.field_type, field_ptr, document);
        }

    } // namespace

    DocumentExpected<Value> to_document(const TypeInfo &type, const void *object) {
        Value result = Value::make_object();
        for (const FieldInfo &field : type.fields) {
            const auto *field_ptr = static_cast<const unsigned char *>(object) + field.offset;
            auto field_document = document_from_field(field, field_ptr);
            if (!field_document) {
                return std::unexpected(field_document.error());
            }
            result.set(field.name.cpp_string_view(), std::move(*field_document));
        }
        return result;
    }

    DocumentExpected<void> from_document(const TypeInfo &type, void *object, const Value &document) {
        if (!document.is_object()) {
            return std::unexpected(document_error(DocumentErrorCode::TypeMismatch, "expected an object"));
        }
        for (const FieldInfo &field : type.fields) {
            const Value *field_document = document.find(field.name.cpp_string_view());
            if (field_document == nullptr) {
                // Absent field: leave whatever is already in `object` alone (see from_document's
                // doc comment) rather than treating it as an error.
                continue;
            }
            auto *field_ptr = static_cast<unsigned char *>(object) + field.offset;
            auto result = field_from_document(field, field_ptr, *field_document);
            if (!result) {
                return result;
            }
        }
        return {};
    }

} // namespace SFT::Reflection
