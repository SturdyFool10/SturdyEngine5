#pragma once

#include <Reflection/TypeInfo.hpp>
#include <Reflection/TypeRegistry.hpp>

#include <Foundation/Foundation.hpp>

#include <cstring>
#include <expected>
#include <span>
#include <vector>

namespace SFT::Reflection {

    /// A minimal binary serializer built directly on the reflection data model: walk
    /// `TypeInfo::fields` in declaration order, write each one, recursing into fields whose type
    /// is itself a registered `TypeInfo` (made possible by `erased_type_id`/`FieldInfo::field_type`
    /// sharing identity with that type's own `TypeInfo::key`).
    ///
    /// Round-trips trivially-copyable fields, `UString` fields (length-prefixed), fields whose
    /// declared type is itself a reflected struct (recursively), and `std::vector<T>` fields for
    /// *any* `T` this walk can otherwise handle — `Trivial` elements go out as one bulk `memcpy`
    /// of the container's contiguous storage; `UString`/nested-reflected-struct elements are
    /// walked one at a time, straight out of that same contiguous storage (a `std::vector` never
    /// holds anything but fully-constructed elements, so no scratch copy is needed either way —
    /// see `ContainerInfo`). A field that is none of those (an opaque non-trivial type with no
    /// nested `TypeInfo` and no container recognition, e.g. an associative container) is a clean,
    /// reported error rather than silently dropped or a crash — associative containers
    /// (`std::unordered_map`/`std::map`) and `std::optional` are known, separate pieces of future
    /// work, not yet recognized by `FieldInfo`/`ContainerInfo` at all.
    enum class SerializeErrorCode : u32 {
        /// A field's type is not `Trivial`, not `UString`, and not itself a registered `TypeInfo`
        /// — there is no generic way to walk it.
        UnsupportedFieldType,
        /// A required nested type (a non-trivial field's `field_type`) is not registered in
        /// `TypeRegistry`.
        NestedTypeNotRegistered,
        /// The byte buffer ran out before every field was read, or a length prefix claimed more
        /// bytes than remained.
        UnexpectedEndOfData,
        /// `TypeInfo::default_construct` was needed (to receive a deserialized value) and is null.
        NotDefaultConstructible,
    };

    struct SerializeError {
        SerializeErrorCode code = SerializeErrorCode::UnsupportedFieldType;
        UString message;
    };

    template <class Value>
    using SerializeExpected = std::expected<Value, SerializeError>;

    /// Appends `object`'s fields (an instance of `type`) to `out`, in `type.fields` declaration
    /// order, recursing into any field whose type is itself registered.
    ///
    /// @note Not a hot-path operation — same rationale as `Engine::read_component_field`: this
    /// trades allocation and per-field dispatch for generality, deliberately, in exchange for
    /// `SFT_REFLECT_INVOKE`-style call sites elsewhere staying free.
    [[nodiscard]] SerializeExpected<void> serialize_to_bytes(const TypeInfo &type, const void *object, std::vector<std::byte> &out);

    /// Reads `type.fields` back out of `bytes` (in the same order `serialize_to_bytes` wrote
    /// them) into `object`, which must already be a live, default-constructed instance of `type`
    /// (deserialization assigns into existing fields; it does not placement-construct `object`
    /// itself — see `default_construct_instance` if you need a fresh one first).
    ///
    /// @param bytes_consumed Set to how many bytes of `bytes` were read, so a caller
    /// deserializing a sequence of values back-to-back knows where the next one starts.
    [[nodiscard]] SerializeExpected<void> deserialize_from_bytes(const TypeInfo &type, void *object, std::span<const std::byte> bytes, usize &bytes_consumed);

} // namespace SFT::Reflection
