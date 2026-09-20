#pragma once

#include <Reflection/FixedString.hpp>
#include <Reflection/TypeId.hpp>

#include <Foundation/Foundation.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
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

    /// Which alternative a `StaticAttribute` carries. An explicit tag rather than a
    /// `std::variant` because `StaticAttribute` must be usable as a compile-time value, and
    /// `std::variant` is not a structural type (it has private members) — it can be neither an
    /// NTTP nor stored in one without losing constant-evaluation.
    enum class StaticAttributeKind : u8 {
        Bool,
        SignedInt,
        UnsignedInt,
        Float,
        String,
        Type,
        /// An enumerator of some `enum`/`enum class`. Carries both the enum *type*'s identity
        /// (`type_value`, reused rather than adding a redundant `enum_type` field — `Type` and
        /// `Enum` never occupy the same `StaticAttribute`, so there is nothing to disambiguate)
        /// and the enumerator's own integer value (`signed_value`, reused for the same reason:
        /// every enumerator's underlying value fits in an `i64`, which is exactly what
        /// `SignedInt` already stores).
        Enum,
        /// A small fixed-capacity, homogeneous `i64` array — see `array_storage`/`array_length`/
        /// `array_values()` below. Ints were chosen as the one element type because they already
        /// subsume the other obvious array use cases (bit flags, enumerator lists, small ID sets)
        /// without needing a variant-of-arrays.
        Array,
    };

    /// How many elements a single `Array`-kind `StaticAttribute` can hold inline. Mirrors
    /// `StaticReflection.hpp`'s `max_static_attribute_count`/`max_static_parameter_count`: a
    /// `StaticAttribute` must stay one flat, fixed-size struct so it can be assigned into a plain
    /// `std::array<StaticAttribute, N>` element (see `static_attribute_array` in
    /// `StaticReflection.hpp`), so the array payload lives inline with a capacity/length pair
    /// rather than as a heap-backed container.
    inline constexpr usize max_static_attribute_array_length = 8;

    /// The compile-time counterpart of `Attribute` — the design doc's `StaticAttribute`.
    ///
    /// `Attribute` owns a `UString` and is therefore unusable during constant evaluation; this
    /// carries the same information in literal types only (`std::string_view`, `TypeId`, and
    /// scalars selected by `kind`), so a member's attributes can be inspected in a `static_assert`
    /// and compiled away entirely when nothing reads them at runtime. The alternatives sit side by
    /// side rather than in a union: the whole struct is a handful of words that only ever exists
    /// during compilation, so overlapping them would trade constexpr-friendliness for nothing.
    struct StaticAttribute {
        TypeId key{};
        std::string_view name;
        StaticAttributeKind kind = StaticAttributeKind::Bool;
        bool bool_value = false;
        i64 signed_value = 0;
        u64 unsigned_value = 0;
        f64 float_value = 0.0;
        std::string_view string_value;
        TypeId type_value{};
        std::array<i64, max_static_attribute_array_length> array_storage{};
        usize array_length = 0;

        /// The populated prefix of `array_storage` — meaningful only when `kind ==
        /// StaticAttributeKind::Array`. Same "fixed capacity + length, sliced by a `std::span`"
        /// idiom as `StaticFieldInfo::attributes()`/`StaticMethodInfo::parameter_types()` in
        /// `StaticReflection.hpp`.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr std::span<const i64> array_values() const noexcept {
            return std::span<const i64>{array_storage.data(), array_length};
        }

        /// Compares the operands for equality.
        ///
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(const StaticAttribute &, const StaticAttribute &) noexcept = default;
    };

    namespace Detail {

        /// Builds an `Attribute`, used by the `SFT_ATTR_*` macros below.
        [[nodiscard]] inline Attribute make_attribute(std::string_view name, AttributeValue value) {
            return Attribute{.key = TypeId::from_name(name), .name = UString{name}, .value = std::move(value)};
        }

        /// Carries one attribute's name and value as *template arguments*, so a single declaration
        /// can be read both at compile time (`static_value()`) and at runtime (`operator()`). See
        /// `SFT_ATTR_BOOL`'s doc comment for why this must be a type rather than a value.
        template <FixedString Name, StaticAttributeKind Kind, auto Value>
        struct AttributeFactory {
            /// Returns the compile-time form.
            [[nodiscard]] static consteval StaticAttribute static_value() noexcept {
                // Default-construct then assign, rather than a partial designated initializer:
                // naming only some members trips -Wmissing-designated-field-initializers, and the
                // unnamed alternatives are meant to stay at their defaults by design (only the one
                // selected by `kind` is meaningful).
                StaticAttribute attribute{};
                attribute.key = TypeId::from_name(Name.view());
                attribute.name = Name.view();
                attribute.kind = Kind;
                if constexpr (Kind == StaticAttributeKind::Bool) {
                    attribute.bool_value = Value;
                } else if constexpr (Kind == StaticAttributeKind::SignedInt) {
                    attribute.signed_value = Value;
                } else if constexpr (Kind == StaticAttributeKind::UnsignedInt) {
                    attribute.unsigned_value = Value;
                } else if constexpr (Kind == StaticAttributeKind::Float) {
                    attribute.float_value = Value;
                } else if constexpr (Kind == StaticAttributeKind::String) {
                    attribute.string_value = Value.view();
                } else {
                    static_assert(Kind == StaticAttributeKind::Type,
                                  "AttributeFactory only models the Bool/SignedInt/UnsignedInt/Float/"
                                  "String/Type kinds; Enum is built by Detail::EnumAttributeFactory and "
                                  "Array by Detail::ArrayAttributeFactory instead — both need an extra "
                                  "template parameter (the enum's TypeId, or the value pack) that this "
                                  "single-`Value` factory has no slot for.");
                    attribute.type_value = Value;
                }
                return attribute;
            }

            /// Materializes the runtime `Attribute`. Called only from the non-`constexpr`
            /// descriptor builders in `Macros.hpp`, never during a `consteval` walk.
            [[nodiscard]] Attribute operator()() const {
                if constexpr (Kind == StaticAttributeKind::Bool) {
                    return make_attribute(Name.view(), static_cast<bool>(Value));
                } else if constexpr (Kind == StaticAttributeKind::SignedInt) {
                    return make_attribute(Name.view(), static_cast<i64>(Value));
                } else if constexpr (Kind == StaticAttributeKind::UnsignedInt) {
                    // `AttributeValue` has no unsigned alternative; widening to `i64` keeps one
                    // runtime representation rather than growing the variant for a distinction the
                    // runtime side has never needed to make.
                    return make_attribute(Name.view(), static_cast<i64>(Value));
                } else if constexpr (Kind == StaticAttributeKind::Float) {
                    return make_attribute(Name.view(), static_cast<f64>(Value));
                } else if constexpr (Kind == StaticAttributeKind::String) {
                    return make_attribute(Name.view(), UString{Value.view()});
                } else {
                    // `Kind == Type`. A `TypeId`-valued attribute has no runtime `AttributeValue`
                    // alternative either. Rather than widen the variant, it degrades to the
                    // attribute's own name on the runtime side; the compile-time side
                    // (`static_value().type_value`) is where the real identity lives, and a tool
                    // wanting it at runtime should read the static descriptor.
                    static_assert(Kind == StaticAttributeKind::Type,
                                  "AttributeFactory only models the Bool/SignedInt/UnsignedInt/Float/"
                                  "String/Type kinds; see static_value()'s static_assert for why.");
                    return make_attribute(Name.view(), UString{Name.view()});
                }
            }
        };

        /// Sibling to `AttributeFactory` for `StaticAttributeKind::Enum`. A plain `auto Value` NTTP
        /// captures the enumerator itself, but recovering the enum *type*'s own `TypeId` from
        /// `decltype(Value)` needs `type_id<T>()`, which this header cannot call directly: unlike a
        /// dependent-argument-only call, `type_id<decltype(Value)>()` uses an explicit template
        /// argument list, so the compiler must already know `type_id` names a template via ordinary
        /// unqualified lookup at the point this template is *defined* — and `Attribute.hpp`
        /// deliberately never includes `StaticTypeId.hpp` (see `SFT_ATTR_TYPE`'s doc comment for the
        /// include cycle that would create). So `SFT_ATTR_ENUM` computes the `TypeId` itself, in the
        /// macro expansion at the *use* site (where `StaticTypeId.hpp` is already visible via
        /// `Macros.hpp`), and hands it in here as an ordinary extra template argument — the same
        /// "compute at the use site" trick `SFT_ATTR_TYPE` already relies on, just one level further
        /// out because `Enum` needs two pieces of information instead of one.
        template <FixedString Name, TypeId EnumType, auto Value>
        struct EnumAttributeFactory {
            /// Returns the compile-time form. `type_value` is the enum type's identity;
            /// `signed_value` (reused rather than adding a redundant field — `Type` and `Enum`
            /// never share a `StaticAttribute`) is the enumerator's own integer value.
            [[nodiscard]] static consteval StaticAttribute static_value() noexcept {
                StaticAttribute attribute{};
                attribute.key = TypeId::from_name(Name.view());
                attribute.name = Name.view();
                attribute.kind = StaticAttributeKind::Enum;
                attribute.type_value = EnumType;
                attribute.signed_value = static_cast<i64>(Value);
                return attribute;
            }

            /// Materializes the runtime `Attribute`. Unlike `Type`, an enumerator's numeric value
            /// *is* useful on its own at runtime (an editor can still show/compare "3" even without
            /// the enum's name), so this degrades to the underlying integer rather than to the
            /// attribute's own name; only the enum *type* identity (`static_value().type_value`) is
            /// compile-time-only.
            [[nodiscard]] Attribute operator()() const {
                return make_attribute(Name.view(), static_cast<i64>(Value));
            }
        };

        /// Sibling to `AttributeFactory` for `StaticAttributeKind::Array`: the payload is a
        /// variadic pack rather than one `auto Value`, which `AttributeFactory` cannot express
        /// without turning every scalar kind's single-value branches into pack-of-one handling for
        /// no benefit — the scalar macros stay exactly as they were. Only `i64`-convertible packs
        /// are supported (see `StaticAttributeKind::Array`'s doc comment for why one element type
        /// is enough for v1).
        template <FixedString Name, auto... Values>
        struct ArrayAttributeFactory {
            /// Returns the compile-time form, with the full-fidelity array in `array_values()`.
            [[nodiscard]] static consteval StaticAttribute static_value() noexcept {
                static_assert(sizeof...(Values) <= max_static_attribute_array_length,
                              "This attribute declares more elements than StaticAttribute can hold "
                              "inline; raise SFT::Reflection::max_static_attribute_array_length.");
                StaticAttribute attribute{};
                attribute.key = TypeId::from_name(Name.view());
                attribute.name = Name.view();
                attribute.kind = StaticAttributeKind::Array;
                attribute.array_length = sizeof...(Values);
                if constexpr (sizeof...(Values) > 0) {
                    // A zero-size C array is ill-formed, so the fold is only built under the
                    // `if constexpr` guard rather than relying on `sizeof...(Values) == 0` alone to
                    // make the empty-pack instantiation well-formed.
                    const i64 values[] = {static_cast<i64>(Values)...};
                    for (usize i = 0; i < sizeof...(Values); ++i) {
                        attribute.array_storage[i] = values[i];
                    }
                }
                return attribute;
            }

            /// Materializes the runtime `Attribute`. `AttributeValue` has no array alternative (see
            /// this header's top-level comment for why one was deliberately not added), so this
            /// degrades to a comma-delimited textual rendering, e.g. `"1,2,3"` — good enough for a
            /// debug UI or log line, but lossy in the sense that a consumer wanting the real `i64`s
            /// back would have to re-parse it. Code that needs the full-fidelity array must read
            /// `static_value().array_values()` at compile time instead; this is the same trade-off
            /// `AttributeFactory`'s `Type`/`Enum` branches already make for their compile-time-only
            /// pieces.
            [[nodiscard]] Attribute operator()() const {
                std::string rendered;
                bool first = true;
                for (i64 value : {static_cast<i64>(Values)...}) {
                    if (!first) {
                        rendered += ',';
                    }
                    rendered += std::to_string(value);
                    first = false;
                }
                return make_attribute(Name.view(), UString{rendered});
            }
        };

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
///
/// Expands to an instance of an empty `Detail::AttributeFactory` whose name and value are template
/// arguments, which is what lets one macro serve both worlds:
///
///   - the runtime path (`Detail::build_field_info` and friends) calls `operator()` to materialize
///     a real `Attribute`, heap-allocated `UString` and all;
///   - the compile-time path (`StaticTypeInfo<T>::fields()`) reads `static_value()` instead — a
///     `consteval` `StaticAttribute` built from those same template arguments.
///
/// It must also stay a *type* rather than an already-constructed value, for a subtler reason: these
/// expressions are passed as ordinary arguments to `TypeTraits<T>::for_each_member`'s visitor, and
/// that same call is constant-evaluated by the `consteval` walks in `StaticReflection.hpp`.
/// `Detail::make_attribute` is not `constexpr` (it builds a `UString`), so constructing an
/// `Attribute` right here would poison every `consteval` walk over any type with an attributed
/// member. Constructing an empty class type is always a constant expression, whatever its member
/// functions would do if they were called.
#define SFT_ATTR_BOOL(NAME, VALUE)                            \
    ::SFT::Reflection::Detail::AttributeFactory<              \
        ::SFT::Reflection::Detail::FixedString{NAME},         \
        ::SFT::Reflection::StaticAttributeKind::Bool,         \
        static_cast<bool>(VALUE)> {}
/// Builds a signed-integer-valued attribute. See `SFT_ATTR_BOOL`.
#define SFT_ATTR_INT(NAME, VALUE)                             \
    ::SFT::Reflection::Detail::AttributeFactory<              \
        ::SFT::Reflection::Detail::FixedString{NAME},         \
        ::SFT::Reflection::StaticAttributeKind::SignedInt,    \
        static_cast<SFT::i64>(VALUE)> {}
/// Builds an unsigned-integer-valued attribute. See `SFT_ATTR_BOOL`.
#define SFT_ATTR_UINT(NAME, VALUE)                            \
    ::SFT::Reflection::Detail::AttributeFactory<              \
        ::SFT::Reflection::Detail::FixedString{NAME},         \
        ::SFT::Reflection::StaticAttributeKind::UnsignedInt,  \
        static_cast<SFT::u64>(VALUE)> {}
/// Builds a floating-point-valued attribute. See `SFT_ATTR_BOOL`.
#define SFT_ATTR_FLOAT(NAME, VALUE)                           \
    ::SFT::Reflection::Detail::AttributeFactory<              \
        ::SFT::Reflection::Detail::FixedString{NAME},         \
        ::SFT::Reflection::StaticAttributeKind::Float,        \
        static_cast<SFT::f64>(VALUE)> {}
/// Builds a string-valued attribute. The value is a `FixedString` template argument rather than a
/// `std::string_view` one because `std::string_view` is not a structural type and so cannot be an
/// NTTP. See `SFT_ATTR_BOOL`.
#define SFT_ATTR_STRING(NAME, VALUE)                          \
    ::SFT::Reflection::Detail::AttributeFactory<              \
        ::SFT::Reflection::Detail::FixedString{NAME},         \
        ::SFT::Reflection::StaticAttributeKind::String,       \
        ::SFT::Reflection::Detail::FixedString{VALUE}> {}
/// Builds a `TypeId`-valued attribute — tagging a member with a type, e.g. which type an editor
/// should offer in a picker for it. Expands to a `type_id<TYPE>()` call, so the use site must have
/// `StaticTypeId.hpp` visible (every user of the `SFT_REFLECT_*` macros already does, via
/// `Macros.hpp`); this header deliberately does not include it, since doing so would cycle back
/// through `TypeInfo.hpp`/`EnumInfo.hpp` into this one. Compile-time only in full fidelity; see `AttributeFactory`'s
/// `operator()` for how it degrades on the runtime side. See `SFT_ATTR_BOOL`.
#define SFT_ATTR_TYPE(NAME, TYPE)                             \
    ::SFT::Reflection::Detail::AttributeFactory<              \
        ::SFT::Reflection::Detail::FixedString{NAME},         \
        ::SFT::Reflection::StaticAttributeKind::Type,         \
        ::SFT::Reflection::type_id<TYPE>()> {}
/// Builds an enum-valued attribute, e.g. `SFT_ATTR_ENUM("category", DamageType::Fire)`. Expands to
/// a `type_id<decltype(VALUE)>()` call at the *macro's own use site* (not inside `Attribute.hpp`) —
/// see `Detail::EnumAttributeFactory`'s doc comment for why it has to happen there rather than
/// inside `static_value()` the way `SFT_ATTR_TYPE` might suggest. See `SFT_ATTR_BOOL`.
#define SFT_ATTR_ENUM(NAME, VALUE)                                             \
    ::SFT::Reflection::Detail::EnumAttributeFactory<                          \
        ::SFT::Reflection::Detail::FixedString{NAME},                        \
        ::SFT::Reflection::type_id<std::decay_t<decltype(VALUE)>>(),        \
        (VALUE)> {}
/// Builds a small fixed-capacity `i64`-array-valued attribute, e.g.
/// `SFT_REFLECT_FIELD(slot, SFT_ATTR_INT_ARRAY("valid_slots", 1, 2, 3))`. Unlike the single-value
/// `SFT_ATTR_*` macros, this expands to `Detail::ArrayAttributeFactory` rather than
/// `Detail::AttributeFactory`, since the payload is a pack of NTTPs, not one — see that type's doc
/// comment for why it is a sibling rather than a generalization of `AttributeFactory`. Each `VALUES`
/// element is `static_cast<i64>`'d individually so a mix of int-like literal types (int, bool,
/// enumerators, ...) all just work. Full-fidelity only at compile time via
/// `StaticAttribute::array_values()`; see `ArrayAttributeFactory::operator()` for the lossy runtime
/// degradation. See `SFT_ATTR_BOOL`.
#define SFT_ATTR_INT_ARRAY(NAME, ...)                         \
    ::SFT::Reflection::Detail::ArrayAttributeFactory<         \
        ::SFT::Reflection::Detail::FixedString{NAME},         \
        __VA_ARGS__> {}
