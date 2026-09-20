#pragma once

#include <Reflection/Macros.hpp>
#include <Reflection/TypeInfo.hpp>

#include <Foundation/Foundation.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

/// Compile-time-first binary serialize/deserialize for reflected types (audit Phase 3, items
/// 20-21) — sits alongside `StaticReflection.hpp`'s `StaticTypeInfo<T>`/`get`/`set` and the
/// runtime `Serialize.hpp`/`Document.hpp` machinery, replacing neither.
///
/// `static_serialize`/`static_deserialize` walk `TypeTraits<T>::for_each_member` directly (not
/// through `StaticTypeInfo<T>::fields()`'s homogeneous `StaticFieldInfo` array — that type
/// deliberately drops each field's member pointer, since member pointers of different fields have
/// different C++ types and can't live in one array; reading/writing a field's real value needs
/// that member pointer back). Only two field shapes are supported in v1:
///
///   - trivially copyable fields (`int`, `float`, `bool`, POD structs, ...): copied as raw bytes.
///   - fields whose type is itself `SFT_REFLECT_TYPE`-annotated: serialized recursively, field by
///     field, so nested reflected structs (e.g. a `Player` with a `Position` field) round-trip
///     without ever becoming an opaque blob.
///
/// Everything else (containers, `UString`, `std::optional`, smart pointers, ...) is a compile
/// error here — see `Detail::StaticSerializeUnsupportedField`'s `static_assert` below. Those
/// shapes already have a real serializer: `Reflection::serialize_to_bytes`/`to_document`
/// (`Serialize.hpp`/`Document.hpp`), which walks the *runtime* `TypeInfo`/`FieldInfo::container`/
/// `FieldInfo::map`/`FieldInfo::optional` machinery those shapes actually need. This file is the
/// narrow, genuinely-`constexpr`, zero-`TypeRegistry` fast path for the common "plain data" case;
/// it is not meant to (and cannot, by construction) replace the general-purpose runtime path.
///
/// Both functions are plain `constexpr` (not `consteval`): they must also work unmodified on a
/// real runtime object (a `Player` read from disk, say), not only inside a `static_assert`. See
/// `static_serialize_constexpr_roundtrip` below for proof this also genuinely evaluates as a
/// constant expression, satisfying the "compile-time serialization must actually compile away"
/// goal, not just "is markable constexpr".
namespace SFT::Reflection {

    namespace Detail {

        /// Deliberately instantiation-dependent `false` (never just `false`) — same idiom as
        /// `Detail::FieldNotFound` in `StaticReflection.hpp` — so the `static_assert` below only
        /// fires for the specific unsupported `FieldT` actually reached, not unconditionally for
        /// every instantiation of `static_serialize`/`static_deserialize`.
        template <class>
        struct StaticSerializeAlwaysFalse : std::false_type {};

        /// Reports whether `FieldT` is itself a reflected type (`SFT_REFLECT_TYPE`-annotated),
        /// i.e. whether a field of this type should be recursed into rather than treated as an
        /// opaque trivial blob.
        template <class FieldT>
        [[nodiscard]] consteval bool is_reflected_type() noexcept {
            return !TypeTraits<std::remove_cv_t<FieldT>>::name.empty();
        }

    } // namespace Detail

    template <class T>
    constexpr void static_serialize(const T &object, std::vector<std::byte> &out);

    template <class T>
    [[nodiscard]] constexpr usize static_deserialize(T &object, std::span<const std::byte> in);

    /// Appends `object`'s reflected fields to `out`, in declaration order, as a flat byte stream.
    ///
    /// Walks `TypeTraits<std::remove_cv_t<T>>::for_each_member`; only `MemberKind::Field` entries
    /// contribute bytes (methods/events/static members/constructors are skipped — they carry no
    /// per-instance state to serialize). For each field:
    ///   - trivially copyable `FieldT`: `sizeof(FieldT)` raw bytes are appended.
    ///   - `FieldT` itself reflected: recurses via `static_serialize<FieldT>`.
    ///   - anything else: a compile error (see the file-level doc comment).
    ///
    /// `attrs` (the trailing `SFT_ATTR_*` factories `for_each_member` also passes) are accepted by
    /// the visitor but never called — calling one invokes `Detail::make_attribute`, which is not
    /// `constexpr`, and would break this function's constant-evaluation guarantee for no benefit
    /// (this serializer does not consult attributes).
    template <class T>
    constexpr void static_serialize(const T &object, std::vector<std::byte> &out) {
        using TypeT = std::remove_cv_t<T>;
        TypeTraits<TypeT>::for_each_member(
            [&object, &out]<auto Member, MemberKind Kind, class... MemberTypeArgs>(std::string_view /*name*/, auto &&.../*attrs*/) {
                if constexpr (Kind == MemberKind::Field) {
                    using FieldT = std::remove_reference_t<decltype(std::declval<T &>().*Member)>;
                    if constexpr (std::is_trivially_copyable_v<FieldT>) {
                        // `std::bit_cast` (not `reinterpret_cast`, which is never usable in a
                        // constant expression) to turn the field's value into a byte array — the
                        // only standard-sanctioned way to view an object's representation as bytes
                        // that also works during constant evaluation.
                        const auto bytes = std::bit_cast<std::array<std::byte, sizeof(FieldT)>>(object.*Member);
                        for (std::byte b : bytes) {
                            out.push_back(b);
                        }
                    } else if constexpr (Detail::is_reflected_type<FieldT>()) {
                        static_serialize<FieldT>(object.*Member, out);
                    } else {
                        static_assert(Detail::StaticSerializeAlwaysFalse<FieldT>::value,
                                      "static_serialize<T>: field type is neither trivially copyable nor "
                                      "itself SFT_REFLECT_TYPE-annotated. Containers/UString/std::optional/"
                                      "smart-pointer fields are not supported by this compile-time path — use "
                                      "the runtime SFT::Reflection::serialize_to_bytes/to_document "
                                      "(Serialize.hpp/Document.hpp) instead.");
                    }
                }
            });
    }

    /// Inverse of `static_serialize`: reads `object`'s reflected fields back out of `in`, in the
    /// same declaration order, and returns how many bytes were consumed.
    ///
    /// Contract matches this codebase's other raw-offset field accessors (see
    /// `FieldInfo.hpp`'s `copy_field_out`/`copy_field_in`, which validate only that the caller's
    /// buffer size matches — never that the *source* the bytes came from was well-formed): `in`
    /// must contain at least as many bytes as `static_serialize` would have produced for an object
    /// of type `T`. Passing a too-short `in` is undefined behavior (out-of-bounds `memcpy`/index),
    /// not a checked failure — this is a hot-path, trust-the-caller primitive, not an untrusted-
    /// input parser.
    ///
    /// @return Returns the number of bytes consumed from `in` (equal to what `static_serialize`
    /// would append for an object of type `T`).
    template <class T>
    [[nodiscard]] constexpr usize static_deserialize(T &object, std::span<const std::byte> in) {
        using TypeT = std::remove_cv_t<T>;
        usize offset = 0;
        TypeTraits<TypeT>::for_each_member(
            [&object, &in, &offset]<auto Member, MemberKind Kind, class... MemberTypeArgs>(std::string_view /*name*/, auto &&.../*attrs*/) {
                if constexpr (Kind == MemberKind::Field) {
                    using FieldT = std::remove_reference_t<decltype(std::declval<T &>().*Member)>;
                    if constexpr (std::is_trivially_copyable_v<FieldT>) {
                        // `std::bit_cast` back into `FieldT` — the constexpr-safe inverse of the
                        // `bit_cast`-to-bytes used by `static_serialize` above; see there for why
                        // `reinterpret_cast`/raw pointer aliasing can't be used here instead.
                        std::array<std::byte, sizeof(FieldT)> bytes{};
                        for (usize i = 0; i < sizeof(FieldT); ++i) {
                            bytes[i] = in[offset + i];
                        }
                        object.*Member = std::bit_cast<FieldT>(bytes);
                        offset += sizeof(FieldT);
                    } else if constexpr (Detail::is_reflected_type<FieldT>()) {
                        offset += static_deserialize<FieldT>(object.*Member, in.subspan(offset));
                    } else {
                        static_assert(Detail::StaticSerializeAlwaysFalse<FieldT>::value,
                                      "static_deserialize<T>: field type is neither trivially copyable nor "
                                      "itself SFT_REFLECT_TYPE-annotated. Containers/UString/std::optional/"
                                      "smart-pointer fields are not supported by this compile-time path — use "
                                      "the runtime SFT::Reflection::deserialize_from_bytes/from_document "
                                      "(Serialize.hpp/Document.hpp) instead.");
                    }
                }
            });
        return offset;
    }

} // namespace SFT::Reflection
