#include <Reflection/Serialize.hpp>

namespace SFT::Reflection {

    namespace {

        [[nodiscard]] SerializeError serialize_error(SerializeErrorCode code, std::string_view message) {
            return SerializeError{.code = code, .message = UString{message}};
        }

        [[nodiscard]] TypeId ustring_type_id() {
            static const TypeId id = type_id_for<UString>();
            return id;
        }

        void append_bytes(std::vector<std::byte> &out, const void *data, usize size) {
            const auto *bytes = static_cast<const std::byte *>(data);
            out.insert(out.end(), bytes, bytes + size);
        }

        [[nodiscard]] SerializeExpected<std::span<const std::byte>> take_bytes(std::span<const std::byte> &cursor, usize count) {
            if (cursor.size() < count) {
                return std::unexpected(serialize_error(SerializeErrorCode::UnexpectedEndOfData,
                                                        "ran out of bytes while reading a value"));
            }
            std::span<const std::byte> taken = cursor.first(count);
            cursor = cursor.subspan(count);
            return taken;
        }

        /// Serializes one `UString` or nested-reflected-struct value — the shared "not Trivial"
        /// logic, used both for a field of that shape directly and for each element of a
        /// container whose element type is that shape. `value_ptr` must already be a live,
        /// constructed value of `value_type` (true for a struct's own non-trivial fields as part
        /// of the struct itself, and equally true for a container's elements: `std::vector`
        /// always holds fully-constructed elements, never raw storage).
        [[nodiscard]] SerializeExpected<void> serialize_non_trivial_value(TypeId value_type, const void *value_ptr, std::vector<std::byte> &out) {
            if (value_type == ustring_type_id()) {
                const auto *value = static_cast<const UString *>(value_ptr);
                const u64 length = value->byte_size();
                append_bytes(out, &length, sizeof(length));
                append_bytes(out, value->data(), length);
                return {};
            }
            const TypeInfo *nested = TypeRegistry::instance().find(value_type);
            if (nested == nullptr) {
                return std::unexpected(serialize_error(
                    SerializeErrorCode::NestedTypeNotRegistered,
                    "value's declared type is not Trivial, not UString, and is not a registered TypeInfo"));
            }
            return serialize_to_bytes(*nested, value_ptr, out);
        }

        /// Deserializes one `UString` or nested-reflected-struct value into `value_ptr` (assigned
        /// into, like `serialize_non_trivial_value` — `value_ptr` must already be live). See
        /// `serialize_non_trivial_value`.
        [[nodiscard]] SerializeExpected<void> deserialize_non_trivial_value(TypeId value_type, void *value_ptr, std::span<const std::byte> &cursor) {
            if (value_type == ustring_type_id()) {
                auto length_bytes = take_bytes(cursor, sizeof(u64));
                if (!length_bytes) {
                    return std::unexpected(length_bytes.error());
                }
                u64 length = 0;
                std::memcpy(&length, length_bytes->data(), sizeof(length));
                auto string_bytes = take_bytes(cursor, static_cast<usize>(length));
                if (!string_bytes) {
                    return std::unexpected(string_bytes.error());
                }
                const auto *chars = reinterpret_cast<const char *>(string_bytes->data());
                *static_cast<UString *>(value_ptr) = UString{std::string_view(chars, static_cast<usize>(length))};
                return {};
            }
            const TypeInfo *nested = TypeRegistry::instance().find(value_type);
            if (nested == nullptr) {
                return std::unexpected(serialize_error(
                    SerializeErrorCode::NestedTypeNotRegistered,
                    "value's declared type is not Trivial, not UString, and is not a registered TypeInfo"));
            }
            usize nested_bytes_consumed = 0;
            auto nested_result = deserialize_from_bytes(*nested, value_ptr, cursor, nested_bytes_consumed);
            if (!nested_result) {
                return std::unexpected(nested_result.error());
            }
            cursor = cursor.subspan(nested_bytes_consumed);
            return {};
        }

        /// Serializes a container field: element count, then each element in order. Trivial
        /// elements go out as one bulk `memcpy` of the contiguous storage; non-trivial elements
        /// (`UString`, a nested reflected struct) are walked one at a time through
        /// `serialize_non_trivial_value`, reading straight out of the container's real
        /// contiguous storage — `std::vector` never holds anything but fully-constructed
        /// elements, so no scratch/temporary construction is needed either way.
        [[nodiscard]] SerializeExpected<void> serialize_container(const FieldInfo &field, const void *field_ptr, std::vector<std::byte> &out) {
            const ContainerInfo &container = *field.container;
            if (container.data == nullptr) {
                return std::unexpected(serialize_error(SerializeErrorCode::UnsupportedFieldType,
                                                        "container field has no readable storage accessor"));
            }
            const u64 count = container_size(field.container, field_ptr);
            append_bytes(out, &count, sizeof(count));
            const auto *base = static_cast<const unsigned char *>(container.data(field_ptr));
            if (container.element_trivial) {
                append_bytes(out, base, static_cast<usize>(count) * container.element_size);
                return {};
            }
            for (u64 i = 0; i < count; ++i) {
                auto result = serialize_non_trivial_value(container.element_type, base + i * container.element_size, out);
                if (!result) {
                    return result;
                }
            }
            return {};
        }

        /// Deserializes a container field: reads the element count, resizes to match (which
        /// default-constructs any new non-trivial elements — required before
        /// `deserialize_non_trivial_value` can assign into them), then fills every element
        /// either as one bulk `memcpy` (trivial) or one at a time (non-trivial), writing directly
        /// into the container's real contiguous storage.
        [[nodiscard]] SerializeExpected<void> deserialize_container(const FieldInfo &field, void *field_ptr, std::span<const std::byte> &cursor) {
            const ContainerInfo &container = *field.container;
            if (container.mutable_data == nullptr) {
                return std::unexpected(serialize_error(SerializeErrorCode::UnsupportedFieldType,
                                                        "container field has no writable storage accessor"));
            }
            auto count_bytes = take_bytes(cursor, sizeof(u64));
            if (!count_bytes) {
                return std::unexpected(count_bytes.error());
            }
            u64 count = 0;
            std::memcpy(&count, count_bytes->data(), sizeof(count));

            if (!container_resize(field.container, field_ptr, static_cast<usize>(count))) {
                return std::unexpected(serialize_error(
                    SerializeErrorCode::NotDefaultConstructible,
                    "container field's element type has no default constructor, so it cannot be resized to receive deserialized elements"));
            }

            if (container.element_trivial) {
                const usize byte_count = static_cast<usize>(count) * container.element_size;
                auto element_bytes = take_bytes(cursor, byte_count);
                if (!element_bytes) {
                    return std::unexpected(element_bytes.error());
                }
                std::memcpy(container.mutable_data(field_ptr), element_bytes->data(), byte_count);
                return {};
            }

            auto *base = static_cast<unsigned char *>(container.mutable_data(field_ptr));
            for (u64 i = 0; i < count; ++i) {
                auto result = deserialize_non_trivial_value(container.element_type, base + i * container.element_size, cursor);
                if (!result) {
                    return result;
                }
            }
            return {};
        }

        /// Default-constructs a `UString`/nested-reflected-struct scratch value into `destination`
        /// — needed only for a map's key/value (a map creates its entries via `insert_or_assign`,
        /// unlike a `std::vector`, whose elements already live at a real, constructed address
        /// after `resize`, so `deserialize_non_trivial_value` can assign straight into it there).
        [[nodiscard]] bool default_construct_scratch_value(TypeId value_type, void *destination) {
            if (value_type == ustring_type_id()) {
                ::new (destination) UString();
                return true;
            }
            const TypeInfo *nested = TypeRegistry::instance().find(value_type);
            return nested != nullptr && default_construct_instance(*nested, destination);
        }

        /// Destroys a scratch value built by `default_construct_scratch_value`.
        void destroy_scratch_value(TypeId value_type, void *object) noexcept {
            if (value_type == ustring_type_id()) {
                static_cast<UString *>(object)->~UString();
                return;
            }
            if (const TypeInfo *nested = TypeRegistry::instance().find(value_type); nested != nullptr) {
                destroy_instance(*nested, object);
            }
        }

        /// Shared by `serialize_map`'s `MapInfo::for_each` visitor callback: state the
        /// capture-less callback needs, plus how a mid-walk failure is reported (the visitor
        /// itself cannot return a `SerializeExpected` — `MapInfo::VisitFn` has no return value —
        /// so a failure is recorded here and stops writing further entries instead).
        struct MapSerializeContext {
            std::vector<std::byte> *out;
            const MapInfo *map;
            SerializeError error{};
            bool failed = false;
        };

        /// Serializes a map field: entry count, then each key/value pair in whatever order
        /// `MapInfo::for_each` visits them (a `std::unordered_map` has no meaningful order to
        /// preserve). Trivial keys/values go out as raw bytes; non-trivial ones go through
        /// `serialize_non_trivial_value`, reading straight out of the entry `for_each` hands the
        /// visitor — a live map entry is always fully constructed, so (like a vector's elements)
        /// no scratch copy is needed here either.
        [[nodiscard]] SerializeExpected<void> serialize_map(const FieldInfo &field, const void *field_ptr, std::vector<std::byte> &out) {
            const MapInfo &map = *field.map;
            if (map.for_each == nullptr) {
                return std::unexpected(serialize_error(SerializeErrorCode::UnsupportedFieldType, "map field has no readable representation"));
            }
            const u64 count = static_cast<u64>(map_size(field.map, field_ptr));
            append_bytes(out, &count, sizeof(count));

            MapSerializeContext context{.out = &out, .map = &map};
            map.for_each(
                field_ptr,
                [](const void *key, const void *value, void *user_data) noexcept {
                    auto *ctx = static_cast<MapSerializeContext *>(user_data);
                    if (ctx->failed) {
                        return;
                    }
                    if (ctx->map->key_trivial) {
                        append_bytes(*ctx->out, key, ctx->map->key_size);
                    } else if (auto result = serialize_non_trivial_value(ctx->map->key_type, key, *ctx->out); !result) {
                        ctx->failed = true;
                        ctx->error = result.error();
                        return;
                    }
                    if (ctx->map->value_trivial) {
                        append_bytes(*ctx->out, value, ctx->map->value_size);
                    } else if (auto result = serialize_non_trivial_value(ctx->map->value_type, value, *ctx->out); !result) {
                        ctx->failed = true;
                        ctx->error = result.error();
                    }
                },
                &context);

            if (context.failed) {
                return std::unexpected(context.error);
            }
            return {};
        }

        /// Deserializes a map field: entry count, then reads and inserts that many key/value
        /// pairs. Unlike a vector, a map has no pre-existing storage to deserialize a non-trivial
        /// entry directly into (an entry only starts existing once `insert_or_assign` creates
        /// it) — so a non-trivial key/value is built in scratch storage first
        /// (`default_construct_scratch_value` + `deserialize_non_trivial_value`), inserted, then
        /// destroyed, with every error path unwinding whatever scratch storage it already
        /// constructed rather than leaking it.
        [[nodiscard]] SerializeExpected<void> deserialize_map(const FieldInfo &field, void *field_ptr, std::span<const std::byte> &cursor) {
            const MapInfo &map = *field.map;
            if (map.clear == nullptr || map.insert_or_assign == nullptr) {
                return std::unexpected(serialize_error(SerializeErrorCode::UnsupportedFieldType, "map field has no writable representation"));
            }
            auto count_bytes = take_bytes(cursor, sizeof(u64));
            if (!count_bytes) {
                return std::unexpected(count_bytes.error());
            }
            u64 count = 0;
            std::memcpy(&count, count_bytes->data(), sizeof(count));

            map.clear(field_ptr);

            std::vector<std::byte> key_storage(map.key_size);
            std::vector<std::byte> value_storage(map.value_size);

            for (u64 i = 0; i < count; ++i) {
                if (map.key_trivial) {
                    auto key_bytes = take_bytes(cursor, map.key_size);
                    if (!key_bytes) {
                        return std::unexpected(key_bytes.error());
                    }
                    std::memcpy(key_storage.data(), key_bytes->data(), map.key_size);
                } else {
                    if (!default_construct_scratch_value(map.key_type, key_storage.data())) {
                        return std::unexpected(serialize_error(SerializeErrorCode::NotDefaultConstructible,
                                                                "map field's key type has no default constructor"));
                    }
                    auto result = deserialize_non_trivial_value(map.key_type, key_storage.data(), cursor);
                    if (!result) {
                        destroy_scratch_value(map.key_type, key_storage.data());
                        return std::unexpected(result.error());
                    }
                }

                if (map.value_trivial) {
                    auto value_bytes = take_bytes(cursor, map.value_size);
                    if (!value_bytes) {
                        if (!map.key_trivial) {
                            destroy_scratch_value(map.key_type, key_storage.data());
                        }
                        return std::unexpected(value_bytes.error());
                    }
                    std::memcpy(value_storage.data(), value_bytes->data(), map.value_size);
                } else {
                    if (!default_construct_scratch_value(map.value_type, value_storage.data())) {
                        if (!map.key_trivial) {
                            destroy_scratch_value(map.key_type, key_storage.data());
                        }
                        return std::unexpected(serialize_error(SerializeErrorCode::NotDefaultConstructible,
                                                                "map field's value type has no default constructor"));
                    }
                    auto result = deserialize_non_trivial_value(map.value_type, value_storage.data(), cursor);
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
            }
            return {};
        }

        /// Serializes an optional field: one presence byte, then — only when present — the
        /// contained value, straight out of `OptionalInfo::data` (a present optional's value is
        /// always fully constructed, so like a vector's elements or a live map entry, no scratch
        /// copy is needed here either).
        [[nodiscard]] SerializeExpected<void> serialize_optional(const FieldInfo &field, const void *field_ptr, std::vector<std::byte> &out) {
            const OptionalInfo &optional = *field.optional;
            if (optional.has_value == nullptr || optional.data == nullptr) {
                return std::unexpected(serialize_error(SerializeErrorCode::UnsupportedFieldType, "optional field has no readable representation"));
            }
            const bool present = optional.has_value(field_ptr);
            const u8 flag = present ? 1 : 0;
            append_bytes(out, &flag, sizeof(flag));
            if (!present) {
                return {};
            }
            const void *value_ptr = optional.data(field_ptr);
            if (optional.value_trivial) {
                append_bytes(out, value_ptr, optional.value_size);
                return {};
            }
            return serialize_non_trivial_value(optional.value_type, value_ptr, out);
        }

        /// Deserializes an optional field. Unlike a vector, an empty optional has no storage to
        /// deserialize a non-trivial value directly into (the value only starts existing once
        /// `emplace_copy` creates it) — so, like a map's entries, a non-trivial value is built in
        /// scratch storage first, emplaced, then destroyed.
        [[nodiscard]] SerializeExpected<void> deserialize_optional(const FieldInfo &field, void *field_ptr, std::span<const std::byte> &cursor) {
            const OptionalInfo &optional = *field.optional;
            if (optional.reset == nullptr || optional.emplace_copy == nullptr) {
                return std::unexpected(serialize_error(SerializeErrorCode::UnsupportedFieldType, "optional field has no writable representation"));
            }
            auto flag_bytes = take_bytes(cursor, sizeof(u8));
            if (!flag_bytes) {
                return std::unexpected(flag_bytes.error());
            }
            u8 flag = 0;
            std::memcpy(&flag, flag_bytes->data(), sizeof(flag));
            if (flag == 0) {
                optional.reset(field_ptr);
                return {};
            }

            if (optional.value_trivial) {
                auto value_bytes = take_bytes(cursor, optional.value_size);
                if (!value_bytes) {
                    return std::unexpected(value_bytes.error());
                }
                optional.emplace_copy(field_ptr, value_bytes->data());
                return {};
            }

            std::vector<std::byte> scratch(optional.value_size);
            if (!default_construct_scratch_value(optional.value_type, scratch.data())) {
                return std::unexpected(serialize_error(SerializeErrorCode::NotDefaultConstructible,
                                                        "optional field's value type has no default constructor"));
            }
            auto result = deserialize_non_trivial_value(optional.value_type, scratch.data(), cursor);
            if (!result) {
                destroy_scratch_value(optional.value_type, scratch.data());
                return std::unexpected(result.error());
            }
            optional.emplace_copy(field_ptr, scratch.data());
            destroy_scratch_value(optional.value_type, scratch.data());
            return {};
        }

        /// Shared by `serialize_set`'s `SetInfo::for_each` visitor callback. Mirrors
        /// `MapSerializeContext`.
        struct SetSerializeContext {
            std::vector<std::byte> *out;
            const SetInfo *set;
            SerializeError error{};
            bool failed = false;
        };

        /// Serializes a set field: element count, then each element in whatever order
        /// `SetInfo::for_each` visits them. Mirrors `serialize_map` (an element is always fully
        /// constructed, so no scratch copy is needed here either).
        [[nodiscard]] SerializeExpected<void> serialize_set(const FieldInfo &field, const void *field_ptr, std::vector<std::byte> &out) {
            const SetInfo &set = *field.set;
            if (set.for_each == nullptr) {
                return std::unexpected(serialize_error(SerializeErrorCode::UnsupportedFieldType, "set field has no readable representation"));
            }
            const u64 count = static_cast<u64>(set_size(field.set, field_ptr));
            append_bytes(out, &count, sizeof(count));

            SetSerializeContext context{.out = &out, .set = &set};
            set.for_each(
                field_ptr,
                [](const void *element, void *user_data) noexcept {
                    auto *ctx = static_cast<SetSerializeContext *>(user_data);
                    if (ctx->failed) {
                        return;
                    }
                    if (ctx->set->element_trivial) {
                        append_bytes(*ctx->out, element, ctx->set->element_size);
                    } else if (auto result = serialize_non_trivial_value(ctx->set->element_type, element, *ctx->out); !result) {
                        ctx->failed = true;
                        ctx->error = result.error();
                    }
                },
                &context);

            if (context.failed) {
                return std::unexpected(context.error);
            }
            return {};
        }

        /// Deserializes a set field: entry count, then reads and inserts that many elements.
        /// Mirrors `deserialize_map`'s scratch-storage discipline for non-trivial elements (a set
        /// only starts owning an element once `insert` copies it in).
        [[nodiscard]] SerializeExpected<void> deserialize_set(const FieldInfo &field, void *field_ptr, std::span<const std::byte> &cursor) {
            const SetInfo &set = *field.set;
            if (set.clear == nullptr || set.insert == nullptr) {
                return std::unexpected(serialize_error(SerializeErrorCode::UnsupportedFieldType, "set field has no writable representation"));
            }
            auto count_bytes = take_bytes(cursor, sizeof(u64));
            if (!count_bytes) {
                return std::unexpected(count_bytes.error());
            }
            u64 count = 0;
            std::memcpy(&count, count_bytes->data(), sizeof(count));

            set.clear(field_ptr);

            std::vector<std::byte> element_storage(set.element_size);
            for (u64 i = 0; i < count; ++i) {
                if (set.element_trivial) {
                    auto element_bytes = take_bytes(cursor, set.element_size);
                    if (!element_bytes) {
                        return std::unexpected(element_bytes.error());
                    }
                    std::memcpy(element_storage.data(), element_bytes->data(), set.element_size);
                } else {
                    if (!default_construct_scratch_value(set.element_type, element_storage.data())) {
                        return std::unexpected(serialize_error(SerializeErrorCode::NotDefaultConstructible,
                                                                "set field's element type has no default constructor"));
                    }
                    auto result = deserialize_non_trivial_value(set.element_type, element_storage.data(), cursor);
                    if (!result) {
                        destroy_scratch_value(set.element_type, element_storage.data());
                        return std::unexpected(result.error());
                    }
                }

                set.insert(field_ptr, element_storage.data());
                if (!set.element_trivial) {
                    destroy_scratch_value(set.element_type, element_storage.data());
                }
            }
            return {};
        }

        [[nodiscard]] SerializeExpected<void> serialize_field(const FieldInfo &field, const void *field_ptr, std::vector<std::byte> &out) {
            if (has_flag(field.flags, FieldFlags::Trivial)) {
                append_bytes(out, field_ptr, field.size);
                return {};
            }
            if (field.container != nullptr) {
                return serialize_container(field, field_ptr, out);
            }
            if (field.map != nullptr) {
                return serialize_map(field, field_ptr, out);
            }
            if (field.set != nullptr) {
                return serialize_set(field, field_ptr, out);
            }
            if (field.optional != nullptr) {
                return serialize_optional(field, field_ptr, out);
            }
            return serialize_non_trivial_value(field.field_type, field_ptr, out);
        }

        [[nodiscard]] SerializeExpected<void> deserialize_field(const FieldInfo &field, void *field_ptr, std::span<const std::byte> &cursor) {
            if (has_flag(field.flags, FieldFlags::Trivial)) {
                auto taken = take_bytes(cursor, field.size);
                if (!taken) {
                    return std::unexpected(taken.error());
                }
                std::memcpy(field_ptr, taken->data(), field.size);
                return {};
            }
            if (field.container != nullptr) {
                return deserialize_container(field, field_ptr, cursor);
            }
            if (field.map != nullptr) {
                return deserialize_map(field, field_ptr, cursor);
            }
            if (field.set != nullptr) {
                return deserialize_set(field, field_ptr, cursor);
            }
            if (field.optional != nullptr) {
                return deserialize_optional(field, field_ptr, cursor);
            }
            return deserialize_non_trivial_value(field.field_type, field_ptr, cursor);
        }

    } // namespace

    SerializeExpected<void> serialize_to_bytes(const TypeInfo &type, const void *object, std::vector<std::byte> &out) {
        for (const FieldInfo &field : type.fields) {
            const auto *field_ptr = static_cast<const unsigned char *>(object) + field.offset;
            auto result = serialize_field(field, field_ptr, out);
            if (!result) {
                return result;
            }
        }
        return {};
    }

    SerializeExpected<void> deserialize_from_bytes(const TypeInfo &type, void *object, std::span<const std::byte> bytes, usize &bytes_consumed) {
        std::span<const std::byte> cursor = bytes;
        for (const FieldInfo &field : type.fields) {
            auto *field_ptr = static_cast<unsigned char *>(object) + field.offset;
            auto result = deserialize_field(field, field_ptr, cursor);
            if (!result) {
                return result;
            }
        }
        bytes_consumed = bytes.size() - cursor.size();
        return {};
    }

} // namespace SFT::Reflection
