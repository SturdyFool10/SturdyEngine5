#pragma once

#include <Foundation/src/Concepts.hpp>
#include <Foundation/src/Types.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <compare>
#include <concepts>
#include <cstring>
#include <exception>
#include <expected>
#include <fmt/format.h>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <ostream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>



using std::array;
using std::expected;
using std::format;
using std::invalid_argument;
using std::length_error;
using std::nullopt;
using std::optional;
using std::out_of_range;
using std::size_t;
using std::string;
using std::string_view;
using std::strong_ordering;
using std::unexpected;
using std::u16string;
using std::u16string_view;
using std::u32string;
using std::u32string_view;
using std::u8string;
using std::u8string_view;
using std::wstring;
using std::wstring_view;

namespace SFT::Foundation {

    class ustr;
    class USlicePattern;

    class USlice {
      public:
        static constexpr usize npos = static_cast<usize>(-1);

        constexpr explicit USlice(usize start) noexcept
            : start_(start) {
        }

        constexpr USlice(usize start, usize end) noexcept
            : start_(start), end_(end) {
        }

        [[nodiscard]] constexpr usize start() const noexcept {
            return start_;
        }

        [[nodiscard]] constexpr usize end_or(usize fallback) const noexcept {
            return has_end() ? end_ : fallback;
        }

        [[nodiscard]] constexpr bool has_end() const noexcept {
            return end_ != npos;
        }

        [[nodiscard]] constexpr USlice to(usize end) const noexcept {
            return USlice{start_, end};
        }

        [[nodiscard]] constexpr USlice until(usize end) const noexcept {
            return to(end);
        }

        [[nodiscard]] constexpr USlicePattern spread_by(usize spread) const noexcept;
        [[nodiscard]] constexpr USlicePattern by(usize spread) const noexcept;
        [[nodiscard]] constexpr USlicePattern grouped(usize grouping) const noexcept;
        [[nodiscard]] constexpr USlicePattern group(usize grouping) const noexcept;

      private:
        usize start_ = 0;
        usize end_ = npos;
    };

    class USlicePattern {
      public:
        static constexpr usize npos = USlice::npos;

        constexpr explicit USlicePattern(usize start) noexcept
            : start_(start) {
        }

        constexpr USlicePattern(usize start, usize end) noexcept
            : start_(start), end_(end) {
        }

        constexpr explicit USlicePattern(USlice range) noexcept
            : start_(range.start()), end_(range.end_or(npos)) {
        }

        [[nodiscard]] constexpr usize start() const noexcept {
            return start_;
        }

        [[nodiscard]] constexpr usize end_or(usize fallback) const noexcept {
            return has_end() ? end_ : fallback;
        }

        [[nodiscard]] constexpr bool has_end() const noexcept {
            return end_ != npos;
        }

        [[nodiscard]] constexpr usize spread() const noexcept {
            return spread_;
        }

        [[nodiscard]] constexpr usize grouping() const noexcept {
            return grouping_;
        }

        [[nodiscard]] constexpr USlicePattern to(usize end) const noexcept {
            USlicePattern result = *this;
            result.end_ = end;
            return result;
        }

        [[nodiscard]] constexpr USlicePattern until(usize end) const noexcept {
            return to(end);
        }

        [[nodiscard]] constexpr USlicePattern spread_by(usize spread) const noexcept {
            USlicePattern result = *this;
            result.spread_ = spread;
            return result;
        }

        [[nodiscard]] constexpr USlicePattern by(usize spread) const noexcept {
            return spread_by(spread);
        }

        [[nodiscard]] constexpr USlicePattern grouped(usize grouping) const noexcept {
            USlicePattern result = *this;
            result.grouping_ = grouping;
            return result;
        }

        [[nodiscard]] constexpr USlicePattern group(usize grouping) const noexcept {
            return grouped(grouping);
        }

      private:
        usize start_ = 0;
        usize end_ = npos;
        usize spread_ = 0;
        usize grouping_ = 1;
    };

    [[nodiscard]] constexpr USlicePattern USlice::spread_by(usize spread) const noexcept {
        return USlicePattern{*this}.spread_by(spread);
    }

    [[nodiscard]] constexpr USlicePattern USlice::by(usize spread) const noexcept {
        return spread_by(spread);
    }

    [[nodiscard]] constexpr USlicePattern USlice::grouped(usize grouping) const noexcept {
        return USlicePattern{*this}.grouped(grouping);
    }

    [[nodiscard]] constexpr USlicePattern USlice::group(usize grouping) const noexcept {
        return grouped(grouping);
    }

    [[nodiscard]] constexpr USlice uslice(usize start) noexcept {
        return USlice{start};
    }

    [[nodiscard]] constexpr USlice uslice(usize start, usize end) noexcept {
        return USlice{start, end};
    }

    [[nodiscard]] constexpr USlice slice_from(usize start) noexcept {
        return uslice(start);
    }

#ifdef None
#undef None
#endif

    enum class UStringValidationError : u8 {
        None,
        NullPointer,
        EmbeddedNul,
        UnexpectedContinuationByte,
        MissingContinuationByte,
        TruncatedSequence,
        OverlongEncoding,
        SurrogateCodePoint,
        CodePointTooLarge,
        InvalidLeadingByte,
    };

    /// std::string has no encoding in its type. Converting Unicode text back to it is therefore an
    /// explicit ASCII downcast; UTF-8 byte interop remains available separately through the
    /// cpp_string()/cpp_string_view() APIs.
    enum class TextConversionError : u8 {
        NonAscii,
    };

    struct UStringValidation {
        bool valid = true;
        UStringValidationError error = UStringValidationError::None;
        usize byte_index = 0;
        usize scalar_count = 0;

        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return valid;
        }
    };

    [[nodiscard]] constexpr string_view to_string(UStringValidationError error) noexcept {
        switch (error) {
        case UStringValidationError::None:
            return "none";
        case UStringValidationError::NullPointer:
            return "null pointer";
        case UStringValidationError::EmbeddedNul:
            return "embedded NUL byte";
        case UStringValidationError::UnexpectedContinuationByte:
            return "unexpected UTF-8 continuation byte";
        case UStringValidationError::MissingContinuationByte:
            return "missing UTF-8 continuation byte";
        case UStringValidationError::TruncatedSequence:
            return "truncated UTF-8 sequence";
        case UStringValidationError::OverlongEncoding:
            return "overlong UTF-8 encoding";
        case UStringValidationError::SurrogateCodePoint:
            return "UTF-16 surrogate code point";
        case UStringValidationError::CodePointTooLarge:
            return "code point above U+10FFFF";
        case UStringValidationError::InvalidLeadingByte:
            return "invalid UTF-8 leading byte";
        }

        return "unknown UTF-8 validation error";
    }

    [[nodiscard]] constexpr string_view to_string(TextConversionError error) noexcept {
        switch (error) {
        case TextConversionError::NonAscii:
            return "text contains non-ASCII code points";
        }
        return "unknown text conversion error";
    }

    namespace Detail {

        struct ResolvedSlice {
            usize start = 0;
            usize end = 0;

            [[nodiscard]] constexpr usize scalar_count() const noexcept {
                return end - start;
            }
        };

        struct ResolvedSlicePattern {
            ResolvedSlice range{};
            usize spread = 0;
            usize grouping = 1;
        };

        [[nodiscard]] ResolvedSlice resolve_slice(USlice slice, usize scalar_size, string_view owner);

        [[nodiscard]] ResolvedSlicePattern resolve_slice(USlicePattern slice, usize scalar_size, string_view owner);

        /// Length of a C string that never scans past `max_bytes` — the safe answer when a buffer's NUL
        /// terminator cannot be trusted to exist. Returns the offset of the first NUL within
        /// `[0, max_bytes)`, or `max_bytes` if the region holds no terminator. `std::memchr` reads at most
        /// `max_bytes` bytes, so a missing terminator can never trigger an over-read into unmapped memory.
        [[nodiscard]] usize bounded_c_length(const char *text, usize max_bytes) noexcept;

        /// Human-readable renderings shared by the `operator<<` overloads and the `std::formatter`
        /// specializations below, so both spell a value the same way.
        [[nodiscard]] string display_string(USlice slice);

        [[nodiscard]] string display_string(USlicePattern pattern);

        [[nodiscard]] string display_string(const UStringValidation &validation);

    } // namespace Detail

    /// `UString` is a UTF-8 owning string with hard invariants:
    /// - every stored byte sequence is strict UTF-8 (no overlong encodings, surrogates, or out-of-range scalars),
    /// - interior NUL bytes are rejected so `c_str()` cannot be truncated by C APIs,
    /// - byte size and Unicode scalar count are tracked and updated together,
    /// - mutation APIs validate replacement text before touching the current storage,
    /// - byte-boundary-sensitive operations are exposed as scalar-indexed operations by default.
    class UString {
      public:
        using value_type = char32_t;
        using size_type = usize;
        using difference_type = isize;

        static constexpr usize npos = static_cast<usize>(-1);
        static constexpr usize sso_capacity = 31;

        class CodepointIterator {
          public:
            using iterator_category = std::bidirectional_iterator_tag;
            using iterator_concept = std::bidirectional_iterator_tag;
            using value_type = char32_t;
            using difference_type = isize;

            constexpr CodepointIterator() noexcept = default;

            [[nodiscard]] char32_t operator*() const noexcept;

            CodepointIterator &operator++() noexcept;

            CodepointIterator operator++(int) noexcept;

            CodepointIterator &operator--() noexcept;

            CodepointIterator operator--(int) noexcept;

            friend bool operator==(CodepointIterator lhs, CodepointIterator rhs) noexcept;

          private:
            friend class UString;
            friend class ustr;

            constexpr CodepointIterator(const char *begin, const char *current) noexcept
                : begin_(begin), current_(current) {
            }

            const char *begin_ = nullptr;
            const char *current_ = nullptr;
        };

        class CodepointView : public std::ranges::view_interface<CodepointView> {
          public:
            constexpr CodepointView() noexcept = default;

            [[nodiscard]] constexpr CodepointIterator begin() const noexcept {
                return CodepointIterator{data_, data_};
            }

            [[nodiscard]] constexpr CodepointIterator end() const noexcept {
                return CodepointIterator{data_, data_ + byte_size_};
            }

            [[nodiscard]] constexpr usize size() const noexcept {
                return scalar_size_;
            }

          private:
            friend class UString;
            friend class ustr;

            constexpr CodepointView(const char *data, usize byte_size, usize scalar_size) noexcept
                : data_(data), byte_size_(byte_size), scalar_size_(scalar_size) {
            }

            const char *data_ = "";
            usize byte_size_ = 0;
            usize scalar_size_ = 0;
        };

        UString() noexcept;

        UString(std::nullptr_t) = delete;

        UString(const char *text);

        UString(const char *text, usize byte_count);

        UString(string_view text);

        /// Implicit bridge from legacy std::string call sites. Its bytes are validated as UTF-8;
        /// invalid input (or failure to allocate the owned copy) violates this noexcept contract
        /// and terminates rather than leaking an exception across the bridge.
        UString(const string &text) noexcept;

        UString(u8string_view text);

        UString(const ustr &text);

        UString(const UString &other);

        UString(UString &&other) noexcept;

        ~UString() noexcept;

        UString &operator=(UString other) noexcept;

        UString &operator=(string_view text);

        UString &operator=(const char *text);

        UString &operator=(u8string_view text);

        UString &operator=(const ustr &text);

        [[nodiscard]] static constexpr usize max_size() noexcept {
            return std::numeric_limits<usize>::max() / 2;
        }

        [[nodiscard]] static UStringValidation validate_utf8(string_view text) noexcept;

        [[nodiscard]] static bool is_valid_utf8(string_view text) noexcept;

        [[nodiscard]] static optional<UString> try_from_utf8(string_view text);

        [[nodiscard]] static optional<UString> try_from_utf8(u8string_view text);

        template <std::ranges::input_range Range>
            requires std::convertible_to<std::ranges::range_reference_t<Range>, char>
        [[nodiscard]] static UString from_utf8_range(Range &&range) {
            UString value;
            value.assign_range(std::forward<Range>(range));
            return value;
        }

        template <std::ranges::input_range Range>
            requires std::convertible_to<std::ranges::range_reference_t<Range>, char32_t>
        [[nodiscard]] static UString from_codepoints(Range &&range) {
            UString value;
            value.append_codepoints(std::forward<Range>(range));
            return value;
        }

        /// Build from a NUL-terminated C string, trusting the caller's terminator (the length is found with
        /// `strlen`). C makes NUL termination a caller obligation the compiler cannot check, so if `buffer`
        /// is not actually terminated this over-reads — prefer the bounded overload whenever the terminator
        /// is not under your control.
        [[nodiscard]] static UString from_c_str(const char *buffer);

        /// Safe, bounded ingestion: reads at most `max_bytes` bytes, stopping early at the first NUL if one
        /// occurs sooner. A buffer with no terminator inside `max_bytes` is taken as exactly `max_bytes`
        /// bytes rather than over-read, so this never leans on C's fragile NUL-termination invariant. Pass
        /// the buffer's real capacity as `max_bytes`.
        [[nodiscard]] static UString from_c_str(const char *buffer, usize max_bytes);

        /// Bounded ingestion that validates instead of throwing: returns `nullopt` for a null buffer or when
        /// the (bounded) bytes are not strict UTF-8. Same over-read guarantee as `from_c_str(buffer, max)`.
        [[nodiscard]] static optional<UString> try_from_c_str(const char *buffer, usize max_bytes);

        /// UTF-16 -> UTF-8, combining surrogate pairs. Throws `invalid_argument` on an unpaired or truncated
        /// surrogate, or on U+0000 (which would violate the no-embedded-NUL invariant).
        [[nodiscard]] static UString from_utf16(u16string_view text);

        /// UTF-32 -> UTF-8. Each unit is a Unicode scalar, so `append` validates it (rejecting surrogates,
        /// values above U+10FFFF, and U+0000).
        [[nodiscard]] static UString from_utf32(u32string_view text);

        /// Platform-wide (`wchar_t`) -> UTF-8, dispatched on the platform's `wchar_t` width: UTF-16 on
        /// Windows, UTF-32 on the Unix-likes. Each unit is widened without a reinterpreting cast.
        [[nodiscard]] static UString from_wstring(wstring_view text);

        [[nodiscard]] const char *data() const noexcept;

        /// Returns a NUL-terminated pointer safe to hand to C APIs: the no-embedded-NUL invariant guarantees
        /// the C string spans the whole value (`strlen(c_str()) == byte_size()`), so it cannot be silently
        /// truncated by a stray interior NUL.
        [[nodiscard]] const char *c_str() const noexcept;

        [[nodiscard]] string_view cpp_string_view() const noexcept;

        [[nodiscard]] string_view cpp_bytes() const noexcept;

        [[nodiscard]] ustr as_ustr() const & noexcept;
        [[nodiscard]] ustr as_ustr() const && = delete;

        [[nodiscard]] ustr slice() const & noexcept;
        [[nodiscard]] ustr slice() const && = delete;
        [[nodiscard]] ustr slice(usize scalar_start) const &;
        [[nodiscard]] ustr slice(usize scalar_start) const && = delete;
        [[nodiscard]] ustr slice(usize scalar_start, usize scalar_end) const &;
        [[nodiscard]] ustr slice(usize scalar_start, usize scalar_end) const && = delete;
        [[nodiscard]] ustr slice(USlice range) const &;
        [[nodiscard]] ustr slice(USlice range) const && = delete;
        [[nodiscard]] UString slice(USlicePattern pattern) const;

        [[nodiscard]] operator ustr() const & noexcept;
        [[nodiscard]] operator ustr() const && = delete;

        [[nodiscard]] string cpp_string() const;

        /// Checked ASCII downcast. Allocation can still throw in the ordinary std::string way; the
        /// expected error reports an encoding failure, not an allocator failure.
        [[nodiscard]] expected<string, TextConversionError> to_std_string() const;

        /// Caller promises this value is ASCII. A violated encoding contract terminates in every
        /// build configuration instead of returning mojibake.
        [[nodiscard]] string to_std_string_unchecked() const;

        /// Borrowed `char8_t` view over the same bytes — UTF-8-typed interop without a copy. The bytes are
        /// identical to `cpp_string_view()`; only the element type differs.
        [[nodiscard]] u8string_view cpp_u8string_view() const noexcept;

        /// Owned `std::u8string` copy.
        [[nodiscard]] u8string cpp_u8string() const;

        /// Owned UTF-16 / UTF-32 / platform-wide copies, for handing text to APIs that speak those units.
        [[nodiscard]] u16string cpp_u16string() const;
        [[nodiscard]] u32string cpp_u32string() const;
        [[nodiscard]] wstring cpp_wstring() const;

        [[nodiscard]] operator string_view() const noexcept;

        [[nodiscard]] bool empty() const noexcept;

        [[nodiscard]] bool is_ascii() const noexcept;

        [[nodiscard]] usize size() const noexcept;

        [[nodiscard]] usize length() const noexcept;

        [[nodiscard]] usize scalar_size() const noexcept;

        [[nodiscard]] usize codepoint_size() const noexcept;

        [[nodiscard]] usize byte_size() const noexcept;

        [[nodiscard]] usize size_bytes() const noexcept;

        [[nodiscard]] usize capacity() const noexcept;

        [[nodiscard]] bool is_small() const noexcept;

        [[nodiscard]] CodepointIterator begin() const noexcept;

        [[nodiscard]] CodepointIterator end() const noexcept;

        [[nodiscard]] CodepointView codepoints() const noexcept;

        [[nodiscard]] const char *byte_begin() const noexcept;

        [[nodiscard]] const char *byte_end() const noexcept;

        [[nodiscard]] char32_t front() const;

        [[nodiscard]] char32_t back() const;

        [[nodiscard]] char32_t at(usize scalar_index) const;

        [[nodiscard]] char32_t operator[](usize scalar_index) const noexcept;

        [[nodiscard]] ustr operator[](USlice range) const &;
        [[nodiscard]] ustr operator[](USlice range) const && = delete;
        [[nodiscard]] UString operator[](USlicePattern pattern) const;

        UString &assign(string_view text);

        UString &assign(u8string_view text);

        UString &assign(const ustr &text);

        UString &assign(const char *text);

        template <std::ranges::input_range Range>
            requires std::convertible_to<std::ranges::range_reference_t<Range>, char>
        UString &assign_range(Range &&range) {
            UString replacement;
            replacement.append_range(std::forward<Range>(range));
            swap(replacement);
            return *this;
        }

        template <std::ranges::input_range Range>
            requires std::convertible_to<std::ranges::range_reference_t<Range>, char32_t>
        UString &assign_codepoints(Range &&range) {
            UString replacement;
            replacement.append_codepoints(std::forward<Range>(range));
            swap(replacement);
            return *this;
        }

        void clear() noexcept;

        void reserve(usize requested_capacity);

        void shrink_to_fit();

        UString &append(const UString &text);

        UString &append(const ustr &text);

        UString &append(string_view text);

        UString &append(u8string_view text);

        UString &append(const char *text);

        UString &append(char32_t scalar);

        template <std::ranges::input_range Range>
            requires std::convertible_to<std::ranges::range_reference_t<Range>, char>
        UString &append_range(Range &&range) {
            string pending;
            if constexpr (std::ranges::sized_range<Range>) {
                pending.reserve(static_cast<size_t>(std::ranges::size(range)));
            }

            for (auto &&value : range) {
                pending.push_back(static_cast<char>(value));
            }

            return append(string_view{pending});
        }

        template <std::ranges::input_range Range>
            requires std::convertible_to<std::ranges::range_reference_t<Range>, char32_t>
        UString &append_codepoints(Range &&range) {
            UString pending;
            for (auto &&value : range) {
                pending.append(static_cast<char32_t>(value));
            }
            return append(pending);
        }

        UString &push_back(char32_t scalar);

        void pop_back();

        UString &operator+=(const UString &text);

        UString &operator+=(const ustr &text);

        UString &operator+=(string_view text);

        UString &operator+=(const char *text);

        UString &operator+=(char32_t scalar);

        UString &insert(usize scalar_index, const UString &text);

        UString &insert(usize scalar_index, const ustr &text);

        UString &insert(usize scalar_index, string_view text);

        UString &insert(usize scalar_index, u8string_view text);

        UString &insert(usize scalar_index, char32_t scalar);

        UString &erase(usize scalar_index = 0, usize scalar_count = npos);

        UString &replace(usize scalar_index, usize scalar_count, const UString &replacement);

        UString &replace(usize scalar_index, usize scalar_count, const ustr &replacement);

        UString &replace(usize scalar_index, usize scalar_count, string_view replacement);

        UString &replace(usize scalar_index, usize scalar_count, u8string_view replacement);

        UString &replace_all(const ustr &needle, const ustr &replacement);

        UString &replace_all(const UString &needle, const UString &replacement);

        UString &resize(usize requested_scalar_size, char32_t fill = U' ');

        [[nodiscard]] UString substr(usize scalar_index = 0, usize scalar_count = npos) const;

        [[nodiscard]] usize find(const UString &needle, usize scalar_position = 0) const;

        [[nodiscard]] usize find(const ustr &needle, usize scalar_position = 0) const;

        [[nodiscard]] usize find(string_view needle, usize scalar_position = 0) const;

        [[nodiscard]] usize find(u8string_view needle, usize scalar_position = 0) const;

        [[nodiscard]] usize rfind(const UString &needle, usize scalar_position = npos) const;

        [[nodiscard]] usize rfind(const ustr &needle, usize scalar_position = npos) const;

        [[nodiscard]] usize rfind(string_view needle, usize scalar_position = npos) const;

        [[nodiscard]] usize rfind(u8string_view needle, usize scalar_position = npos) const;

        [[nodiscard]] bool contains(const UString &needle) const noexcept;

        [[nodiscard]] bool contains(const ustr &needle) const noexcept;

        [[nodiscard]] bool contains(string_view needle) const;

        [[nodiscard]] bool contains(u8string_view needle) const;

        [[nodiscard]] bool contains(char32_t scalar) const;

        [[nodiscard]] bool starts_with(const UString &prefix) const noexcept;

        [[nodiscard]] bool starts_with(const ustr &prefix) const noexcept;

        [[nodiscard]] bool starts_with(string_view prefix) const;

        [[nodiscard]] bool starts_with(u8string_view prefix) const;

        [[nodiscard]] bool ends_with(const UString &suffix) const noexcept;

        [[nodiscard]] bool ends_with(const ustr &suffix) const noexcept;

        [[nodiscard]] bool ends_with(string_view suffix) const;

        [[nodiscard]] bool ends_with(u8string_view suffix) const;

        [[nodiscard]] usize find_first_of(const UString &scalars, usize scalar_position = 0) const;

        [[nodiscard]] usize find_first_of(string_view scalars, usize scalar_position = 0) const;

        [[nodiscard]] usize find_first_not_of(const UString &scalars, usize scalar_position = 0) const;

        [[nodiscard]] usize find_first_not_of(string_view scalars, usize scalar_position = 0) const;

        [[nodiscard]] usize find_last_of(const UString &scalars, usize scalar_position = npos) const;

        [[nodiscard]] usize find_last_of(string_view scalars, usize scalar_position = npos) const;

        [[nodiscard]] usize find_last_not_of(const UString &scalars, usize scalar_position = npos) const;

        [[nodiscard]] usize find_last_not_of(string_view scalars, usize scalar_position = npos) const;

        [[nodiscard]] bool is_codepoint_boundary(usize byte_index) const noexcept;

        [[nodiscard]] usize byte_index_of(usize scalar_index) const;

        [[nodiscard]] usize scalar_index_of_byte(usize byte_index) const;

        [[nodiscard]] int compare(const UString &other) const noexcept;

        [[nodiscard]] int compare(const ustr &other) const noexcept;

        [[nodiscard]] int compare(string_view other) const;

        void swap(UString &other) noexcept;

        friend bool operator==(const UString &lhs, const UString &rhs) noexcept;

        friend strong_ordering operator<=>(const UString &lhs, const UString &rhs) noexcept;

        friend UString operator+(UString lhs, const UString &rhs);

        friend UString operator+(UString lhs, string_view rhs);

        friend UString operator+(string_view lhs, const UString &rhs);

        friend UString operator+(UString lhs, char32_t rhs);

      private:
        struct ValidatedInput {};

        static constexpr u32 max_unicode_scalar = 0x10FFFF;

        UString(string_view text, usize scalar_count, ValidatedInput);

        [[nodiscard]] static constexpr bool is_continuation_byte(unsigned char byte) noexcept {
            return (byte & 0xC0u) == 0x80u;
        }

        [[nodiscard]] static constexpr bool is_surrogate(u32 scalar) noexcept {
            return scalar >= 0xD800 && scalar <= 0xDFFF;
        }

        [[nodiscard]] static constexpr UStringValidation failure(UStringValidationError error, usize byte_index, usize scalar_count) noexcept {
            return UStringValidation{.valid = false, .error = error, .byte_index = byte_index, .scalar_count = scalar_count};
        }

        [[nodiscard]] static string_view as_char_view(u8string_view text) noexcept;

        static UStringValidation validate_or_throw(string_view text);

        static void validate_scalar_or_throw(char32_t scalar);

        [[nodiscard]] static usize encode_scalar_or_throw(char32_t scalar, char *buffer);

        [[nodiscard]] static usize encoded_length_from_lead(unsigned char lead) noexcept;

        [[nodiscard]] static usize encoded_length_unchecked(const char *text) noexcept;

        [[nodiscard]] static char32_t decode_unchecked(const char *text) noexcept;

        [[nodiscard]] static const char *previous_codepoint(const char *begin, const char *cursor) noexcept;

        [[nodiscard]] static char *allocate_buffer(usize capacity);

        [[nodiscard]] const char *storage_data() const noexcept;

        [[nodiscard]] char *mutable_data() noexcept;

        [[nodiscard]] bool overlaps_storage(string_view text) const noexcept;

        void move_from(UString &&other) noexcept;

        void assign_validated_unaliased(string_view text, usize scalar_count);

        void ensure_capacity(usize requested_capacity);

        [[nodiscard]] usize checked_total_byte_size(usize additional_bytes) const;

        UString &append_validated(string_view text, usize scalar_count);

        void append_validated_unaliased(string_view text, usize scalar_count);

        void append_encoded_scalar(const char *encoded, usize encoded_size);

        UString &insert_validated(usize scalar_index, string_view text, usize scalar_count);

        UString &replace_validated(usize scalar_index, usize scalar_count, string_view replacement, usize replacement_scalar_count);

        [[nodiscard]] usize byte_index_of_unchecked(usize scalar_index) const noexcept;

        [[nodiscard]] usize scalar_index_of_byte_unchecked(usize byte_index) const noexcept;

        [[nodiscard]] CodepointIterator iterator_at_unchecked(usize scalar_index) const noexcept;

        [[nodiscard]] bool contains_scalar_unchecked(char32_t scalar) const noexcept;

        [[nodiscard]] usize find_validated(string_view needle, usize scalar_position) const;

        [[nodiscard]] usize rfind_validated(string_view needle, usize scalar_position) const;

        usize byte_size_ = 0;
        usize scalar_size_ = 0;
        usize capacity_ = sso_capacity;
        char *heap_ = nullptr;
        array<char, sso_capacity + 1> small_{};
    };

    void swap(UString &lhs, UString &rhs) noexcept;

    /// Borrowed UTF-8 string slice: the Rust `&str` counterpart to owned `UString`.
    ///
    /// `ustr` owns no bytes, so it has `string_view`-like lifetime rules. It is intentionally non-copyable
    /// and non-assignable to make engine APIs spell borrowed UTF-8 as `const ustr&`, which mirrors Rust's
    /// convention of passing `str` behind a reference while still allowing literals and temporary slices to
    /// bind to parameters.
    class ustr final {
      public:
        using value_type = char32_t;
        using size_type = usize;
        using difference_type = isize;
        using CodepointIterator = UString::CodepointIterator;
        using CodepointView = UString::CodepointView;

        static constexpr usize npos = UString::npos;

        ustr() noexcept = default;
        ustr(std::nullptr_t) = delete;
        ustr(const ustr &) = delete;
        ustr(ustr &&) = delete;
        ustr &operator=(const ustr &) = delete;
        ustr &operator=(ustr &&) = delete;

        ustr(const char *text);

        ustr(const char *text, usize byte_count);

        ustr(string_view text);

        ustr(const string &text);

        ustr(string &&) = delete;
        ustr(const string &&) = delete;

        ustr(u8string_view text);

        explicit ustr(const UString &text) noexcept;

        ustr(UString &&) = delete;
        ustr(const UString &&) = delete;

        [[nodiscard]] static UStringValidation validate_utf8(string_view text) noexcept;

        [[nodiscard]] static bool is_valid_utf8(string_view text) noexcept;

        /// Safe, bounded borrow of a C buffer: the resulting slice spans at most `max_bytes` bytes, stopping
        /// at the first NUL if one occurs sooner, and never over-reads past `max_bytes` even if the buffer
        /// is not NUL-terminated. Like every `ustr`, it borrows — the buffer must outlive the slice. Throws
        /// `invalid_argument` if the bounded bytes are not strict UTF-8.
        [[nodiscard]] static ustr from_c_str(const char *buffer, usize max_bytes);

        [[nodiscard]] const char *data() const noexcept;

        /// A borrowed slice may point into the middle of a larger buffer, so it is not guaranteed to be
        /// NUL-terminated. `ustr` therefore has no `c_str()`: materialize an owned value when a C string is
        /// required (`UString{slice}.c_str()`), which restores the terminator and the no-interior-NUL
        /// guarantee. Deleted (rather than merely absent) so a mistaken call is a clear compile error.
        const char *c_str() const = delete;

        [[nodiscard]] string_view cpp_string_view() const noexcept;

        [[nodiscard]] string_view cpp_bytes() const noexcept;

        [[nodiscard]] string cpp_string() const;

        [[nodiscard]] expected<string, TextConversionError> to_std_string() const;

        [[nodiscard]] string to_std_string_unchecked() const;

        [[nodiscard]] UString to_owned() const;

        /// Borrowed `char8_t` view over the same bytes (no copy); owned `std::u8string` copy.
        [[nodiscard]] u8string_view cpp_u8string_view() const noexcept;

        [[nodiscard]] u8string cpp_u8string() const;

        /// Owned UTF-16 / UTF-32 / platform-wide copies.
        [[nodiscard]] u16string cpp_u16string() const;
        [[nodiscard]] u32string cpp_u32string() const;
        [[nodiscard]] wstring cpp_wstring() const;

        [[nodiscard]] operator string_view() const noexcept;

        [[nodiscard]] bool empty() const noexcept;

        [[nodiscard]] bool is_ascii() const noexcept;

        [[nodiscard]] usize size() const noexcept;

        [[nodiscard]] usize length() const noexcept;

        [[nodiscard]] usize scalar_size() const noexcept;

        [[nodiscard]] usize codepoint_size() const noexcept;

        [[nodiscard]] usize byte_size() const noexcept;

        [[nodiscard]] usize size_bytes() const noexcept;

        [[nodiscard]] CodepointIterator begin() const noexcept;

        [[nodiscard]] CodepointIterator end() const noexcept;

        [[nodiscard]] CodepointView codepoints() const noexcept;

        [[nodiscard]] const char *byte_begin() const noexcept;

        [[nodiscard]] const char *byte_end() const noexcept;

        [[nodiscard]] char32_t front() const;

        [[nodiscard]] char32_t back() const;

        [[nodiscard]] char32_t at(usize scalar_index) const;

        [[nodiscard]] char32_t operator[](usize scalar_index) const noexcept;

        [[nodiscard]] ustr operator[](USlice range) const;

        [[nodiscard]] UString operator[](USlicePattern pattern) const;

        [[nodiscard]] ustr slice(usize scalar_start) const;

        [[nodiscard]] ustr slice(usize scalar_start, usize scalar_end) const;

        [[nodiscard]] ustr slice(USlice range) const;

        [[nodiscard]] UString slice(USlicePattern pattern) const;

        [[nodiscard]] ustr substr(usize scalar_index = 0, usize scalar_count = npos) const;

        [[nodiscard]] usize find(const ustr &needle, usize scalar_position = 0) const;

        [[nodiscard]] usize find(string_view needle, usize scalar_position = 0) const;

        [[nodiscard]] usize find(u8string_view needle, usize scalar_position = 0) const;

        [[nodiscard]] usize rfind(const ustr &needle, usize scalar_position = npos) const;

        [[nodiscard]] usize rfind(string_view needle, usize scalar_position = npos) const;

        [[nodiscard]] usize rfind(u8string_view needle, usize scalar_position = npos) const;

        [[nodiscard]] bool contains(const ustr &needle) const noexcept;

        [[nodiscard]] bool contains(string_view needle) const;

        [[nodiscard]] bool contains(u8string_view needle) const;

        [[nodiscard]] bool contains(char32_t scalar) const;

        [[nodiscard]] bool starts_with(const ustr &prefix) const noexcept;

        [[nodiscard]] bool starts_with(string_view prefix) const;

        [[nodiscard]] bool starts_with(u8string_view prefix) const;

        [[nodiscard]] bool ends_with(const ustr &suffix) const noexcept;

        [[nodiscard]] bool ends_with(string_view suffix) const;

        [[nodiscard]] bool ends_with(u8string_view suffix) const;

        [[nodiscard]] bool is_codepoint_boundary(usize byte_index) const noexcept;

        [[nodiscard]] usize byte_index_of(usize scalar_index) const;

        [[nodiscard]] usize scalar_index_of_byte(usize byte_index) const;

        [[nodiscard]] int compare(const ustr &other) const noexcept;

        [[nodiscard]] int compare(string_view other) const;

        friend bool operator==(const ustr &lhs, const ustr &rhs) noexcept;

        friend strong_ordering operator<=>(const ustr &lhs, const ustr &rhs) noexcept;

      private:
        friend class UString;

        struct ValidatedInput {};

        constexpr ustr(string_view text, usize scalar_count, ValidatedInput) noexcept
            : bytes_(text), scalar_size_(scalar_count) {
        }

        void assign_validated(string_view text);

        [[nodiscard]] static string_view as_char_view(u8string_view text) noexcept;

        static UStringValidation validate_or_throw(string_view text);

        [[nodiscard]] static constexpr bool is_continuation_byte(unsigned char byte) noexcept {
            return (byte & 0xC0u) == 0x80u;
        }

        static void validate_scalar_or_throw(char32_t scalar);

        [[nodiscard]] static usize encoded_length_from_lead(unsigned char lead) noexcept;

        [[nodiscard]] static usize encoded_length_unchecked(const char *text) noexcept;

        [[nodiscard]] static char32_t decode_unchecked(const char *text) noexcept;

        [[nodiscard]] static const char *previous_codepoint(const char *begin, const char *cursor) noexcept;

        [[nodiscard]] usize byte_index_of_unchecked(usize scalar_index) const noexcept;

        [[nodiscard]] usize scalar_index_of_byte_unchecked(usize byte_index) const noexcept;

        [[nodiscard]] bool contains_scalar_unchecked(char32_t scalar) const noexcept;

        string_view bytes_{};
        usize scalar_size_ = 0;
    };

    namespace Detail {

        /// UTF-8 -> UTF-16 / UTF-32, shared by the `cpp_u16string()`/`cpp_u32string()` members of both string
        /// types. Defined here (a reopened `Detail`) because they take `UString::CodepointView`, which is
        /// only complete after the class body. The source scalars are already validated, so no re-checking
        /// is needed: UTF-32 is a straight copy, and UTF-16 emits a surrogate pair for astral scalars.
        [[nodiscard]] u32string to_utf32(UString::CodepointView codepoints);

        [[nodiscard]] u16string to_utf16(UString::CodepointView codepoints);

    } // namespace Detail


    [[nodiscard]] bool operator==(const UString &lhs, const ustr &rhs) noexcept;

    [[nodiscard]] bool operator==(const ustr &lhs, const UString &rhs) noexcept;

    [[nodiscard]] strong_ordering operator<=>(const UString &lhs, const ustr &rhs) noexcept;

    [[nodiscard]] strong_ordering operator<=>(const ustr &lhs, const UString &rhs) noexcept;

    [[nodiscard]] UString operator+(UString lhs, const ustr &rhs);

    [[nodiscard]] UString operator+(const ustr &lhs, const UString &rhs);

    std::ostream &operator<<(std::ostream &os, const UString &value);

    std::ostream &operator<<(std::ostream &os, const ustr &value);

    std::ostream &operator<<(std::ostream &os, USlice value);

    std::ostream &operator<<(std::ostream &os, USlicePattern value);

    std::ostream &operator<<(std::ostream &os, UStringValidationError value);

    std::ostream &operator<<(std::ostream &os, TextConversionError value);

    std::ostream &operator<<(std::ostream &os, const UStringValidation &value);

} // namespace SFT::Foundation

[[nodiscard]] SFT::Foundation::ustr operator""_ustr(const char *text, size_t byte_count);

[[nodiscard]] SFT::Foundation::ustr operator""_ustr(const char8_t *text, size_t byte_count);

namespace SFT::Foundation {

    namespace Literals {
        using ::operator""_ustr;
    } // namespace Literals

} // namespace SFT::Foundation

namespace SFT {

    using USlice [[maybe_unused]] = Foundation::USlice;
    using USlicePattern [[maybe_unused]] = Foundation::USlicePattern;
    using TextConversionError [[maybe_unused]] = Foundation::TextConversionError;
    using ustr [[maybe_unused]] = Foundation::ustr;
    using UString [[maybe_unused]] = Foundation::UString;
    using Foundation::slice_from;
    using Foundation::uslice;

} // namespace SFT

/// `std::formatter` specializations for every own-type this partition exposes. Each delegates to the
/// `string_view` formatter so the types work with `std::format`/`std::print` (including format specs like
/// `{:>10}`); the `format` member is templated on the context so the types also satisfy `std::formattable`
/// (and thus `Displayable`), which probes with a different context than `std::format` itself uses. They are
/// intentionally left non-exported: explicit specializations are found by reachability rather than name
/// lookup, so importing the module makes them usable without polluting the exported name set.
template <>
struct std::formatter<SFT::Foundation::UString> : std::formatter<std::string_view> {
    auto format(const SFT::Foundation::UString &value, auto &ctx) const {
        return std::formatter<std::string_view>::format(value.cpp_string_view(), ctx);
    }
};

template <>
struct std::formatter<SFT::Foundation::ustr> : std::formatter<std::string_view> {
    auto format(const SFT::Foundation::ustr &value, auto &ctx) const {
        return std::formatter<std::string_view>::format(value.cpp_string_view(), ctx);
    }
};

template <>
struct std::formatter<SFT::Foundation::USlice> : std::formatter<std::string_view> {
    auto format(SFT::Foundation::USlice value, auto &ctx) const {
        return std::formatter<std::string_view>::format(SFT::Foundation::Detail::display_string(value), ctx);
    }
};

template <>
struct std::formatter<SFT::Foundation::USlicePattern> : std::formatter<std::string_view> {
    auto format(SFT::Foundation::USlicePattern value, auto &ctx) const {
        return std::formatter<std::string_view>::format(SFT::Foundation::Detail::display_string(value), ctx);
    }
};

template <>
struct std::formatter<SFT::Foundation::UStringValidationError> : std::formatter<std::string_view> {
    auto format(SFT::Foundation::UStringValidationError value, auto &ctx) const {
        return std::formatter<std::string_view>::format(SFT::Foundation::to_string(value), ctx);
    }
};

template <>
struct std::formatter<SFT::Foundation::TextConversionError> : std::formatter<std::string_view> {
    auto format(SFT::Foundation::TextConversionError value, auto &ctx) const {
        return std::formatter<std::string_view>::format(SFT::Foundation::to_string(value), ctx);
    }
};

template <>
struct std::formatter<SFT::Foundation::UStringValidation> : std::formatter<std::string_view> {
    auto format(const SFT::Foundation::UStringValidation &value, auto &ctx) const {
        return std::formatter<std::string_view>::format(SFT::Foundation::Detail::display_string(value), ctx);
    }
};

/// `fmt::formatter` mirrors of the above, so the same types format through {fmt}/spdlog. Each delegates to
/// fmt's `string_view` formatter (inheriting its `parse`, so format specs still work). Kept in lockstep with
/// the `std::formatter` set — `Displayable` requires both, so a type is never printable through one path but
/// not the other.
template <>
struct fmt::formatter<SFT::Foundation::UString> : fmt::formatter<std::string_view> {
    fmt::format_context::iterator format(const SFT::Foundation::UString &value, fmt::format_context &ctx) const;
};

template <>
struct fmt::formatter<SFT::Foundation::ustr> : fmt::formatter<std::string_view> {
    fmt::format_context::iterator format(const SFT::Foundation::ustr &value, fmt::format_context &ctx) const;
};

template <>
struct fmt::formatter<SFT::Foundation::USlice> : fmt::formatter<std::string_view> {
    fmt::format_context::iterator format(SFT::Foundation::USlice value, fmt::format_context &ctx) const;
};

template <>
struct fmt::formatter<SFT::Foundation::USlicePattern> : fmt::formatter<std::string_view> {
    fmt::format_context::iterator format(SFT::Foundation::USlicePattern value, fmt::format_context &ctx) const;
};

template <>
struct fmt::formatter<SFT::Foundation::UStringValidationError> : fmt::formatter<std::string_view> {
    fmt::format_context::iterator format(SFT::Foundation::UStringValidationError value, fmt::format_context &ctx) const;
};

template <>
struct fmt::formatter<SFT::Foundation::TextConversionError> : fmt::formatter<std::string_view> {
    fmt::format_context::iterator format(SFT::Foundation::TextConversionError value, fmt::format_context &ctx) const;
};

template <>
struct fmt::formatter<SFT::Foundation::UStringValidation> : fmt::formatter<std::string_view> {
    fmt::format_context::iterator format(const SFT::Foundation::UStringValidation &value, fmt::format_context &ctx) const;
};

/// `std::hash` specializations so both string types are usable as keys in `std::unordered_map`/`set`. Both
/// hash their raw UTF-8 bytes, so an equal `UString` and `ustr` hash identically (matching their `==`).
/// Non-exported for the same reachability reason as the formatters above.
template <>
struct std::hash<SFT::Foundation::UString> {
    [[nodiscard]] std::size_t operator()(const SFT::Foundation::UString &value) const noexcept;
};

template <>
struct std::hash<SFT::Foundation::ustr> {
    [[nodiscard]] std::size_t operator()(const SFT::Foundation::ustr &value) const noexcept;
};

/// Every own-type this partition exposes is `Displayable` (streams with `<<` and formats with
/// `std::format`). Checked here, after the formatter specializations are in scope, so a regression in
/// either facility is a hard compile error rather than a silent loss of printability.
static_assert(SFT::Foundation::Displayable<SFT::Foundation::UString>);
static_assert(SFT::Foundation::Displayable<SFT::Foundation::ustr>);
static_assert(SFT::Foundation::Displayable<SFT::Foundation::USlice>);
static_assert(SFT::Foundation::Displayable<SFT::Foundation::USlicePattern>);
static_assert(SFT::Foundation::Displayable<SFT::Foundation::UStringValidationError>);
static_assert(SFT::Foundation::Displayable<SFT::Foundation::UStringValidation>);
static_assert(SFT::Foundation::Displayable<SFT::Foundation::TextConversionError>);

/// And both string types are `Hashable` (usable as hash-map keys), verified after the `std::hash`
/// specializations are in scope.
static_assert(SFT::Foundation::Hashable<SFT::Foundation::UString>);
static_assert(SFT::Foundation::Hashable<SFT::Foundation::ustr>);
static_assert(noexcept(SFT::Foundation::UString{std::declval<const std::string &>()}));
