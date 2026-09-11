#pragma once

#include <Foundation/Foundation.hpp>

#include <type_traits>

namespace SFT::Reflection {


    /// Classifies a `Trivial` value's real C++ shape (bool/signed/unsigned/floating), read off
    /// `T` at compile time when a `FieldInfo`/`ContainerInfo`/`MapInfo`/`OptionalInfo` is built.
    ///
    /// This exists for exactly one reason: a `Trivial` value's raw bytes plus its byte `size`
    /// alone aren't enough to know whether those bytes mean `true`/`false`, a negative-capable
    /// integer, an unsigned count, or a fractional number — and a text format like JSON has to
    /// pick one meaning to print. The binary serializer (`Serialize.hpp`) never needed this: it
    /// only ever `memcpy`s bytes in and back out, so it never has to interpret them as anything.
    /// `Document.hpp`'s `to_document`/`from_document` does.
    enum class PrimitiveKind : u8 {
        /// Not a recognized scalar primitive — a struct, container, map, or anything else
        /// `to_document` cannot turn directly into a JSON-ish `Bool`/`Int`/`Float`.
        None,
        Bool,
        /// A `Trivial` value whose bytes, read as `size` bytes, form a signed integer
        /// (`i8`/`i16`/`i32`/`i64`, or a signed enum's underlying type).
        SignedInt,
        /// Same as `SignedInt`, unsigned.
        UnsignedInt,
        /// A `Trivial` value whose bytes, read as `size` bytes, form `float` (`size == 4`) or
        /// `double` (`size == 8`).
        Float,
    };

    namespace Detail {

        /// Derives the `PrimitiveKind` for `T` at compile time. `std::is_enum_v` is checked via
        /// the underlying type, so a reflected `enum class : u8` reads out as an integer, not
        /// `None` — `Document.hpp` has no separate "enum" `ValueKind` of its own; an enum's
        /// *name* is only recoverable through `TypeRegistry::find_enum`/`EnumInfo`, which
        /// `to_document`/`from_document` do not attempt (see their doc comments).
        template <class T>
        [[nodiscard]] consteval PrimitiveKind primitive_kind_of() {
            using Value = std::remove_cv_t<T>;
            if constexpr (std::is_same_v<Value, bool>) {
                return PrimitiveKind::Bool;
            } else if constexpr (std::is_enum_v<Value>) {
                return std::is_unsigned_v<std::underlying_type_t<Value>> ? PrimitiveKind::UnsignedInt : PrimitiveKind::SignedInt;
            } else if constexpr (std::is_floating_point_v<Value>) {
                return PrimitiveKind::Float;
            } else if constexpr (std::is_unsigned_v<Value>) {
                return PrimitiveKind::UnsignedInt;
            } else if constexpr (std::is_signed_v<Value>) {
                return PrimitiveKind::SignedInt;
            } else {
                return PrimitiveKind::None;
            }
        }

    } // namespace Detail


} // namespace SFT::Reflection
