#pragma once

#include <Reflection/Attribute.hpp>
#include <Reflection/TypeId.hpp>

#include <Foundation/Foundation.hpp>

#include <string_view>
#include <vector>

namespace SFT::Reflection {


    /// One name<->value pair of a reflected enum.
    struct EnumeratorInfo {
        UString name;
        /// Widest common integer representation — every enum's underlying type fits in an `i64`
        /// (the largest standard integer type an enum's underlying type may be, per the
        /// language), so this is enough to hold any enumerator value losslessly.
        i64 value = 0;
        /// Arbitrary tooling/mod-facing metadata. See `find_attribute`.
        std::vector<Attribute> attributes;
    };

    /// A reflected `enum`/`enum class`, registered separately from `TypeInfo` (via
    /// `TypeRegistry::enum_type<T>()`/`find_enum`, not `type<T>()`/`find`) since an enum has no
    /// fields, methods, or constructors — only a name<->value mapping. The Java-`Enum`/
    /// `Class.getEnumConstants()` analog.
    struct EnumInfo {
        TypeId key{};
        UString canonical_name;
        /// Identifies the enum's underlying integer type (see `type_id_for`).
        TypeId underlying_type{};
        usize size = 0;
        usize align = 0;
        std::vector<EnumeratorInfo> enumerators;
        /// Arbitrary tooling/mod-facing metadata for the enum type itself. See `find_attribute`.
        std::vector<Attribute> attributes;

        /// Finds the requested enumerator by name.
        ///
        /// @return Returns a pointer to the requested enumerator, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const EnumeratorInfo *find_enumerator(std::string_view enumerator_name) const noexcept;
        /// Finds the requested enumerator by value. Returns the first match in declaration order
        /// when more than one enumerator shares a value (aliases are legal in C++).
        ///
        /// @return Returns a pointer to the requested enumerator, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const EnumeratorInfo *find_enumerator(i64 value) const noexcept;
    };

    /// Specialized per reflected enum via `SFT_REFLECT_ENUM`/`SFT_REFLECT_ENUM_VALUE`/
    /// `SFT_REFLECT_ENUM_END` (`Macros.hpp`). The primary template declares only `name`, matching
    /// `TypeTraits<T>`'s discipline.
    template <class T>
    struct EnumTraits {
        static constexpr std::string_view name{};
    };


} // namespace SFT::Reflection
