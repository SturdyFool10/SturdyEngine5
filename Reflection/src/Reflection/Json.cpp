#include <Reflection/Json.hpp>

#include <charconv>
#include <cmath>
#include <string>

namespace SFT::Reflection {

    namespace {

        void write_indent(std::string &out, bool pretty, u32 depth) {
            if (!pretty) {
                return;
            }
            out.push_back('\n');
            out.append(static_cast<usize>(depth) * 2, ' ');
        }

        void write_json_string(std::string &out, std::string_view text) {
            out.push_back('"');
            for (const char c : text) {
                switch (c) {
                case '"':
                    out.append("\\\"");
                    break;
                case '\\':
                    out.append("\\\\");
                    break;
                case '\b':
                    out.append("\\b");
                    break;
                case '\f':
                    out.append("\\f");
                    break;
                case '\n':
                    out.append("\\n");
                    break;
                case '\r':
                    out.append("\\r");
                    break;
                case '\t':
                    out.append("\\t");
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        static constexpr char hex_digits[] = "0123456789abcdef";
                        out.append("\\u00");
                        out.push_back(hex_digits[(static_cast<unsigned char>(c) >> 4) & 0xF]);
                        out.push_back(hex_digits[static_cast<unsigned char>(c) & 0xF]);
                    } else {
                        out.push_back(c);
                    }
                    break;
                }
            }
            out.push_back('"');
        }

        void write_json_number(std::string &out, f64 value) {
            if (!std::isfinite(value)) {
                // JSON has no representation for NaN/Infinity; `null` is the least surprising
                // stand-in (matches what most JSON libraries do when asked to serialize one).
                out.append("null");
                return;
            }
            std::array<char, 64> buffer{};
            auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
            out.append(buffer.data(), result.ptr);
        }

        void write_json_value(std::string &out, const Value &value, bool pretty, u32 depth) {
            switch (value.kind()) {
            case ValueKind::Null:
                out.append("null");
                return;
            case ValueKind::Bool:
                out.append(value.as_bool() ? "true" : "false");
                return;
            case ValueKind::Int: {
                std::array<char, 32> buffer{};
                auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value.as_int());
                out.append(buffer.data(), result.ptr);
                return;
            }
            case ValueKind::Float:
                write_json_number(out, value.as_float());
                return;
            case ValueKind::String:
                write_json_string(out, value.as_string().cpp_string_view());
                return;
            case ValueKind::Array: {
                out.push_back('[');
                for (usize i = 0; i < value.size(); ++i) {
                    if (i != 0) {
                        out.push_back(',');
                    }
                    write_indent(out, pretty, depth + 1);
                    write_json_value(out, value.at(i), pretty, depth + 1);
                }
                if (value.size() != 0) {
                    write_indent(out, pretty, depth);
                }
                out.push_back(']');
                return;
            }
            case ValueKind::Object: {
                out.push_back('{');
                for (usize i = 0; i < value.size(); ++i) {
                    if (i != 0) {
                        out.push_back(',');
                    }
                    write_indent(out, pretty, depth + 1);
                    write_json_string(out, value.key_at(i).cpp_string_view());
                    out.push_back(':');
                    if (pretty) {
                        out.push_back(' ');
                    }
                    write_json_value(out, value.value_at(i), pretty, depth + 1);
                }
                if (value.size() != 0) {
                    write_indent(out, pretty, depth);
                }
                out.push_back('}');
                return;
            }
            }
        }

        /// Hand-written recursive-descent JSON parser — a small, dependency-free reader over the
        /// `Value` type, deliberately not reusing any other format's parsing machinery in this
        /// codebase (there isn't one for a JSON-shaped grammar) or pulling in a third-party JSON
        /// library, since the entire point of `Value` is that this is just one interchangeable
        /// encoder/decoder pair among possibly several.
        class Parser {
          public:
            explicit Parser(std::string_view text) : text_(text) {}

            [[nodiscard]] JsonExpected<Value> parse() {
                skip_whitespace();
                auto value = parse_value();
                if (!value) {
                    return value;
                }
                skip_whitespace();
                if (position_ != text_.size()) {
                    return fail(JsonParseErrorCode::TrailingContent, "unexpected content after the top-level JSON value");
                }
                return value;
            }

          private:
            std::string_view text_;
            usize position_ = 0;

            [[nodiscard]] JsonParseError make_error(JsonParseErrorCode code, std::string_view message) const {
                return JsonParseError{.code = code, .message = UString{message}, .offset = position_};
            }
            [[nodiscard]] std::unexpected<JsonParseError> fail(JsonParseErrorCode code, std::string_view message) const {
                return std::unexpected(make_error(code, message));
            }

            [[nodiscard]] bool at_end() const noexcept {
                return position_ >= text_.size();
            }
            [[nodiscard]] char peek() const noexcept {
                return text_[position_];
            }

            void skip_whitespace() noexcept {
                while (!at_end() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) {
                    ++position_;
                }
            }

            [[nodiscard]] bool consume_literal(std::string_view literal) noexcept {
                if (text_.substr(position_).starts_with(literal)) {
                    position_ += literal.size();
                    return true;
                }
                return false;
            }

            [[nodiscard]] JsonExpected<Value> parse_value() {
                if (at_end()) {
                    return fail(JsonParseErrorCode::UnexpectedEndOfInput, "expected a JSON value");
                }
                switch (peek()) {
                case '{':
                    return parse_object();
                case '[':
                    return parse_array();
                case '"':
                    return parse_string_value();
                case 't':
                    if (consume_literal("true")) {
                        return Value(true);
                    }
                    return fail(JsonParseErrorCode::UnexpectedCharacter, "invalid literal, expected 'true'");
                case 'f':
                    if (consume_literal("false")) {
                        return Value(false);
                    }
                    return fail(JsonParseErrorCode::UnexpectedCharacter, "invalid literal, expected 'false'");
                case 'n':
                    if (consume_literal("null")) {
                        return Value();
                    }
                    return fail(JsonParseErrorCode::UnexpectedCharacter, "invalid literal, expected 'null'");
                default:
                    if (peek() == '-' || (peek() >= '0' && peek() <= '9')) {
                        return parse_number();
                    }
                    return fail(JsonParseErrorCode::UnexpectedCharacter, "unexpected character where a JSON value was expected");
                }
            }

            [[nodiscard]] JsonExpected<Value> parse_object() {
                ++position_; // '{'
                Value object = Value::make_object();
                skip_whitespace();
                if (!at_end() && peek() == '}') {
                    ++position_;
                    return object;
                }
                while (true) {
                    skip_whitespace();
                    if (at_end() || peek() != '"') {
                        return fail(JsonParseErrorCode::UnexpectedCharacter, "expected a quoted member name");
                    }
                    auto key = parse_raw_string();
                    if (!key) {
                        return std::unexpected(key.error());
                    }
                    skip_whitespace();
                    if (at_end() || peek() != ':') {
                        return fail(JsonParseErrorCode::UnexpectedCharacter, "expected ':' after a member name");
                    }
                    ++position_;
                    skip_whitespace();
                    auto member_value = parse_value();
                    if (!member_value) {
                        return member_value;
                    }
                    object.set(*key, std::move(*member_value));
                    skip_whitespace();
                    if (at_end()) {
                        return fail(JsonParseErrorCode::UnexpectedEndOfInput, "unterminated object");
                    }
                    if (peek() == ',') {
                        ++position_;
                        continue;
                    }
                    if (peek() == '}') {
                        ++position_;
                        return object;
                    }
                    return fail(JsonParseErrorCode::UnexpectedCharacter, "expected ',' or '}' in object");
                }
            }

            [[nodiscard]] JsonExpected<Value> parse_array() {
                ++position_; // '['
                Value array = Value::make_array();
                skip_whitespace();
                if (!at_end() && peek() == ']') {
                    ++position_;
                    return array;
                }
                while (true) {
                    skip_whitespace();
                    auto element = parse_value();
                    if (!element) {
                        return element;
                    }
                    array.push_back(std::move(*element));
                    skip_whitespace();
                    if (at_end()) {
                        return fail(JsonParseErrorCode::UnexpectedEndOfInput, "unterminated array");
                    }
                    if (peek() == ',') {
                        ++position_;
                        continue;
                    }
                    if (peek() == ']') {
                        ++position_;
                        return array;
                    }
                    return fail(JsonParseErrorCode::UnexpectedCharacter, "expected ',' or ']' in array");
                }
            }

            [[nodiscard]] JsonExpected<Value> parse_string_value() {
                auto text = parse_raw_string();
                if (!text) {
                    return std::unexpected(text.error());
                }
                return Value(std::string_view(*text));
            }

            [[nodiscard]] JsonExpected<std::string> parse_raw_string() {
                ++position_; // opening '"'
                std::string result;
                while (true) {
                    if (at_end()) {
                        return fail(JsonParseErrorCode::UnexpectedEndOfInput, "unterminated string");
                    }
                    const char c = peek();
                    if (c == '"') {
                        ++position_;
                        return result;
                    }
                    if (c == '\\') {
                        ++position_;
                        if (at_end()) {
                            return fail(JsonParseErrorCode::UnexpectedEndOfInput, "unterminated escape sequence");
                        }
                        const char escaped = peek();
                        ++position_;
                        switch (escaped) {
                        case '"':
                            result.push_back('"');
                            break;
                        case '\\':
                            result.push_back('\\');
                            break;
                        case '/':
                            result.push_back('/');
                            break;
                        case 'b':
                            result.push_back('\b');
                            break;
                        case 'f':
                            result.push_back('\f');
                            break;
                        case 'n':
                            result.push_back('\n');
                            break;
                        case 'r':
                            result.push_back('\r');
                            break;
                        case 't':
                            result.push_back('\t');
                            break;
                        case 'u': {
                            auto code_unit = parse_hex4();
                            if (!code_unit) {
                                return std::unexpected(code_unit.error());
                            }
                            u32 codepoint = *code_unit;
                            // A UTF-16 surrogate pair spans two \\uXXXX escapes; combine them into
                            // one scalar value before re-encoding to UTF-8.
                            if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                                if (position_ + 1 >= text_.size() || text_[position_] != '\\' || text_[position_ + 1] != 'u') {
                                    return fail(JsonParseErrorCode::InvalidEscape, "unpaired UTF-16 high surrogate");
                                }
                                position_ += 2;
                                auto low = parse_hex4();
                                if (!low) {
                                    return std::unexpected(low.error());
                                }
                                if (*low < 0xDC00 || *low > 0xDFFF) {
                                    return fail(JsonParseErrorCode::InvalidEscape, "invalid UTF-16 low surrogate");
                                }
                                codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (*low - 0xDC00);
                            }
                            append_utf8(result, codepoint);
                            break;
                        }
                        default:
                            return fail(JsonParseErrorCode::InvalidEscape, "unrecognized escape sequence");
                        }
                        continue;
                    }
                    result.push_back(c);
                    ++position_;
                }
            }

            [[nodiscard]] JsonExpected<u32> parse_hex4() {
                if (position_ + 4 > text_.size()) {
                    return fail(JsonParseErrorCode::UnexpectedEndOfInput, "truncated \\u escape");
                }
                u32 value = 0;
                for (usize i = 0; i < 4; ++i) {
                    const char c = text_[position_ + i];
                    value <<= 4;
                    if (c >= '0' && c <= '9') {
                        value |= static_cast<u32>(c - '0');
                    } else if (c >= 'a' && c <= 'f') {
                        value |= static_cast<u32>(c - 'a' + 10);
                    } else if (c >= 'A' && c <= 'F') {
                        value |= static_cast<u32>(c - 'A' + 10);
                    } else {
                        return fail(JsonParseErrorCode::InvalidEscape, "invalid hex digit in \\u escape");
                    }
                }
                position_ += 4;
                return value;
            }

            static void append_utf8(std::string &out, u32 codepoint) {
                if (codepoint <= 0x7F) {
                    out.push_back(static_cast<char>(codepoint));
                } else if (codepoint <= 0x7FF) {
                    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                } else if (codepoint <= 0xFFFF) {
                    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
                    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                }
            }

            [[nodiscard]] JsonExpected<Value> parse_number() {
                const usize start = position_;
                bool is_float = false;
                if (!at_end() && peek() == '-') {
                    ++position_;
                }
                if (at_end() || peek() < '0' || peek() > '9') {
                    return fail(JsonParseErrorCode::InvalidNumber, "expected a digit");
                }
                while (!at_end() && peek() >= '0' && peek() <= '9') {
                    ++position_;
                }
                if (!at_end() && peek() == '.') {
                    is_float = true;
                    ++position_;
                    if (at_end() || peek() < '0' || peek() > '9') {
                        return fail(JsonParseErrorCode::InvalidNumber, "expected a digit after '.'");
                    }
                    while (!at_end() && peek() >= '0' && peek() <= '9') {
                        ++position_;
                    }
                }
                if (!at_end() && (peek() == 'e' || peek() == 'E')) {
                    is_float = true;
                    ++position_;
                    if (!at_end() && (peek() == '+' || peek() == '-')) {
                        ++position_;
                    }
                    if (at_end() || peek() < '0' || peek() > '9') {
                        return fail(JsonParseErrorCode::InvalidNumber, "expected a digit in exponent");
                    }
                    while (!at_end() && peek() >= '0' && peek() <= '9') {
                        ++position_;
                    }
                }

                const std::string_view token = text_.substr(start, position_ - start);
                if (is_float) {
                    f64 value = 0.0;
                    auto result = std::from_chars(token.data(), token.data() + token.size(), value);
                    if (result.ec != std::errc{}) {
                        return fail(JsonParseErrorCode::InvalidNumber, "malformed floating-point number");
                    }
                    return Value(value);
                }
                i64 value = 0;
                auto result = std::from_chars(token.data(), token.data() + token.size(), value);
                if (result.ec != std::errc{}) {
                    // A plain-integer token too large for `i64` (e.g. a 20-digit number) still
                    // round-trips as JSON text; fall back to reading it as a `Float` rather than
                    // failing the whole parse over one oversized field.
                    f64 float_value = 0.0;
                    auto float_result = std::from_chars(token.data(), token.data() + token.size(), float_value);
                    if (float_result.ec != std::errc{}) {
                        return fail(JsonParseErrorCode::InvalidNumber, "malformed integer");
                    }
                    return Value(float_value);
                }
                return Value(value);
            }
        };

    } // namespace

    UString write_json(const Value &value, bool pretty) {
        std::string out;
        write_json_value(out, value, pretty, 0);
        return UString{std::string_view(out)};
    }

    JsonExpected<Value> parse_json(std::string_view text) {
        Parser parser(text);
        return parser.parse();
    }

} // namespace SFT::Reflection
