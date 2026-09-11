#pragma once

#include <Reflection/Document.hpp>

#include <Foundation/Foundation.hpp>

#include <expected>
#include <string_view>

namespace SFT::Reflection {

    /// Renders `value` as JSON text. This is the default, built-in text-format encoder for the
    /// `Value` document tree (`Document.hpp`) — it knows nothing about reflection or `TypeInfo`,
    /// only about `Value`, exactly like any encoder for another format (YAML, TOML, ...) that
    /// someone else writes would. `to_document`'s result can be handed straight to this function;
    /// a hand-built `Value` works exactly the same way.
    ///
    /// @param pretty When `true`, indents nested arrays/objects with two spaces per level and
    /// inserts newlines; when `false` (the default), produces compact, single-line output.
    [[nodiscard]] UString write_json(const Value &value, bool pretty = false);

    enum class JsonParseErrorCode : u32 {
        UnexpectedCharacter,
        UnexpectedEndOfInput,
        InvalidNumber,
        InvalidEscape,
        TrailingContent,
    };

    struct JsonParseError {
        JsonParseErrorCode code = JsonParseErrorCode::UnexpectedCharacter;
        UString message;
        /// Byte offset into the input string_view where the error was detected.
        usize offset = 0;
    };

    template <class T>
    using JsonExpected = std::expected<T, JsonParseError>;

    /// Parses `text` as a single JSON value (object, array, string, number, `true`/`false`/
    /// `null`) — the reverse of `write_json`. The whole of `text` must be one value plus optional
    /// trailing whitespace; anything else left over is a `TrailingContent` error.
    [[nodiscard]] JsonExpected<Value> parse_json(std::string_view text);

} // namespace SFT::Reflection
