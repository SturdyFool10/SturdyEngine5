#pragma once

#include <Reflection/Attribute.hpp>
#include <Reflection/ContainerInfo.hpp>
#include <Reflection/MapInfo.hpp>
#include <Reflection/OptionalInfo.hpp>
#include <Reflection/PrimitiveKind.hpp>
#include <Reflection/SetInfo.hpp>
#include <Reflection/TypeId.hpp>

#include <Foundation/Foundation.hpp>

#include <cstring>
#include <vector>

namespace SFT::Reflection {


    enum class FieldFlags : u32 {
        None = 0,
        /// Set when the field's declared type is trivially copyable, so `TypeRegistry`'s
        /// get/set path can take the raw offset+memcpy fast path instead of calling
        /// through `copy_get`/`copy_set`.
        Trivial = 1u << 0u,
        /// Rejects `set_field`/`copy_field_in`, e.g. for fields a mod may read but never write.
        ReadOnly = 1u << 1u,
        /// Set on a static data member (`SFT_REFLECT_STATIC_FIELD`). Static fields have no
        /// per-instance offset — `FieldInfo::static_address` holds the field's one true address
        /// instead, and `FieldInfo::offset` is always `0` for them, which is exactly what lets
        /// `copy_field_out`/`copy_field_in`'s existing offset math work unchanged when called as
        /// `copy_static_field_out`/`copy_static_field_in` (passing `static_address` as `object`).
        Static = 1u << 2u,
    };

    /// Combines the operands with bitwise OR.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr FieldFlags operator|(FieldFlags lhs, FieldFlags rhs) noexcept {
        return static_cast<FieldFlags>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
    }

    /// Reports whether flag is available.
    ///
    /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr bool has_flag(FieldFlags value, FieldFlags flag) noexcept {
        return (static_cast<u32>(value) & static_cast<u32>(flag)) != 0;
    }

    /// Placement-copies a field's current value out of `object` into `out_value`, used only for
    /// non-`Trivial` fields (e.g. `UString`, `std::vector<T>`) that cannot be safely `memcpy`'d.
    using FieldCopyGetFn = void (*)(const void *object, void *out_value) noexcept;
    /// Placement-assigns `in_value` into a field on `object`, used only for non-`Trivial` fields.
    using FieldCopySetFn = void (*)(void *object, const void *in_value) noexcept;


    struct FieldInfo {
        TypeId key{};
        UString name;
        TypeId field_type{};
        usize offset = 0;
        usize size = 0;
        usize align = 0;
        FieldFlags flags = FieldFlags::None;
        FieldCopyGetFn copy_get = nullptr;
        FieldCopySetFn copy_set = nullptr;
        /// Arbitrary tooling/mod-facing metadata (min/max, display name, ...). Never consulted by
        /// `copy_field_out`/`copy_field_in`; see `find_attribute`.
        std::vector<Attribute> attributes;
        /// Only meaningful when `has_flag(flags, FieldFlags::Static)`: the field's one true
        /// address (`&Type::static_member`), unused/null for instance fields.
        void *static_address = nullptr;
        /// Non-null when this field's type is a recognized container (`std::vector<T>` in v1),
        /// enabling element-by-element access/serialization instead of treating it as an opaque
        /// blob. Always null for `Trivial` fields (a trivially-copyable field is never a
        /// container `Detail::build_field_info` recognizes — containers own heap storage).
        const ContainerInfo *container = nullptr;
        /// Non-null when this field's type is a recognized associative container
        /// (`std::unordered_map<K, V>` in v1), enabling key/value access/serialization instead of
        /// treating it as an opaque blob. Mutually exclusive with `container` — a field is a
        /// sequence or a map, never both.
        const MapInfo *map = nullptr;
        /// Non-null when this field's type is a recognized set (`std::set<T>`/
        /// `std::unordered_set<T>`), enabling element access/serialization instead of treating it
        /// as an opaque blob. Mutually exclusive with `container`/`map`.
        const SetInfo *set = nullptr;
        /// Non-null when this field's type is `std::optional<T>`, `std::unique_ptr<T>`, or
        /// `std::shared_ptr<T>` — every "nullable single value" shape shares this one accessor
        /// interface (has-value/get/emplace/reset), since reflection only cares about that shape,
        /// never about ownership semantics. Mutually exclusive with `container`/`map`/`set`.
        const OptionalInfo *optional = nullptr;
        /// Only meaningful when `has_flag(flags, FieldFlags::Trivial)`: which scalar shape the
        /// field's bytes represent (see `PrimitiveKind`'s doc comment for why this exists —
        /// `Document.hpp`'s JSON-ish `to_document`/`from_document` are its only consumers; the
        /// binary serializer never needs it).
        PrimitiveKind primitive_kind = PrimitiveKind::None;
    };

    /// Reads `field`'s value out of `object` into `out_data`.
    ///
    /// Trivial fields take the raw offset+memcpy fast path with no indirect call; non-trivial
    /// fields fall through to the type-generated `copy_get` trampoline.
    ///
    /// @return Returns `true` on success; `false` when `size` does not match the field's size or
    /// the field has no readable representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool copy_field_out(const FieldInfo &field, const void *object, void *out_data, usize size) noexcept {
        if (size != field.size) {
            return false;
        }
        if (has_flag(field.flags, FieldFlags::Trivial)) {
            const auto *base = static_cast<const unsigned char *>(object) + field.offset;
            std::memcpy(out_data, base, size);
            return true;
        }
        if (field.copy_get != nullptr) {
            field.copy_get(object, out_data);
            return true;
        }
        return false;
    }

    /// Writes `in_data` into `field` on `object`.
    ///
    /// @return Returns `true` on success; `false` when `size` mismatches, the field is
    /// `ReadOnly`, or the field has no writable representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool copy_field_in(const FieldInfo &field, void *object, const void *in_data, usize size) noexcept {
        if (size != field.size || has_flag(field.flags, FieldFlags::ReadOnly)) {
            return false;
        }
        if (has_flag(field.flags, FieldFlags::Trivial)) {
            auto *base = static_cast<unsigned char *>(object) + field.offset;
            std::memcpy(base, in_data, size);
            return true;
        }
        if (field.copy_set != nullptr) {
            field.copy_set(object, in_data);
            return true;
        }
        return false;
    }

    /// Same as `copy_field_out`, additionally rejecting the read when `expected_type` does not
    /// match `field.field_type`.
    ///
    /// `copy_field_out` only checks that the byte size matches — two unrelated field types can
    /// share a size (a `float` and an `int`, an `enum class : u8` and a `bool`), so a caller that
    /// doesn't already know the field's exact type at compile time (an FFI boundary, an untrusted
    /// mod) should use this instead: it costs one extra `TypeId` comparison, paid only by callers
    /// that ask for it — trusted first-party call sites that already know the type keep using the
    /// unchecked version at no extra cost.
    ///
    /// @return Returns `true` on success; `false` on a type or size mismatch, or when the field
    /// has no readable representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool copy_field_out_checked(const FieldInfo &field, TypeId expected_type, const void *object, void *out_data, usize size) noexcept {
        if (field.field_type != expected_type) {
            return false;
        }
        return copy_field_out(field, object, out_data, size);
    }

    /// Same as `copy_field_in`, additionally rejecting the write when `expected_type` does not
    /// match `field.field_type`. See `copy_field_out_checked` for why this exists.
    ///
    /// @return Returns `true` on success; `false` on a type or size mismatch, the field being
    /// `ReadOnly`, or the field having no writable representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool copy_field_in_checked(const FieldInfo &field, TypeId expected_type, void *object, const void *in_data, usize size) noexcept {
        if (field.field_type != expected_type) {
            return false;
        }
        return copy_field_in(field, object, in_data, size);
    }

    /// Reads a static field's value (one declared via `SFT_REFLECT_STATIC_FIELD`) into `out_data`.
    /// There is no per-instance object to read from — `field.static_address` supplies it, and
    /// since a static field's `offset` is always `0`, this is exactly `copy_field_out` with that
    /// address standing in for `object`.
    ///
    /// @return Returns `true` on success; `false` when `size` does not match the field's size or
    /// the field has no readable representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool copy_static_field_out(const FieldInfo &field, void *out_data, usize size) noexcept {
        return copy_field_out(field, field.static_address, out_data, size);
    }

    /// Writes `in_data` into a static field. See `copy_static_field_out`.
    ///
    /// @return Returns `true` on success; `false` when `size` mismatches, the field is
    /// `ReadOnly`, or the field has no writable representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool copy_static_field_in(const FieldInfo &field, const void *in_data, usize size) noexcept {
        return copy_field_in(field, field.static_address, in_data, size);
    }

    /// Type-checked variant of `copy_static_field_out`. See `copy_field_out_checked`.
    [[nodiscard]] inline bool copy_static_field_out_checked(const FieldInfo &field, TypeId expected_type, void *out_data, usize size) noexcept {
        return copy_field_out_checked(field, expected_type, field.static_address, out_data, size);
    }

    /// Type-checked variant of `copy_static_field_in`. See `copy_field_in_checked`.
    [[nodiscard]] inline bool copy_static_field_in_checked(const FieldInfo &field, TypeId expected_type, const void *in_data, usize size) noexcept {
        return copy_field_in_checked(field, expected_type, field.static_address, in_data, size);
    }


} // namespace SFT::Reflection
