#pragma once

#include <Reflection/TypeInfo.hpp>
#include <Reflection/TypeRegistry.hpp>

#include <Foundation/Foundation.hpp>

#include <expected>
#include <string_view>
#include <vector>

namespace SFT::Reflection {

    /// What kind of value a `Value` currently holds — the JSON-ish value model every text
    /// serializer in this package (`Json.hpp`'s `write_json`/`parse_json`, or one you write
    /// yourself) is built on. Deliberately not called `JsonValue`/`JsonKind`: this type knows
    /// nothing about JSON syntax at all — it is the *only* thing `to_document`/`from_document`
    /// below produce or consume, and it is a completely ordinary, public, mutable value type you
    /// can build, inspect, and rewrite by hand with no reflection or text-format code involved.
    enum class ValueKind : u8 {
        Null,
        Bool,
        Int,
        Float,
        String,
        Array,
        Object,
    };

    /// A small, format-agnostic tree value — this package's document model. `to_document` turns a
    /// reflected object into one of these; `from_document` turns one back into a reflected
    /// object; `Json.hpp`'s `write_json`/`parse_json` are the only things in this package that
    /// know a `Value` can also be *text*.
    ///
    /// **This is the extensibility seam.** Every field, every accessor, is public and ordinary —
    /// nothing about `Value` is JSON-specific or hidden behind the serializer. Writing a YAML,
    /// TOML, or custom-format encoder/decoder means writing a function `Value -> your text` (or
    /// the reverse), with zero changes to `to_document`, `from_document`, or anything about how
    /// reflection is walked — exactly the same way `write_json`/`parse_json` are implemented, in
    /// a separate file, using only the public interface below.
    ///
    /// Internally backed by two parallel `std::vector`s rather than a `vector<pair<key, Value>>`
    /// or a `variant` holding `Value` recursively — both would make `Value` self-referential in a
    /// way that either needs an incomplete-type dance or a `unique_ptr` indirection; two plain
    /// vectors avoid the question entirely at the cost of only using `keys_` when `kind() ==
    /// Object`. Object member order is insertion order, not sorted/hashed — readability and
    /// diff-friendliness matter more than lookup speed for a value with at most a few dozen
    /// entries (one per reflected field).
    class Value {
      public:
        /// Constructs a `Null` value.
        Value() noexcept = default;
        Value(bool value) noexcept : kind_(ValueKind::Bool), bool_value_(value) {}
        Value(i64 value) noexcept : kind_(ValueKind::Int), int_value_(value) {}
        Value(f64 value) noexcept : kind_(ValueKind::Float), float_value_(value) {}
        Value(UString value) noexcept : kind_(ValueKind::String), string_value_(std::move(value)) {}
        Value(std::string_view value) : kind_(ValueKind::String), string_value_(UString{value}) {}

        /// Constructs an empty `Array` value.
        [[nodiscard]] static Value make_array() noexcept {
            Value value;
            value.kind_ = ValueKind::Array;
            return value;
        }

        /// Constructs an empty `Object` value.
        [[nodiscard]] static Value make_object() noexcept {
            Value value;
            value.kind_ = ValueKind::Object;
            return value;
        }

        [[nodiscard]] ValueKind kind() const noexcept {
            return kind_;
        }
        [[nodiscard]] bool is_null() const noexcept {
            return kind_ == ValueKind::Null;
        }
        [[nodiscard]] bool is_bool() const noexcept {
            return kind_ == ValueKind::Bool;
        }
        [[nodiscard]] bool is_int() const noexcept {
            return kind_ == ValueKind::Int;
        }
        [[nodiscard]] bool is_float() const noexcept {
            return kind_ == ValueKind::Float;
        }
        /// True for either `Int` or `Float` — the two kinds `as_number()` accepts.
        [[nodiscard]] bool is_number() const noexcept {
            return kind_ == ValueKind::Int || kind_ == ValueKind::Float;
        }
        [[nodiscard]] bool is_string() const noexcept {
            return kind_ == ValueKind::String;
        }
        [[nodiscard]] bool is_array() const noexcept {
            return kind_ == ValueKind::Array;
        }
        [[nodiscard]] bool is_object() const noexcept {
            return kind_ == ValueKind::Object;
        }

        /// Returns the boolean value. Meaningless (always `false`) unless `is_bool()`.
        [[nodiscard]] bool as_bool() const noexcept {
            return bool_value_;
        }
        /// Returns the integer value. Meaningless (always `0`) unless `is_int()`.
        [[nodiscard]] i64 as_int() const noexcept {
            return int_value_;
        }
        /// Returns the floating-point value. Meaningless (always `0`) unless `is_float()`.
        [[nodiscard]] f64 as_float() const noexcept {
            return float_value_;
        }
        /// Returns the value as `f64` regardless of whether it's `Int` or `Float` — convenient
        /// for a caller that doesn't care which a parsed number turned out to be (`parse_json`
        /// only ever produces `Int` for a text number with no `.`/exponent, `Float` otherwise).
        /// Meaningless (always `0`) unless `is_number()`.
        [[nodiscard]] f64 as_number() const noexcept {
            return kind_ == ValueKind::Int ? static_cast<f64>(int_value_) : float_value_;
        }
        /// Returns the string value. Meaningless (empty) unless `is_string()`.
        [[nodiscard]] const UString &as_string() const noexcept {
            return string_value_;
        }

        /// Returns the number of `Array`/`Object` elements. `0` for any other kind.
        [[nodiscard]] usize size() const noexcept {
            return elements_.size();
        }
        /// Indexes an `Array`. `index` must be `< size()`. Meaningless unless `is_array()`.
        [[nodiscard]] const Value &at(usize index) const noexcept {
            return elements_[index];
        }
        [[nodiscard]] Value &at(usize index) noexcept {
            return elements_[index];
        }
        /// Appends `value` to an `Array`. Only meaningful on a value built via `make_array()`.
        void push_back(Value value) {
            elements_.push_back(std::move(value));
        }

        /// Returns the key of the `index`th `Object` member, in insertion order. `index` must be
        /// `< size()`. Meaningless unless `is_object()`.
        [[nodiscard]] const UString &key_at(usize index) const noexcept {
            return keys_[index];
        }
        /// Returns the value of the `index`th `Object` member. See `key_at`.
        [[nodiscard]] const Value &value_at(usize index) const noexcept {
            return elements_[index];
        }
        [[nodiscard]] Value &value_at(usize index) noexcept {
            return elements_[index];
        }
        /// Inserts a new member or overwrites an existing one with the same key, preserving that
        /// key's original position on overwrite. Only meaningful on a value built via
        /// `make_object()`.
        void set(std::string_view key, Value value) {
            for (usize i = 0; i < keys_.size(); ++i) {
                if (keys_[i].cpp_string_view() == key) {
                    elements_[i] = std::move(value);
                    return;
                }
            }
            keys_.emplace_back(key);
            elements_.push_back(std::move(value));
        }
        /// Finds a member by key.
        ///
        /// @return Returns a pointer to the member's value, or `nullptr` when no member has that
        /// key (or this value isn't an `Object`).
        [[nodiscard]] const Value *find(std::string_view key) const noexcept {
            for (usize i = 0; i < keys_.size(); ++i) {
                if (keys_[i].cpp_string_view() == key) {
                    return &elements_[i];
                }
            }
            return nullptr;
        }

      private:
        ValueKind kind_ = ValueKind::Null;
        bool bool_value_ = false;
        i64 int_value_ = 0;
        f64 float_value_ = 0.0;
        UString string_value_;
        /// Array elements when `kind_ == Array`; object member values (parallel to `keys_`) when
        /// `kind_ == Object`; unused otherwise.
        std::vector<Value> elements_;
        /// Object member keys, parallel to `elements_`. Unused unless `kind_ == Object`.
        std::vector<UString> keys_;
    };

    enum class DocumentErrorCode : u32 {
        /// A field's type is not `Trivial`, not `UString`, and not itself a registered
        /// `TypeInfo` — there is no generic way to turn it into a `Value`.
        UnsupportedFieldType,
        /// A required nested type (a non-trivial field's `field_type`) is not registered in
        /// `TypeRegistry`.
        NestedTypeNotRegistered,
        /// `from_document` found a `Value` of the wrong `ValueKind` for the field it corresponds
        /// to (e.g. a string where a number was expected) — expected for hand-edited or
        /// foreign-authored documents, not just a programming error.
        TypeMismatch,
        /// An object field's document had no member with that field's name.
        MissingField,
        /// `TypeInfo::default_construct` was needed (to receive a value) and is null.
        NotDefaultConstructible,
    };

    struct DocumentError {
        DocumentErrorCode code = DocumentErrorCode::UnsupportedFieldType;
        UString message;
    };

    template <class T>
    using DocumentExpected = std::expected<T, DocumentError>;

    /// Walks `type.fields` (declaration order) and builds a `Value::make_object()` describing
    /// `object` — the format-agnostic counterpart to `serialize_to_bytes`. Every field shape
    /// `Serialize.hpp` supports, this supports too: `Trivial` scalars (via `FieldInfo::
    /// primitive_kind`, so a `bool`/int/float field becomes a real JSON-ish `Bool`/`Int`/`Float`,
    /// not an opaque number of bytes), `UString` (`String`), nested reflected structs
    /// (recursively, `Object`), `std::vector<T>` (`Array`), `std::unordered_map<K, V>` (`Object`
    /// when `K` is `String`/a recognized primitive — stringified — else an `Array` of `{"key":
    /// ..., "value": ...}` objects, since a text-document object's keys must be strings), and
    /// `std::optional<T>` (the contained value's document, or `Null` when empty).
    [[nodiscard]] DocumentExpected<Value> to_document(const TypeInfo &type, const void *object);

    /// The reverse of `to_document`: reads `document` (which must be an `Object`) into `object`,
    /// which must already be a live, default-constructed instance of `type` — same contract as
    /// `deserialize_from_bytes`. A field present in `type` but missing from `document` is left
    /// untouched (its existing value in `object` survives) rather than treated as an error —
    /// this is what makes an old, field-added-since document still load; a genuinely required
    /// field being absent is the caller's concern, not this function's, since reflection alone
    /// cannot know which fields are "required."
    [[nodiscard]] DocumentExpected<void> from_document(const TypeInfo &type, void *object, const Value &document);

} // namespace SFT::Reflection
