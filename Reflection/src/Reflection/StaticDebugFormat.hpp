#pragma once

#include <Reflection/Macros.hpp>
#include <Reflection/TypeInfo.hpp>

#include <Foundation/Foundation.hpp>

#include <string>
#include <type_traits>

/// The first real "metaprogramming" consumer of the compile-time reflection layer in the sense
/// the hybrid-reflection vision doc (`plans/reflection-hybrid-vision.md`) calls out as missing:
/// everything else that folds over a reflected type's shape (`StaticSerialize.hpp`,
/// `SchemaHash.hpp`, `ArrayReflection.hpp`) *computes a value* from it — a byte stream, a hash, an
/// array shape. This instead *generates a new piece of code* per reflected type: a whole
/// `T::debug_string`-shaped formatting function, assembled entirely from `T`'s declared fields,
/// with no hand-written per-type formatter anywhere. Point this at any `SFT_REFLECT_TYPE`d
/// struct and it produces a working `"TypeName{field: value, ...}"` renderer for free — the same
/// value proposition a script-binding generator or an RPC-stub generator would have, just scoped
/// small enough to build, test, and land in one pass.
///
/// Like `StaticSerialize.hpp`, this walks `TypeTraits<T>::for_each_member` directly (not through
/// `StaticTypeInfo<T>::fields()`'s homogeneous `StaticFieldInfo` array, which deliberately drops
/// each field's member pointer) and supports the same restricted set of field shapes: trivially
/// formattable scalars, `UString`, and fields whose type is itself reflected (recursed into).
/// Everything else is a compile error naming the unsupported field type, via the same
/// instantiation-dependent-`false` idiom used throughout this package
/// (`Detail::StaticSerializeAlwaysFalse` in `StaticSerialize.hpp`,
/// `Detail::FieldNotFound`/`Detail::MethodOverloadNotFound` in `StaticReflection.hpp`) — adding
/// container/enum/pointer support is a natural follow-up, not a redesign, exactly like
/// `StaticSerialize.hpp`'s own "v1" scope note.
///
/// Deliberately not `consteval`: unlike `StaticSerialize.hpp`'s byte-for-byte serializer, this
/// builds human-readable text via `std::to_string`, which cannot be constant-evaluated — this is
/// a logging/debugging/tooling helper meant to run at ordinary runtime against a real object (a
/// crashed entity, a mod's malformed save record), not a `static_assert`-time value.
namespace SFT::Reflection {

    namespace Detail {

        /// Deliberately instantiation-dependent `false` — same idiom as
        /// `StaticSerializeAlwaysFalse` (`StaticSerialize.hpp`), kept as its own type here rather
        /// than reused so this file's `static_assert` fires independently of that one's.
        template <class>
        struct StaticDebugFormatAlwaysFalse : std::false_type {};

    } // namespace Detail

    template <class T>
    [[nodiscard]] std::string static_debug_string(const T &object);

    /// Appends `value`'s rendering to `out`. Overloads select the formatting rule for each
    /// supported field shape; the `T`-generic overload below handles nested reflected structs by
    /// recursing into `static_debug_string<T>`.
    [[nodiscard]] inline std::string static_debug_format_value(bool value) {
        return value ? "true" : "false";
    }

    template <class Arithmetic>
    [[nodiscard]] std::string static_debug_format_value(Arithmetic value)
        requires(std::is_arithmetic_v<Arithmetic> && !std::is_same_v<Arithmetic, bool>)
    {
        return std::to_string(value);
    }

    [[nodiscard]] inline std::string static_debug_format_value(const UString &value) {
        std::string out;
        out.push_back('"');
        out += value.cpp_string_view();
        out.push_back('"');
        return out;
    }

    template <class Reflected>
    [[nodiscard]] std::string static_debug_format_value(const Reflected &value)
        requires(!std::is_arithmetic_v<Reflected> && !std::is_same_v<Reflected, UString> &&
                 !TypeTraits<std::remove_cv_t<Reflected>>::name.empty())
    {
        return static_debug_string(value);
    }

    /// Renders `object` as `"<T's canonical name>{field: value, field: value, ...}"`, in
    /// declaration order — one generated formatter per reflected type, with no hand-written
    /// per-type case anywhere in this file.
    ///
    /// @return Returns the newly formatted string.
    template <class T>
    [[nodiscard]] std::string static_debug_string(const T &object) {
        using TypeT = std::remove_cv_t<T>;
        std::string out{TypeTraits<TypeT>::name};
        out.push_back('{');
        bool first = true;
        TypeTraits<TypeT>::for_each_member(
            [&object, &out, &first]<auto Member, MemberKind Kind, class... MemberTypeArgs>(std::string_view name, auto &&.../*attrs*/) {
                if constexpr (Kind == MemberKind::Field) {
                    using FieldT = std::remove_reference_t<decltype(std::declval<const T &>().*Member)>;
                    if constexpr (requires { static_debug_format_value(std::declval<const FieldT &>()); }) {
                        if (!first) {
                            out += ", ";
                        }
                        first = false;
                        out += name;
                        out += ": ";
                        out += static_debug_format_value(object.*Member);
                    } else {
                        static_assert(Detail::StaticDebugFormatAlwaysFalse<FieldT>::value,
                                      "static_debug_string<T>: field type is not a supported shape (arithmetic, "
                                      "bool, UString, or another SFT_REFLECT_TYPE-annotated struct). Containers/"
                                      "std::optional/smart-pointer/enum fields are not supported by this v1 "
                                      "formatter.");
                    }
                }
            });
        out.push_back('}');
        return out;
    }

} // namespace SFT::Reflection
