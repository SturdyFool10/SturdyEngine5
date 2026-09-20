#pragma once

#include <Reflection/StaticReflection.hpp>
#include <Reflection/StaticTypeId.hpp>
#include <Reflection/TypeId.hpp>
#include <Reflection/TypeInfo.hpp>

#include <Foundation/Foundation.hpp>

#include <string>
#include <string_view>

/// A structural (as opposed to nominal) fingerprint for a reflected type — the audit's "schema/ABI
/// hash" item. `TypeId` (`TypeId.hpp`) already gives every reflected type a stable *name-derived*
/// identity, but two types can share a name-derived identity's *purpose* (mod A's save data loading
/// into mod B's slightly-different struct of the same canonical name) while disagreeing on shape:
/// a field renamed, reordered, retyped, or a base class changed. `TypeId::from_name` cannot see any
/// of that — it only ever hashes the name string. `SchemaHash` closes that gap by folding the type's
/// *name*, *every field's name and type identity in declaration order*, and (recursively) its
/// *base's* schema hash into one value, so a save file, network packet, or mod manifest can compare
/// "is the shape I'm about to read still the shape this hash was produced from" without any
/// hand-maintained version counter.
namespace SFT::Reflection {


    /// A structural hash, distinct from `TypeId` on purpose: a `TypeId` answers "is this the same
    /// named type" while a `SchemaHash` answers "does this type still have the same on-the-wire
    /// shape" — two questions with different failure modes (a type can keep its name and identity
    /// while changing shape across a version, which is exactly the case this exists to catch).
    /// Deliberately not comparable against `TypeId`: they wrap the same `Foundation::Fnv1a128`
    /// machinery, but conflating "same identity" with "same shape" by accidentally comparing one
    /// against the other would silently hide the exact bug this type exists to surface.
    struct SchemaHash {
        Foundation::Fnv1a128 hash{};

        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(SchemaHash, SchemaHash) noexcept = default;
    };

    static_assert(sizeof(SchemaHash) == sizeof(u64) * 2);
    static_assert(std::is_standard_layout_v<SchemaHash>);
    static_assert(std::is_trivially_copyable_v<SchemaHash>);

    namespace Detail {

        /// Appends `value`'s bytes to `buffer` in a fixed, arbitrary (big-endian) order — the
        /// direction itself is unimportant, only that it is the same every time within a single
        /// compilation, since `SchemaHash` values are never meant to be compared across compilers/
        /// builds/architectures, only within one (the same guarantee `Fnv1a128::from_bytes` already
        /// carries). Followed by a delimiter byte, same reason as `schema_hash_append` below: so
        /// `combine(a); combine(b)` can never alias with a differently-split `combine(ab)`.
        constexpr void schema_hash_append_u64(std::string &buffer, u64 value) noexcept {
            for (int shift = 56; shift >= 0; shift -= 8) {
                buffer.push_back(static_cast<char>(static_cast<u8>(value >> shift)));
            }
        }

        /// Appends `text` to `buffer`, followed by a `'\0'` delimiter. The delimiter is load-
        /// bearing: without it, a type named `"ab"` with a field named `"c"` would hash identically
        /// to a type named `"a"` with a field named `"bc"` — naive concatenation of variable-length
        /// strings is ambiguous, a fixed separator that cannot appear inside a canonical name or
        /// field name (both are plain identifiers/dotted paths in this codebase) resolves it.
        constexpr void schema_hash_append(std::string &buffer, std::string_view text) noexcept {
            buffer.append(text);
            buffer.push_back('\0');
        }

        /// Appends a `Foundation::Fnv1a128` (a field's `TypeId::hash`, or a recursively computed
        /// base `SchemaHash::hash`) as its two constituent 64-bit halves, delimited the same way as
        /// `schema_hash_append(std::string&, std::string_view)`.
        constexpr void schema_hash_append(std::string &buffer, Foundation::Fnv1a128 value) noexcept {
            schema_hash_append_u64(buffer, value.high);
            schema_hash_append_u64(buffer, value.low);
            buffer.push_back('\0');
        }

    } // namespace Detail

    /// Computes `T`'s structural schema hash: `StaticTypeInfo<T>::name()`, then every field's name
    /// and type identity in declaration order, then — when `T` was declared via
    /// `SFT_REFLECT_TYPE_WITH_BASE` — `schema_hash_of<Base>()` folded in too, so a derived type's
    /// schema depends on its base's schema exactly as the audit calls for.
    ///
    /// `Foundation::Fnv1a128` only exposes one-shot `from_bytes(std::string_view)`, no incremental
    /// "combine another hash into an existing one" primitive, so this assembles one flat byte buffer
    /// (name, then per field, then base) and hashes it in a single call rather than inventing a
    /// bespoke mixing function on top of an algorithm that doesn't need one.
    ///
    /// Field-type coverage is exactly `StaticTypeInfo<T>::fields()`'s, which now resolves each
    /// field through `structural_type_ref` (`StructuralTypeId.hpp`) — so a `UString`,
    /// `std::vector<Item>`, `std::optional<i32>`, or `std::unordered_map<i32, Item>` field is
    /// covered, and the hash is sensitive to a container's *element* type, not merely to the fact
    /// that it is a container. What remains uncovered is a field whose type is neither reflected,
    /// nor a reflected enum, nor a known fundamental, nor a recognized container/wrapper of such
    /// (see `StructurallyIdentifiable`): that still fails to compile `fields()`, and therefore
    /// `schema_hash_of<T>()`, with the same diagnostic. This function deliberately carries no `requires`
    /// clause attempting to detect that case ahead of time: `requires { StaticTypeInfo<T>::fields();
    /// }` looks like it should turn that hard error into a clean "constraint not satisfied", but it
    /// does not — a simple-requirement only checks that the *call expression* `fields()` type-checks
    /// against `fields()`'s own declared signature (which does not depend on any field's type, only
    /// on `field_count()`), not that the function's *body* would actually instantiate successfully.
    /// Verified empirically against this exact compiler: a `concept` built from that requires-
    /// expression evaluates to `true` for a type with an unstable-typed field, then the type still
    /// hard-errors the moment `fields()`/`schema_hash_of()` is actually instantiated — the guard
    /// would have been actively misleading, not merely redundant, so it is omitted and the natural
    /// compile error is left to propagate, matching how `StaticTypeInfo<T>::fields()` itself already
    /// behaves for such types.
    ///
    /// @return Returns the newly computed structural hash.
    template <class T>
    [[nodiscard]] consteval SchemaHash schema_hash_of() noexcept {
        std::string buffer;
        Detail::schema_hash_append(buffer, StaticTypeInfo<T>::name());

        for (const StaticFieldInfo &field : StaticTypeInfo<T>::fields()) {
            Detail::schema_hash_append(buffer, field.name);
            Detail::schema_hash_append(buffer, field.type().hash);
        }

        // Methods are folded in by name and full signature (`StaticMethodInfo::key()`, the same
        // identity `Detail::compute_method_key` computes at runtime registration), not merely by
        // name: a method whose parameter list changed shape (an overload added/removed/retyped)
        // is exactly the kind of shape change a save/mod-compatibility hash exists to catch, the
        // same way a retyped field already is above. Return type is deliberately not folded in
        // separately — two methods differing only in return type but sharing name+parameters
        // would be an ill-formed overload set in C++ anyway, so `key()` already disambiguates
        // everything that can legally coexist under one name.
        for (const StaticMethodInfo &method : StaticTypeInfo<T>::methods()) {
            Detail::schema_hash_append(buffer, method.name);
            Detail::schema_hash_append(buffer, method.key().hash);
        }

        if constexpr (requires { typename TypeTraits<T>::BaseType; }) {
            Detail::schema_hash_append(buffer, schema_hash_of<typename TypeTraits<T>::BaseType>().hash);
        }

        return SchemaHash{.hash = Foundation::Fnv1a128::from_bytes(buffer)};
    }


} // namespace SFT::Reflection
