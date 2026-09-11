#pragma once

#include <Reflection/TypeId.hpp>

#include <Foundation/Foundation.hpp>

#include <string_view>
#include <variant>
#include <vector>

namespace SFT::Reflection {


    /// One tagged value on a field/method/type — arbitrary key-value metadata for tooling and
    /// data-driven mods, not consulted by any of the core access/invoke paths. Examples: a
    /// numeric range (`"min"`/`"max"`) for a property editor, a `"modder_visible"` flag a mod
    /// loader checks before exposing a field at all, a display name for a debug UI.
    using AttributeValue = std::variant<bool, i64, f64, UString>;

    struct Attribute {
        TypeId key{};
        UString name;
        AttributeValue value;
    };

    namespace Detail {

        /// Builds an `Attribute`, used by the `SFT_ATTR_*` macros below.
        [[nodiscard]] inline Attribute make_attribute(std::string_view name, AttributeValue value) {
            return Attribute{.key = TypeId::from_name(name), .name = UString{name}, .value = std::move(value)};
        }

    } // namespace Detail

    /// Finds an attribute by key on anything with a `std::vector<Attribute> attributes` member
    /// (`FieldInfo`, `MethodInfo`, `EventInfo`, `TypeInfo`) — one function instead of four
    /// hand-written duplicates.
    ///
    /// @return Returns a pointer to the requested attribute, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    template <class Described>
    [[nodiscard]] const Attribute *find_attribute(const Described &described, TypeId key) noexcept {
        for (const Attribute &attribute : described.attributes) {
            if (attribute.key == key) {
                return &attribute;
            }
        }
        return nullptr;
    }

    /// Same as the `TypeId` overload, looked up by name.
    template <class Described>
    [[nodiscard]] const Attribute *find_attribute(const Described &described, std::string_view name) noexcept {
        return find_attribute(described, TypeId::from_name(name));
    }


} // namespace SFT::Reflection

/// Builds a `bool`-valued attribute, for use as a trailing argument to `SFT_REFLECT_FIELD`/
/// `SFT_REFLECT_METHOD`, e.g. `SFT_REFLECT_FIELD(is_hidden, SFT_ATTR_BOOL("modder_visible", false))`.
#define SFT_ATTR_BOOL(NAME, VALUE) ::SFT::Reflection::Detail::make_attribute((NAME), static_cast<bool>(VALUE))
/// Builds an integer-valued attribute.
#define SFT_ATTR_INT(NAME, VALUE) ::SFT::Reflection::Detail::make_attribute((NAME), static_cast<SFT::i64>(VALUE))
/// Builds a floating-point-valued attribute.
#define SFT_ATTR_FLOAT(NAME, VALUE) ::SFT::Reflection::Detail::make_attribute((NAME), static_cast<SFT::f64>(VALUE))
/// Builds a string-valued attribute.
#define SFT_ATTR_STRING(NAME, VALUE) ::SFT::Reflection::Detail::make_attribute((NAME), ::SFT::Foundation::UString{VALUE})
