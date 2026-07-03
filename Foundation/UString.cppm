module;

#include <algorithm>
#include <array>
#include <cassert>
#include <compare>
#include <concepts>
#include <cstring>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <ostream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

export module Sturdy.Foundation:UString;

import :Concepts;
import :Types;

using std::array;
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
using std::u8string_view;

export namespace SFT::Foundation {

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

        [[nodiscard]] inline ResolvedSlice resolve_slice(USlice slice, usize scalar_size, string_view owner) {
            const usize start = slice.start();
            const usize end = slice.end_or(scalar_size);

            if (start > scalar_size) {
                throw out_of_range{format("{} slice start {} is out of range for size {}.", owner, start, scalar_size)};
            }
            if (end > scalar_size) {
                throw out_of_range{format("{} slice end {} is out of range for size {}.", owner, end, scalar_size)};
            }
            if (end < start) {
                throw out_of_range{format("{} slice end {} is before start {}.", owner, end, start)};
            }

            return ResolvedSlice{.start = start, .end = end};
        }

        [[nodiscard]] inline ResolvedSlicePattern resolve_slice(USlicePattern slice, usize scalar_size, string_view owner) {
            const ResolvedSlice range = resolve_slice(USlice{slice.start(), slice.end_or(USlice::npos)}, scalar_size, owner);
            const usize grouping = slice.grouping();
            if (grouping == 0) {
                throw invalid_argument{format("{} slice grouping must be greater than zero.", owner)};
            }

            return ResolvedSlicePattern{.range = range, .spread = slice.spread(), .grouping = grouping};
        }

        // Human-readable renderings shared by the `operator<<` overloads and the `std::formatter`
        // specializations below, so both spell a value the same way.
        [[nodiscard]] inline string display_string(USlice slice) {
            if (slice.has_end()) {
                return format("USlice({}..{})", slice.start(), slice.end_or(0));
            }
            return format("USlice({}..)", slice.start());
        }

        [[nodiscard]] inline string display_string(USlicePattern pattern) {
            if (pattern.has_end()) {
                return format("USlicePattern({}..{}, group={}, spread={})", pattern.start(), pattern.end_or(0), pattern.grouping(), pattern.spread());
            }
            return format("USlicePattern({}.., group={}, spread={})", pattern.start(), pattern.grouping(), pattern.spread());
        }

        [[nodiscard]] inline string display_string(const UStringValidation &validation) {
            if (validation.valid) {
                return format("UStringValidation(valid, {} scalars)", validation.scalar_count);
            }
            return format("UStringValidation(invalid: {} at byte {}, {} scalars)", to_string(validation.error), validation.byte_index, validation.scalar_count);
        }

    } // namespace Detail

    // `UString` is a UTF-8 owning string with hard invariants:
    // - every stored byte sequence is strict UTF-8 (no overlong encodings, surrogates, or out-of-range scalars),
    // - interior NUL bytes are rejected so `c_str()` cannot be truncated by C APIs,
    // - byte size and Unicode scalar count are tracked and updated together,
    // - mutation APIs validate replacement text before touching the current storage,
    // - byte-boundary-sensitive operations are exposed as scalar-indexed operations by default.
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

            [[nodiscard]] char32_t operator*() const noexcept {
                assert(current_ != nullptr);
                return UString::decode_unchecked(current_);
            }

            CodepointIterator &operator++() noexcept {
                assert(current_ != nullptr);
                current_ += UString::encoded_length_unchecked(current_);
                return *this;
            }

            CodepointIterator operator++(int) noexcept {
                CodepointIterator copy = *this;
                ++(*this);
                return copy;
            }

            CodepointIterator &operator--() noexcept {
                assert(begin_ != nullptr);
                assert(current_ != nullptr);
                assert(current_ > begin_);
                do {
                    --current_;
                } while (current_ > begin_ && UString::is_continuation_byte(static_cast<unsigned char>(*current_)));
                return *this;
            }

            CodepointIterator operator--(int) noexcept {
                CodepointIterator copy = *this;
                --(*this);
                return copy;
            }

            [[nodiscard]] friend bool operator==(CodepointIterator lhs, CodepointIterator rhs) noexcept {
                return lhs.begin_ == rhs.begin_ && lhs.current_ == rhs.current_;
            }

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

        UString() noexcept {
            small_[0] = '\0';
        }

        UString(std::nullptr_t) = delete;

        UString(const char *text) {
            if (text == nullptr) {
                throw invalid_argument{"UString cannot be constructed from a null pointer."};
            }
            assign(string_view{text});
        }

        UString(const char *text, usize byte_count) {
            if (text == nullptr && byte_count != 0) {
                throw invalid_argument{"UString cannot be constructed from a null pointer and non-zero size."};
            }
            assign(string_view{text == nullptr ? "" : text, byte_count});
        }

        UString(string_view text) {
            assign(text);
        }

        UString(const string &text) {
            assign(string_view{text});
        }

        UString(u8string_view text) {
            assign(as_char_view(text));
        }

        UString(const ustr &text);

        UString(const UString &other) {
            assign_validated_unaliased(other.view(), other.scalar_size_);
        }

        UString(UString &&other) noexcept {
            move_from(std::move(other));
        }

        ~UString() noexcept {
            delete[] heap_;
        }

        UString &operator=(UString other) noexcept {
            swap(other);
            return *this;
        }

        UString &operator=(string_view text) {
            return assign(text);
        }

        UString &operator=(const char *text) {
            if (text == nullptr) {
                throw invalid_argument{"UString cannot be assigned from a null pointer."};
            }
            return assign(string_view{text});
        }

        UString &operator=(u8string_view text) {
            return assign(as_char_view(text));
        }

        UString &operator=(const ustr &text);

        [[nodiscard]] static constexpr usize max_size() noexcept {
            return std::numeric_limits<usize>::max() / 2;
        }

        [[nodiscard]] static UStringValidation validate_utf8(string_view text) noexcept {
            if (text.data() == nullptr && !text.empty()) {
                return failure(UStringValidationError::NullPointer, 0, 0);
            }

            usize scalar_count = 0;

            for (usize index = 0; index < text.size();) {
                const auto lead = static_cast<unsigned char>(text[index]);

                if (lead == 0) {
                    return failure(UStringValidationError::EmbeddedNul, index, scalar_count);
                }

                if (lead <= 0x7F) {
                    ++index;
                    ++scalar_count;
                    continue;
                }

                usize length = 0;
                u32 scalar = 0;
                u32 minimum_scalar = 0;

                if (lead >= 0xC2 && lead <= 0xDF) {
                    length = 2;
                    scalar = lead & 0x1Fu;
                    minimum_scalar = 0x80;
                } else if (lead >= 0xE0 && lead <= 0xEF) {
                    length = 3;
                    scalar = lead & 0x0Fu;
                    minimum_scalar = 0x800;
                } else if (lead >= 0xF0 && lead <= 0xF4) {
                    length = 4;
                    scalar = lead & 0x07u;
                    minimum_scalar = 0x10000;
                } else if (is_continuation_byte(lead)) {
                    return failure(UStringValidationError::UnexpectedContinuationByte, index, scalar_count);
                } else {
                    return failure(UStringValidationError::InvalidLeadingByte, index, scalar_count);
                }

                if (text.size() - index < length) {
                    return failure(UStringValidationError::TruncatedSequence, index, scalar_count);
                }

                for (usize offset = 1; offset < length; ++offset) {
                    const auto continuation = static_cast<unsigned char>(text[index + offset]);
                    if (!is_continuation_byte(continuation)) {
                        return failure(UStringValidationError::MissingContinuationByte, index + offset, scalar_count);
                    }
                    scalar = (scalar << 6u) | (continuation & 0x3Fu);
                }

                if (scalar < minimum_scalar) {
                    return failure(UStringValidationError::OverlongEncoding, index, scalar_count);
                }
                if (is_surrogate(scalar)) {
                    return failure(UStringValidationError::SurrogateCodePoint, index, scalar_count);
                }
                if (scalar > max_unicode_scalar) {
                    return failure(UStringValidationError::CodePointTooLarge, index, scalar_count);
                }

                index += length;
                ++scalar_count;
            }

            return UStringValidation{.valid = true, .error = UStringValidationError::None, .byte_index = text.size(), .scalar_count = scalar_count};
        }

        [[nodiscard]] static bool is_valid_utf8(string_view text) noexcept {
            return static_cast<bool>(validate_utf8(text));
        }

        [[nodiscard]] static optional<UString> try_from_utf8(string_view text) {
            const UStringValidation validation = validate_utf8(text);
            if (!validation) {
                return nullopt;
            }
            return UString{text, validation.scalar_count, ValidatedInput{}};
        }

        [[nodiscard]] static optional<UString> try_from_utf8(u8string_view text) {
            return try_from_utf8(as_char_view(text));
        }

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

        [[nodiscard]] const char *data() const noexcept {
            return storage_data();
        }

        [[nodiscard]] const char *c_str() const noexcept {
            return storage_data();
        }

        [[nodiscard]] string_view view() const noexcept {
            return string_view{storage_data(), byte_size_};
        }

        [[nodiscard]] string_view bytes() const noexcept {
            return view();
        }

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

        [[nodiscard]] string str() const {
            return string{view()};
        }

        [[nodiscard]] operator string_view() const noexcept {
            return view();
        }

        [[nodiscard]] bool empty() const noexcept {
            return byte_size_ == 0;
        }

        [[nodiscard]] usize size() const noexcept {
            return scalar_size_;
        }

        [[nodiscard]] usize length() const noexcept {
            return scalar_size_;
        }

        [[nodiscard]] usize scalar_size() const noexcept {
            return scalar_size_;
        }

        [[nodiscard]] usize codepoint_size() const noexcept {
            return scalar_size_;
        }

        [[nodiscard]] usize byte_size() const noexcept {
            return byte_size_;
        }

        [[nodiscard]] usize size_bytes() const noexcept {
            return byte_size_;
        }

        [[nodiscard]] usize capacity() const noexcept {
            return capacity_;
        }

        [[nodiscard]] bool is_small() const noexcept {
            return heap_ == nullptr;
        }

        [[nodiscard]] CodepointIterator begin() const noexcept {
            return CodepointIterator{storage_data(), storage_data()};
        }

        [[nodiscard]] CodepointIterator end() const noexcept {
            return CodepointIterator{storage_data(), storage_data() + byte_size_};
        }

        [[nodiscard]] CodepointView codepoints() const noexcept {
            return CodepointView{storage_data(), byte_size_, scalar_size_};
        }

        [[nodiscard]] const char *byte_begin() const noexcept {
            return storage_data();
        }

        [[nodiscard]] const char *byte_end() const noexcept {
            return storage_data() + byte_size_;
        }

        [[nodiscard]] char32_t front() const {
            if (empty()) {
                throw out_of_range{"UString::front() called on an empty string."};
            }
            return decode_unchecked(storage_data());
        }

        [[nodiscard]] char32_t back() const {
            if (empty()) {
                throw out_of_range{"UString::back() called on an empty string."};
            }
            return decode_unchecked(previous_codepoint(storage_data(), storage_data() + byte_size_));
        }

        [[nodiscard]] char32_t at(usize scalar_index) const {
            if (scalar_index >= scalar_size_) {
                throw out_of_range{format("UString scalar index {} is out of range for size {}.", scalar_index, scalar_size_)};
            }
            return decode_unchecked(storage_data() + byte_index_of(scalar_index));
        }

        [[nodiscard]] char32_t operator[](usize scalar_index) const noexcept {
            assert(scalar_index < scalar_size_);
            return decode_unchecked(storage_data() + byte_index_of_unchecked(scalar_index));
        }

        [[nodiscard]] ustr operator[](USlice range) const &;
        [[nodiscard]] ustr operator[](USlice range) const && = delete;
        [[nodiscard]] UString operator[](USlicePattern pattern) const;

        UString &assign(string_view text) {
            const UStringValidation validation = validate_or_throw(text);
            if (overlaps_storage(text)) {
                UString replacement{text, validation.scalar_count, ValidatedInput{}};
                swap(replacement);
                return *this;
            }
            assign_validated_unaliased(text, validation.scalar_count);
            return *this;
        }

        UString &assign(u8string_view text) {
            return assign(as_char_view(text));
        }

        UString &assign(const ustr &text);

        UString &assign(const char *text) {
            if (text == nullptr) {
                throw invalid_argument{"UString cannot be assigned from a null pointer."};
            }
            return assign(string_view{text});
        }

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

        void clear() noexcept {
            byte_size_ = 0;
            scalar_size_ = 0;
            mutable_data()[0] = '\0';
        }

        void reserve(usize requested_capacity) {
            if (requested_capacity > max_size()) {
                throw length_error{"UString capacity request exceeds max_size()."};
            }
            ensure_capacity(requested_capacity);
        }

        void shrink_to_fit() {
            if (byte_size_ <= sso_capacity) {
                if (heap_ == nullptr) {
                    return;
                }

                array<char, sso_capacity + 1> replacement{};
                std::memcpy(replacement.data(), heap_, byte_size_ + 1);
                delete[] heap_;
                heap_ = nullptr;
                capacity_ = sso_capacity;
                small_ = replacement;
                return;
            }

            if (heap_ != nullptr && capacity_ == byte_size_) {
                return;
            }

            char *replacement = allocate_buffer(byte_size_);
            std::memcpy(replacement, storage_data(), byte_size_ + 1);
            delete[] heap_;
            heap_ = replacement;
            capacity_ = byte_size_;
        }

        UString &append(const UString &text) {
            return append_validated(text.view(), text.scalar_size_);
        }

        UString &append(const ustr &text);

        UString &append(string_view text) {
            const UStringValidation validation = validate_or_throw(text);
            return append_validated(text, validation.scalar_count);
        }

        UString &append(u8string_view text) {
            return append(as_char_view(text));
        }

        UString &append(const char *text) {
            if (text == nullptr) {
                throw invalid_argument{"UString cannot append a null pointer."};
            }
            return append(string_view{text});
        }

        UString &append(char32_t scalar) {
            char buffer[4]{};
            const usize encoded_size = encode_scalar_or_throw(scalar, buffer);
            append_encoded_scalar(buffer, encoded_size);
            return *this;
        }

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

        UString &push_back(char32_t scalar) {
            return append(scalar);
        }

        void pop_back() {
            if (empty()) {
                throw out_of_range{"UString::pop_back() called on an empty string."};
            }

            const char *last = previous_codepoint(storage_data(), storage_data() + byte_size_);
            byte_size_ = static_cast<usize>(last - storage_data());
            --scalar_size_;
            mutable_data()[byte_size_] = '\0';
        }

        UString &operator+=(const UString &text) {
            return append(text);
        }

        UString &operator+=(const ustr &text);

        UString &operator+=(string_view text) {
            return append(text);
        }

        UString &operator+=(const char *text) {
            return append(text);
        }

        UString &operator+=(char32_t scalar) {
            return append(scalar);
        }

        UString &insert(usize scalar_index, const UString &text) {
            return insert_validated(scalar_index, text.view(), text.scalar_size_);
        }

        UString &insert(usize scalar_index, const ustr &text);

        UString &insert(usize scalar_index, string_view text) {
            const UStringValidation validation = validate_or_throw(text);
            return insert_validated(scalar_index, text, validation.scalar_count);
        }

        UString &insert(usize scalar_index, u8string_view text) {
            return insert(scalar_index, as_char_view(text));
        }

        UString &insert(usize scalar_index, char32_t scalar) {
            char buffer[4]{};
            const usize encoded_size = encode_scalar_or_throw(scalar, buffer);
            return insert_validated(scalar_index, string_view{buffer, encoded_size}, 1);
        }

        UString &erase(usize scalar_index = 0, usize scalar_count = npos) {
            if (scalar_index > scalar_size_) {
                throw out_of_range{format("UString erase index {} is out of range for size {}.", scalar_index, scalar_size_)};
            }
            if (scalar_count == 0 || scalar_index == scalar_size_) {
                return *this;
            }

            const usize actual_scalar_count = scalar_count == npos ? scalar_size_ - scalar_index : std::min(scalar_count, scalar_size_ - scalar_index);
            const usize first_byte = byte_index_of_unchecked(scalar_index);
            const usize last_byte = byte_index_of_unchecked(scalar_index + actual_scalar_count);
            const usize removed_bytes = last_byte - first_byte;
            char *target = mutable_data();

            std::memmove(target + first_byte, target + last_byte, byte_size_ - last_byte + 1);
            byte_size_ -= removed_bytes;
            scalar_size_ -= actual_scalar_count;
            return *this;
        }

        UString &replace(usize scalar_index, usize scalar_count, const UString &replacement) {
            return replace_validated(scalar_index, scalar_count, replacement.view(), replacement.scalar_size_);
        }

        UString &replace(usize scalar_index, usize scalar_count, const ustr &replacement);

        UString &replace(usize scalar_index, usize scalar_count, string_view replacement) {
            const UStringValidation validation = validate_or_throw(replacement);
            return replace_validated(scalar_index, scalar_count, replacement, validation.scalar_count);
        }

        UString &replace(usize scalar_index, usize scalar_count, u8string_view replacement) {
            return replace(scalar_index, scalar_count, as_char_view(replacement));
        }

        UString &replace_all(const ustr &needle, const ustr &replacement);

        UString &replace_all(const UString &needle, const UString &replacement) {
            if (needle.empty()) {
                throw invalid_argument{"UString::replace_all() requires a non-empty needle."};
            }

            usize search_from = 0;
            while (true) {
                const usize found = find(needle, search_from);
                if (found == npos) {
                    break;
                }

                replace(found, needle.scalar_size_, replacement);
                search_from = found + replacement.scalar_size_;
            }
            return *this;
        }

        UString &resize(usize requested_scalar_size, char32_t fill = U' ') {
            if (requested_scalar_size < scalar_size_) {
                erase(requested_scalar_size);
                return *this;
            }

            while (scalar_size_ < requested_scalar_size) {
                append(fill);
            }
            return *this;
        }

        [[nodiscard]] UString substr(usize scalar_index = 0, usize scalar_count = npos) const {
            if (scalar_index > scalar_size_) {
                throw out_of_range{format("UString substr index {} is out of range for size {}.", scalar_index, scalar_size_)};
            }

            const usize actual_scalar_count = scalar_count == npos ? scalar_size_ - scalar_index : std::min(scalar_count, scalar_size_ - scalar_index);
            const usize first_byte = byte_index_of_unchecked(scalar_index);
            const usize last_byte = byte_index_of_unchecked(scalar_index + actual_scalar_count);
            return UString{string_view{storage_data() + first_byte, last_byte - first_byte}, actual_scalar_count, ValidatedInput{}};
        }

        [[nodiscard]] usize find(const UString &needle, usize scalar_position = 0) const {
            return find_validated(needle.view(), scalar_position);
        }

        [[nodiscard]] usize find(const ustr &needle, usize scalar_position = 0) const;

        [[nodiscard]] usize find(string_view needle, usize scalar_position = 0) const {
            validate_or_throw(needle);
            return find_validated(needle, scalar_position);
        }

        [[nodiscard]] usize find(u8string_view needle, usize scalar_position = 0) const {
            return find(as_char_view(needle), scalar_position);
        }

        [[nodiscard]] usize rfind(const UString &needle, usize scalar_position = npos) const {
            return rfind_validated(needle.view(), scalar_position);
        }

        [[nodiscard]] usize rfind(const ustr &needle, usize scalar_position = npos) const;

        [[nodiscard]] usize rfind(string_view needle, usize scalar_position = npos) const {
            validate_or_throw(needle);
            return rfind_validated(needle, scalar_position);
        }

        [[nodiscard]] usize rfind(u8string_view needle, usize scalar_position = npos) const {
            return rfind(as_char_view(needle), scalar_position);
        }

        [[nodiscard]] bool contains(const UString &needle) const noexcept {
            return view().find(needle.view()) != string_view::npos;
        }

        [[nodiscard]] bool contains(const ustr &needle) const noexcept;

        [[nodiscard]] bool contains(string_view needle) const {
            return find(needle) != npos;
        }

        [[nodiscard]] bool contains(u8string_view needle) const {
            return contains(as_char_view(needle));
        }

        [[nodiscard]] bool contains(char32_t scalar) const {
            validate_scalar_or_throw(scalar);
            return contains_scalar_unchecked(scalar);
        }

        [[nodiscard]] bool starts_with(const UString &prefix) const noexcept {
            return view().starts_with(prefix.view());
        }

        [[nodiscard]] bool starts_with(const ustr &prefix) const noexcept;

        [[nodiscard]] bool starts_with(string_view prefix) const {
            validate_or_throw(prefix);
            return view().starts_with(prefix);
        }

        [[nodiscard]] bool starts_with(u8string_view prefix) const {
            return starts_with(as_char_view(prefix));
        }

        [[nodiscard]] bool ends_with(const UString &suffix) const noexcept {
            return view().ends_with(suffix.view());
        }

        [[nodiscard]] bool ends_with(const ustr &suffix) const noexcept;

        [[nodiscard]] bool ends_with(string_view suffix) const {
            validate_or_throw(suffix);
            return view().ends_with(suffix);
        }

        [[nodiscard]] bool ends_with(u8string_view suffix) const {
            return ends_with(as_char_view(suffix));
        }

        [[nodiscard]] usize find_first_of(const UString &scalars, usize scalar_position = 0) const {
            if (scalar_position > scalar_size_) {
                throw out_of_range{"UString::find_first_of() start position is out of range."};
            }
            if (scalars.empty()) {
                return npos;
            }

            usize index = scalar_position;
            for (auto it = iterator_at_unchecked(scalar_position); it != end(); ++it, ++index) {
                if (scalars.contains_scalar_unchecked(*it)) {
                    return index;
                }
            }
            return npos;
        }

        [[nodiscard]] usize find_first_of(string_view scalars, usize scalar_position = 0) const {
            return find_first_of(UString{scalars}, scalar_position);
        }

        [[nodiscard]] usize find_first_not_of(const UString &scalars, usize scalar_position = 0) const {
            if (scalar_position > scalar_size_) {
                throw out_of_range{"UString::find_first_not_of() start position is out of range."};
            }

            usize index = scalar_position;
            for (auto it = iterator_at_unchecked(scalar_position); it != end(); ++it, ++index) {
                if (!scalars.contains_scalar_unchecked(*it)) {
                    return index;
                }
            }
            return npos;
        }

        [[nodiscard]] usize find_first_not_of(string_view scalars, usize scalar_position = 0) const {
            return find_first_not_of(UString{scalars}, scalar_position);
        }

        [[nodiscard]] usize find_last_of(const UString &scalars, usize scalar_position = npos) const {
            if (empty() || scalars.empty()) {
                return npos;
            }

            usize index = scalar_position == npos || scalar_position >= scalar_size_ ? scalar_size_ - 1 : scalar_position;
            const char *cursor = storage_data() + byte_index_of_unchecked(index + 1);
            while (true) {
                cursor = previous_codepoint(storage_data(), cursor);
                if (scalars.contains_scalar_unchecked(decode_unchecked(cursor))) {
                    return index;
                }
                if (index == 0) {
                    break;
                }
                --index;
            }
            return npos;
        }

        [[nodiscard]] usize find_last_of(string_view scalars, usize scalar_position = npos) const {
            return find_last_of(UString{scalars}, scalar_position);
        }

        [[nodiscard]] usize find_last_not_of(const UString &scalars, usize scalar_position = npos) const {
            if (empty()) {
                return npos;
            }

            usize index = scalar_position == npos || scalar_position >= scalar_size_ ? scalar_size_ - 1 : scalar_position;
            const char *cursor = storage_data() + byte_index_of_unchecked(index + 1);
            while (true) {
                cursor = previous_codepoint(storage_data(), cursor);
                if (!scalars.contains_scalar_unchecked(decode_unchecked(cursor))) {
                    return index;
                }
                if (index == 0) {
                    break;
                }
                --index;
            }
            return npos;
        }

        [[nodiscard]] usize find_last_not_of(string_view scalars, usize scalar_position = npos) const {
            return find_last_not_of(UString{scalars}, scalar_position);
        }

        [[nodiscard]] bool is_codepoint_boundary(usize byte_index) const noexcept {
            if (byte_index > byte_size_) {
                return false;
            }
            if (byte_index == 0 || byte_index == byte_size_) {
                return true;
            }
            return !is_continuation_byte(static_cast<unsigned char>(storage_data()[byte_index]));
        }

        [[nodiscard]] usize byte_index_of(usize scalar_index) const {
            if (scalar_index > scalar_size_) {
                throw out_of_range{format("UString scalar index {} is out of range for size {}.", scalar_index, scalar_size_)};
            }
            return byte_index_of_unchecked(scalar_index);
        }

        [[nodiscard]] usize scalar_index_of_byte(usize byte_index) const {
            if (!is_codepoint_boundary(byte_index)) {
                throw out_of_range{format("UString byte index {} is not a UTF-8 scalar boundary.", byte_index)};
            }
            return scalar_index_of_byte_unchecked(byte_index);
        }

        [[nodiscard]] int compare(const UString &other) const noexcept {
            return view().compare(other.view());
        }

        [[nodiscard]] int compare(const ustr &other) const noexcept;

        [[nodiscard]] int compare(string_view other) const {
            validate_or_throw(other);
            return view().compare(other);
        }

        void swap(UString &other) noexcept {
            using std::swap;
            swap(byte_size_, other.byte_size_);
            swap(scalar_size_, other.scalar_size_);
            swap(capacity_, other.capacity_);
            swap(heap_, other.heap_);
            swap(small_, other.small_);
        }

        [[nodiscard]] friend bool operator==(const UString &lhs, const UString &rhs) noexcept {
            return lhs.view() == rhs.view();
        }

        [[nodiscard]] friend strong_ordering operator<=>(const UString &lhs, const UString &rhs) noexcept {
            const int result = lhs.compare(rhs);
            if (result < 0) {
                return strong_ordering::less;
            }
            if (result > 0) {
                return strong_ordering::greater;
            }
            return strong_ordering::equal;
        }

        [[nodiscard]] friend UString operator+(UString lhs, const UString &rhs) {
            lhs += rhs;
            return lhs;
        }

        [[nodiscard]] friend UString operator+(UString lhs, string_view rhs) {
            lhs += rhs;
            return lhs;
        }

        [[nodiscard]] friend UString operator+(string_view lhs, const UString &rhs) {
            UString result{lhs};
            result += rhs;
            return result;
        }

        [[nodiscard]] friend UString operator+(UString lhs, char32_t rhs) {
            lhs += rhs;
            return lhs;
        }

      private:
        struct ValidatedInput {};

        static constexpr u32 max_unicode_scalar = 0x10FFFF;

        UString(string_view text, usize scalar_count, ValidatedInput) {
            assign_validated_unaliased(text, scalar_count);
        }

        [[nodiscard]] static constexpr bool is_continuation_byte(unsigned char byte) noexcept {
            return (byte & 0xC0u) == 0x80u;
        }

        [[nodiscard]] static constexpr bool is_surrogate(u32 scalar) noexcept {
            return scalar >= 0xD800 && scalar <= 0xDFFF;
        }

        [[nodiscard]] static constexpr UStringValidation failure(UStringValidationError error, usize byte_index, usize scalar_count) noexcept {
            return UStringValidation{.valid = false, .error = error, .byte_index = byte_index, .scalar_count = scalar_count};
        }

        [[nodiscard]] static string_view as_char_view(u8string_view text) noexcept {
            return string_view{reinterpret_cast<const char *>(text.data()), text.size()};
        }

        static UStringValidation validate_or_throw(string_view text) {
            const UStringValidation validation = validate_utf8(text);
            if (!validation) {
                throw invalid_argument{format(
                    "UString requires strict UTF-8 without embedded NUL bytes: {} at byte {}.",
                    to_string(validation.error),
                    validation.byte_index)};
            }
            return validation;
        }

        static void validate_scalar_or_throw(char32_t scalar) {
            const auto codepoint = static_cast<u32>(scalar);
            if (codepoint == 0) {
                throw invalid_argument{"UString cannot store U+0000 because it would create an embedded NUL byte."};
            }
            if (is_surrogate(codepoint)) {
                throw invalid_argument{"UString cannot store UTF-16 surrogate code points."};
            }
            if (codepoint > max_unicode_scalar) {
                throw invalid_argument{"UString cannot store code points above U+10FFFF."};
            }
        }

        [[nodiscard]] static usize encode_scalar_or_throw(char32_t scalar, char *buffer) {
            validate_scalar_or_throw(scalar);
            const auto codepoint = static_cast<u32>(scalar);

            if (codepoint <= 0x7F) {
                buffer[0] = static_cast<char>(codepoint);
                return 1;
            }
            if (codepoint <= 0x7FF) {
                buffer[0] = static_cast<char>(0xC0 | (codepoint >> 6));
                buffer[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
                return 2;
            }
            if (codepoint <= 0xFFFF) {
                buffer[0] = static_cast<char>(0xE0 | (codepoint >> 12));
                buffer[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                buffer[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
                return 3;
            }

            buffer[0] = static_cast<char>(0xF0 | (codepoint >> 18));
            buffer[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
            buffer[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            buffer[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
            return 4;
        }

        [[nodiscard]] static usize encoded_length_from_lead(unsigned char lead) noexcept {
            if (lead <= 0x7F) {
                return 1;
            }
            if (lead <= 0xDF) {
                return 2;
            }
            if (lead <= 0xEF) {
                return 3;
            }
            return 4;
        }

        [[nodiscard]] static usize encoded_length_unchecked(const char *text) noexcept {
            return encoded_length_from_lead(static_cast<unsigned char>(*text));
        }

        [[nodiscard]] static char32_t decode_unchecked(const char *text) noexcept {
            const auto lead = static_cast<unsigned char>(text[0]);
            if (lead <= 0x7F) {
                return static_cast<char32_t>(lead);
            }

            const usize length = encoded_length_from_lead(lead);
            u32 scalar = 0;
            switch (length) {
            case 2:
                scalar = lead & 0x1Fu;
                break;
            case 3:
                scalar = lead & 0x0Fu;
                break;
            default:
                scalar = lead & 0x07u;
                break;
            }

            for (usize offset = 1; offset < length; ++offset) {
                scalar = (scalar << 6u) | (static_cast<unsigned char>(text[offset]) & 0x3Fu);
            }
            return static_cast<char32_t>(scalar);
        }

        [[nodiscard]] static const char *previous_codepoint(const char *begin, const char *cursor) noexcept {
            do {
                --cursor;
            } while (cursor > begin && is_continuation_byte(static_cast<unsigned char>(*cursor)));
            return cursor;
        }

        [[nodiscard]] static char *allocate_buffer(usize capacity) {
            if (capacity > max_size() || capacity == std::numeric_limits<usize>::max()) {
                throw length_error{"UString capacity exceeds max_size()."};
            }
            char *buffer = new char[capacity + 1];
            buffer[0] = '\0';
            return buffer;
        }

        [[nodiscard]] const char *storage_data() const noexcept {
            return heap_ == nullptr ? small_.data() : heap_;
        }

        [[nodiscard]] char *mutable_data() noexcept {
            return heap_ == nullptr ? small_.data() : heap_;
        }

        [[nodiscard]] bool overlaps_storage(string_view text) const noexcept {
            if (text.empty()) {
                return false;
            }
            const char *begin = storage_data();
            const char *end = begin + byte_size_;
            const char *text_begin = text.data();
            const char *text_end = text_begin + text.size();
            return text_begin < end && text_end > begin;
        }

        void move_from(UString &&other) noexcept {
            byte_size_ = other.byte_size_;
            scalar_size_ = other.scalar_size_;
            capacity_ = other.capacity_;
            small_ = other.small_;
            heap_ = other.heap_;

            other.heap_ = nullptr;
            other.byte_size_ = 0;
            other.scalar_size_ = 0;
            other.capacity_ = sso_capacity;
            other.small_[0] = '\0';
        }

        void assign_validated_unaliased(string_view text, usize scalar_count) {
            if (text.size() <= sso_capacity) {
                array<char, sso_capacity + 1> replacement{};
                if (!text.empty()) {
                    std::memcpy(replacement.data(), text.data(), text.size());
                }
                replacement[text.size()] = '\0';

                delete[] heap_;
                heap_ = nullptr;
                capacity_ = sso_capacity;
                small_ = replacement;
            } else {
                char *replacement = allocate_buffer(text.size());
                std::memcpy(replacement, text.data(), text.size());
                replacement[text.size()] = '\0';

                delete[] heap_;
                heap_ = replacement;
                capacity_ = text.size();
            }

            byte_size_ = text.size();
            scalar_size_ = scalar_count;
        }

        void ensure_capacity(usize requested_capacity) {
            if (requested_capacity <= capacity_) {
                return;
            }
            if (requested_capacity > max_size()) {
                throw length_error{"UString capacity request exceeds max_size()."};
            }

            usize new_capacity = capacity_;
            while (new_capacity < requested_capacity) {
                if (new_capacity > max_size() / 2) {
                    new_capacity = requested_capacity;
                    break;
                }
                new_capacity *= 2;
            }

            char *replacement = allocate_buffer(new_capacity);
            std::memcpy(replacement, storage_data(), byte_size_ + 1);
            delete[] heap_;
            heap_ = replacement;
            capacity_ = new_capacity;
        }

        [[nodiscard]] usize checked_total_byte_size(usize additional_bytes) const {
            if (additional_bytes > max_size() - byte_size_) {
                throw length_error{"UString operation would exceed max_size()."};
            }
            return byte_size_ + additional_bytes;
        }

        UString &append_validated(string_view text, usize scalar_count) {
            if (text.empty()) {
                return *this;
            }
            if (overlaps_storage(text)) {
                UString copy{text, scalar_count, ValidatedInput{}};
                return append_validated(copy.view(), copy.scalar_size_);
            }

            append_validated_unaliased(text, scalar_count);
            return *this;
        }

        void append_validated_unaliased(string_view text, usize scalar_count) {
            const usize old_byte_size = byte_size_;
            const usize new_byte_size = checked_total_byte_size(text.size());
            ensure_capacity(new_byte_size);

            std::memcpy(mutable_data() + old_byte_size, text.data(), text.size());
            byte_size_ = new_byte_size;
            scalar_size_ += scalar_count;
            mutable_data()[byte_size_] = '\0';
        }

        void append_encoded_scalar(const char *encoded, usize encoded_size) {
            const usize old_byte_size = byte_size_;
            const usize new_byte_size = checked_total_byte_size(encoded_size);
            ensure_capacity(new_byte_size);

            std::memcpy(mutable_data() + old_byte_size, encoded, encoded_size);
            byte_size_ = new_byte_size;
            ++scalar_size_;
            mutable_data()[byte_size_] = '\0';
        }

        UString &insert_validated(usize scalar_index, string_view text, usize scalar_count) {
            if (scalar_index > scalar_size_) {
                throw out_of_range{format("UString insert index {} is out of range for size {}.", scalar_index, scalar_size_)};
            }
            if (text.empty()) {
                return *this;
            }
            if (overlaps_storage(text)) {
                UString copy{text, scalar_count, ValidatedInput{}};
                return insert_validated(scalar_index, copy.view(), copy.scalar_size_);
            }

            const usize insert_byte = byte_index_of_unchecked(scalar_index);
            const usize new_byte_size = checked_total_byte_size(text.size());
            ensure_capacity(new_byte_size);
            char *target = mutable_data();

            std::memmove(target + insert_byte + text.size(), target + insert_byte, byte_size_ - insert_byte + 1);
            std::memcpy(target + insert_byte, text.data(), text.size());
            byte_size_ = new_byte_size;
            scalar_size_ += scalar_count;
            return *this;
        }

        UString &replace_validated(usize scalar_index, usize scalar_count, string_view replacement, usize replacement_scalar_count) {
            if (scalar_index > scalar_size_) {
                throw out_of_range{format("UString replace index {} is out of range for size {}.", scalar_index, scalar_size_)};
            }
            if (overlaps_storage(replacement)) {
                UString copy{replacement, replacement_scalar_count, ValidatedInput{}};
                return replace_validated(scalar_index, scalar_count, copy.view(), copy.scalar_size_);
            }

            const usize actual_scalar_count = scalar_count == npos ? scalar_size_ - scalar_index : std::min(scalar_count, scalar_size_ - scalar_index);
            const usize first_byte = byte_index_of_unchecked(scalar_index);
            const usize last_byte = byte_index_of_unchecked(scalar_index + actual_scalar_count);
            const usize removed_bytes = last_byte - first_byte;
            const usize preserved_bytes = byte_size_ - removed_bytes;
            if (replacement.size() > max_size() - preserved_bytes) {
                throw length_error{"UString replace operation would exceed max_size()."};
            }
            const usize new_byte_size = preserved_bytes + replacement.size();

            UString rebuilt;
            rebuilt.reserve(new_byte_size);
            rebuilt.append_validated_unaliased(string_view{storage_data(), first_byte}, scalar_index);
            rebuilt.append_validated_unaliased(replacement, replacement_scalar_count);
            rebuilt.append_validated_unaliased(
                string_view{storage_data() + last_byte, byte_size_ - last_byte},
                scalar_size_ - scalar_index - actual_scalar_count);
            swap(rebuilt);
            return *this;
        }

        [[nodiscard]] usize byte_index_of_unchecked(usize scalar_index) const noexcept {
            const char *cursor = storage_data();
            for (usize index = 0; index < scalar_index; ++index) {
                cursor += encoded_length_unchecked(cursor);
            }
            return static_cast<usize>(cursor - storage_data());
        }

        [[nodiscard]] usize scalar_index_of_byte_unchecked(usize byte_index) const noexcept {
            const char *cursor = storage_data();
            const char *target = storage_data() + byte_index;
            usize scalar_index = 0;
            while (cursor < target) {
                cursor += encoded_length_unchecked(cursor);
                ++scalar_index;
            }
            return scalar_index;
        }

        [[nodiscard]] CodepointIterator iterator_at_unchecked(usize scalar_index) const noexcept {
            const char *cursor = storage_data() + byte_index_of_unchecked(scalar_index);
            return CodepointIterator{storage_data(), cursor};
        }

        [[nodiscard]] bool contains_scalar_unchecked(char32_t scalar) const noexcept {
            for (char32_t current : codepoints()) {
                if (current == scalar) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] usize find_validated(string_view needle, usize scalar_position) const {
            if (scalar_position > scalar_size_) {
                throw out_of_range{"UString::find() start position is out of range."};
            }
            if (needle.empty()) {
                return scalar_position;
            }

            const usize start_byte = byte_index_of_unchecked(scalar_position);
            const size_t found = view().find(needle, start_byte);
            if (found == string_view::npos) {
                return npos;
            }
            return scalar_index_of_byte_unchecked(static_cast<usize>(found));
        }

        [[nodiscard]] usize rfind_validated(string_view needle, usize scalar_position) const {
            if (needle.empty()) {
                return scalar_position == npos ? scalar_size_ : std::min(scalar_position, scalar_size_);
            }
            if (byte_size_ < needle.size()) {
                return npos;
            }

            const usize clamped_scalar_position = scalar_position == npos ? scalar_size_ : std::min(scalar_position, scalar_size_);
            const usize byte_position = byte_index_of_unchecked(clamped_scalar_position);
            const size_t found = view().rfind(needle, byte_position);
            if (found == string_view::npos) {
                return npos;
            }
            return scalar_index_of_byte_unchecked(static_cast<usize>(found));
        }

        usize byte_size_ = 0;
        usize scalar_size_ = 0;
        usize capacity_ = sso_capacity;
        char *heap_ = nullptr;
        array<char, sso_capacity + 1> small_{};
    };

    inline void swap(UString &lhs, UString &rhs) noexcept {
        lhs.swap(rhs);
    }

    // Borrowed UTF-8 string slice: the Rust `&str` counterpart to owned `UString`.
    //
    // `ustr` owns no bytes, so it has `string_view`-like lifetime rules. It is intentionally non-copyable
    // and non-assignable to make engine APIs spell borrowed UTF-8 as `const ustr&`, which mirrors Rust's
    // convention of passing `str` behind a reference while still allowing literals and temporary slices to
    // bind to parameters.
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

        ustr(const char *text) {
            if (text == nullptr) {
                throw invalid_argument{"ustr cannot be constructed from a null pointer."};
            }
            assign_validated(string_view{text});
        }

        ustr(const char *text, usize byte_count) {
            if (text == nullptr && byte_count != 0) {
                throw invalid_argument{"ustr cannot be constructed from a null pointer and non-zero size."};
            }
            assign_validated(string_view{text == nullptr ? "" : text, byte_count});
        }

        ustr(string_view text) {
            assign_validated(text);
        }

        ustr(const string &text) {
            assign_validated(string_view{text});
        }

        ustr(string &&) = delete;
        ustr(const string &&) = delete;

        ustr(u8string_view text) {
            assign_validated(as_char_view(text));
        }

        ustr(const UString &text) noexcept
            : bytes_(text.view()), scalar_size_(text.scalar_size()) {
        }

        ustr(UString &&) = delete;
        ustr(const UString &&) = delete;

        [[nodiscard]] static UStringValidation validate_utf8(string_view text) noexcept {
            return UString::validate_utf8(text);
        }

        [[nodiscard]] static bool is_valid_utf8(string_view text) noexcept {
            return UString::is_valid_utf8(text);
        }

        [[nodiscard]] const char *data() const noexcept {
            return bytes_.data();
        }

        [[nodiscard]] string_view view() const noexcept {
            return bytes_;
        }

        [[nodiscard]] string_view bytes() const noexcept {
            return bytes_;
        }

        [[nodiscard]] string str() const {
            return string{bytes_};
        }

        [[nodiscard]] UString to_owned() const {
            return UString{*this};
        }

        [[nodiscard]] operator string_view() const noexcept {
            return bytes_;
        }

        [[nodiscard]] bool empty() const noexcept {
            return bytes_.empty();
        }

        [[nodiscard]] usize size() const noexcept {
            return scalar_size_;
        }

        [[nodiscard]] usize length() const noexcept {
            return scalar_size_;
        }

        [[nodiscard]] usize scalar_size() const noexcept {
            return scalar_size_;
        }

        [[nodiscard]] usize codepoint_size() const noexcept {
            return scalar_size_;
        }

        [[nodiscard]] usize byte_size() const noexcept {
            return bytes_.size();
        }

        [[nodiscard]] usize size_bytes() const noexcept {
            return bytes_.size();
        }

        [[nodiscard]] CodepointIterator begin() const noexcept {
            return CodepointIterator{bytes_.data(), bytes_.data()};
        }

        [[nodiscard]] CodepointIterator end() const noexcept {
            return CodepointIterator{bytes_.data(), bytes_.data() + bytes_.size()};
        }

        [[nodiscard]] CodepointView codepoints() const noexcept {
            return CodepointView{bytes_.data(), bytes_.size(), scalar_size_};
        }

        [[nodiscard]] const char *byte_begin() const noexcept {
            return bytes_.data();
        }

        [[nodiscard]] const char *byte_end() const noexcept {
            return bytes_.data() + bytes_.size();
        }

        [[nodiscard]] char32_t front() const {
            if (empty()) {
                throw out_of_range{"ustr::front() called on an empty string slice."};
            }
            return decode_unchecked(bytes_.data());
        }

        [[nodiscard]] char32_t back() const {
            if (empty()) {
                throw out_of_range{"ustr::back() called on an empty string slice."};
            }
            return decode_unchecked(previous_codepoint(bytes_.data(), bytes_.data() + bytes_.size()));
        }

        [[nodiscard]] char32_t at(usize scalar_index) const {
            if (scalar_index >= scalar_size_) {
                throw out_of_range{format("ustr scalar index {} is out of range for size {}.", scalar_index, scalar_size_)};
            }
            return decode_unchecked(bytes_.data() + byte_index_of_unchecked(scalar_index));
        }

        [[nodiscard]] char32_t operator[](usize scalar_index) const noexcept {
            assert(scalar_index < scalar_size_);
            return decode_unchecked(bytes_.data() + byte_index_of_unchecked(scalar_index));
        }

        [[nodiscard]] ustr operator[](USlice range) const {
            return slice(range);
        }

        [[nodiscard]] UString operator[](USlicePattern pattern) const {
            return slice(pattern);
        }

        [[nodiscard]] ustr slice(usize scalar_start) const {
            return slice(USlice{scalar_start});
        }

        [[nodiscard]] ustr slice(usize scalar_start, usize scalar_end) const {
            return slice(USlice{scalar_start, scalar_end});
        }

        [[nodiscard]] ustr slice(USlice range) const {
            const Detail::ResolvedSlice resolved = Detail::resolve_slice(range, scalar_size_, "ustr");
            const usize first_byte = byte_index_of_unchecked(resolved.start);
            const usize last_byte = byte_index_of_unchecked(resolved.end);
            return ustr{string_view{bytes_.data() + first_byte, last_byte - first_byte}, resolved.scalar_count(), ValidatedInput{}};
        }

        [[nodiscard]] UString slice(USlicePattern pattern) const {
            const Detail::ResolvedSlicePattern resolved = Detail::resolve_slice(pattern, scalar_size_, "ustr");
            UString result;

            for (usize group_start = resolved.range.start; group_start < resolved.range.end;) {
                const usize remaining = resolved.range.end - group_start;
                const usize group_count = std::min(resolved.grouping, remaining);
                result.append(slice(group_start, group_start + group_count));

                // `spread` is the number of scalars skipped between the end of the group we just
                // captured and the start of the next, so advance past both the group and the gap.
                group_start += group_count + resolved.spread;
            }

            return result;
        }

        [[nodiscard]] ustr substr(usize scalar_index = 0, usize scalar_count = npos) const {
            if (scalar_index > scalar_size_) {
                throw out_of_range{format("ustr substr index {} is out of range for size {}.", scalar_index, scalar_size_)};
            }

            const usize actual_scalar_count = scalar_count == npos ? scalar_size_ - scalar_index : std::min(scalar_count, scalar_size_ - scalar_index);
            const usize first_byte = byte_index_of_unchecked(scalar_index);
            const usize last_byte = byte_index_of_unchecked(scalar_index + actual_scalar_count);
            return ustr{string_view{bytes_.data() + first_byte, last_byte - first_byte}, actual_scalar_count, ValidatedInput{}};
        }

        [[nodiscard]] usize find(const ustr &needle, usize scalar_position = 0) const {
            if (scalar_position > scalar_size_) {
                throw out_of_range{"ustr::find() start position is out of range."};
            }
            if (needle.empty()) {
                return scalar_position;
            }

            const usize start_byte = byte_index_of_unchecked(scalar_position);
            const size_t found = bytes_.find(needle.bytes_, start_byte);
            if (found == string_view::npos) {
                return npos;
            }
            return scalar_index_of_byte_unchecked(static_cast<usize>(found));
        }

        [[nodiscard]] usize find(string_view needle, usize scalar_position = 0) const {
            const ustr checked{needle};
            return find(checked, scalar_position);
        }

        [[nodiscard]] usize find(u8string_view needle, usize scalar_position = 0) const {
            return find(as_char_view(needle), scalar_position);
        }

        [[nodiscard]] usize rfind(const ustr &needle, usize scalar_position = npos) const {
            if (needle.empty()) {
                return scalar_position == npos ? scalar_size_ : std::min(scalar_position, scalar_size_);
            }
            if (bytes_.size() < needle.bytes_.size()) {
                return npos;
            }

            const usize clamped_scalar_position = scalar_position == npos ? scalar_size_ : std::min(scalar_position, scalar_size_);
            const usize byte_position = byte_index_of_unchecked(clamped_scalar_position);
            const size_t found = bytes_.rfind(needle.bytes_, byte_position);
            if (found == string_view::npos) {
                return npos;
            }
            return scalar_index_of_byte_unchecked(static_cast<usize>(found));
        }

        [[nodiscard]] usize rfind(string_view needle, usize scalar_position = npos) const {
            const ustr checked{needle};
            return rfind(checked, scalar_position);
        }

        [[nodiscard]] usize rfind(u8string_view needle, usize scalar_position = npos) const {
            return rfind(as_char_view(needle), scalar_position);
        }

        [[nodiscard]] bool contains(const ustr &needle) const noexcept {
            return bytes_.find(needle.bytes_) != string_view::npos;
        }

        [[nodiscard]] bool contains(string_view needle) const {
            return find(needle) != npos;
        }

        [[nodiscard]] bool contains(u8string_view needle) const {
            return contains(as_char_view(needle));
        }

        [[nodiscard]] bool contains(char32_t scalar) const {
            validate_scalar_or_throw(scalar);
            return contains_scalar_unchecked(scalar);
        }

        [[nodiscard]] bool starts_with(const ustr &prefix) const noexcept {
            return bytes_.starts_with(prefix.bytes_);
        }

        [[nodiscard]] bool starts_with(string_view prefix) const {
            const ustr checked{prefix};
            return starts_with(checked);
        }

        [[nodiscard]] bool starts_with(u8string_view prefix) const {
            return starts_with(as_char_view(prefix));
        }

        [[nodiscard]] bool ends_with(const ustr &suffix) const noexcept {
            return bytes_.ends_with(suffix.bytes_);
        }

        [[nodiscard]] bool ends_with(string_view suffix) const {
            const ustr checked{suffix};
            return ends_with(checked);
        }

        [[nodiscard]] bool ends_with(u8string_view suffix) const {
            return ends_with(as_char_view(suffix));
        }

        [[nodiscard]] bool is_codepoint_boundary(usize byte_index) const noexcept {
            if (byte_index > bytes_.size()) {
                return false;
            }
            if (byte_index == 0 || byte_index == bytes_.size()) {
                return true;
            }
            return !is_continuation_byte(static_cast<unsigned char>(bytes_[byte_index]));
        }

        [[nodiscard]] usize byte_index_of(usize scalar_index) const {
            if (scalar_index > scalar_size_) {
                throw out_of_range{format("ustr scalar index {} is out of range for size {}.", scalar_index, scalar_size_)};
            }
            return byte_index_of_unchecked(scalar_index);
        }

        [[nodiscard]] usize scalar_index_of_byte(usize byte_index) const {
            if (!is_codepoint_boundary(byte_index)) {
                throw out_of_range{format("ustr byte index {} is not a UTF-8 scalar boundary.", byte_index)};
            }
            return scalar_index_of_byte_unchecked(byte_index);
        }

        [[nodiscard]] int compare(const ustr &other) const noexcept {
            return bytes_.compare(other.bytes_);
        }

        [[nodiscard]] int compare(string_view other) const {
            const ustr checked{other};
            return compare(checked);
        }

        [[nodiscard]] friend bool operator==(const ustr &lhs, const ustr &rhs) noexcept {
            return lhs.bytes_ == rhs.bytes_;
        }

        [[nodiscard]] friend strong_ordering operator<=>(const ustr &lhs, const ustr &rhs) noexcept {
            const int result = lhs.compare(rhs);
            if (result < 0) {
                return strong_ordering::less;
            }
            if (result > 0) {
                return strong_ordering::greater;
            }
            return strong_ordering::equal;
        }

      private:
        friend class UString;

        struct ValidatedInput {};

        constexpr ustr(string_view text, usize scalar_count, ValidatedInput) noexcept
            : bytes_(text), scalar_size_(scalar_count) {
        }

        void assign_validated(string_view text) {
            const UStringValidation validation = validate_or_throw(text);
            bytes_ = text;
            scalar_size_ = validation.scalar_count;
        }

        [[nodiscard]] static string_view as_char_view(u8string_view text) noexcept {
            return string_view{reinterpret_cast<const char *>(text.data()), text.size()};
        }

        static UStringValidation validate_or_throw(string_view text) {
            const UStringValidation validation = UString::validate_utf8(text);
            if (!validation) {
                throw invalid_argument{format(
                    "ustr requires strict UTF-8 without embedded NUL bytes: {} at byte {}.",
                    to_string(validation.error),
                    validation.byte_index)};
            }
            return validation;
        }

        [[nodiscard]] static constexpr bool is_continuation_byte(unsigned char byte) noexcept {
            return (byte & 0xC0u) == 0x80u;
        }

        static void validate_scalar_or_throw(char32_t scalar) {
            const auto codepoint = static_cast<u32>(scalar);
            if (codepoint == 0) {
                throw invalid_argument{"ustr cannot store or search for U+0000 because UTF-8 slices reject embedded NUL bytes."};
            }
            if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
                throw invalid_argument{"ustr cannot search for UTF-16 surrogate code points."};
            }
            if (codepoint > 0x10FFFF) {
                throw invalid_argument{"ustr cannot search for code points above U+10FFFF."};
            }
        }

        [[nodiscard]] static usize encoded_length_from_lead(unsigned char lead) noexcept {
            if (lead <= 0x7F) {
                return 1;
            }
            if (lead <= 0xDF) {
                return 2;
            }
            if (lead <= 0xEF) {
                return 3;
            }
            return 4;
        }

        [[nodiscard]] static usize encoded_length_unchecked(const char *text) noexcept {
            return encoded_length_from_lead(static_cast<unsigned char>(*text));
        }

        [[nodiscard]] static char32_t decode_unchecked(const char *text) noexcept {
            const auto lead = static_cast<unsigned char>(text[0]);
            if (lead <= 0x7F) {
                return static_cast<char32_t>(lead);
            }

            const usize length = encoded_length_from_lead(lead);
            u32 scalar = 0;
            switch (length) {
            case 2:
                scalar = lead & 0x1Fu;
                break;
            case 3:
                scalar = lead & 0x0Fu;
                break;
            default:
                scalar = lead & 0x07u;
                break;
            }

            for (usize offset = 1; offset < length; ++offset) {
                scalar = (scalar << 6u) | (static_cast<unsigned char>(text[offset]) & 0x3Fu);
            }
            return static_cast<char32_t>(scalar);
        }

        [[nodiscard]] static const char *previous_codepoint(const char *begin, const char *cursor) noexcept {
            do {
                --cursor;
            } while (cursor > begin && is_continuation_byte(static_cast<unsigned char>(*cursor)));
            return cursor;
        }

        [[nodiscard]] usize byte_index_of_unchecked(usize scalar_index) const noexcept {
            const char *cursor = bytes_.data();
            for (usize index = 0; index < scalar_index; ++index) {
                cursor += encoded_length_unchecked(cursor);
            }
            return static_cast<usize>(cursor - bytes_.data());
        }

        [[nodiscard]] usize scalar_index_of_byte_unchecked(usize byte_index) const noexcept {
            const char *cursor = bytes_.data();
            const char *target = bytes_.data() + byte_index;
            usize scalar_index = 0;
            while (cursor < target) {
                cursor += encoded_length_unchecked(cursor);
                ++scalar_index;
            }
            return scalar_index;
        }

        [[nodiscard]] bool contains_scalar_unchecked(char32_t scalar) const noexcept {
            for (char32_t current : codepoints()) {
                if (current == scalar) {
                    return true;
                }
            }
            return false;
        }

        string_view bytes_{};
        usize scalar_size_ = 0;
    };

    inline UString::UString(const ustr &text) {
        assign_validated_unaliased(text.view(), text.scalar_size());
    }

    inline UString &UString::operator=(const ustr &text) {
        return assign(text);
    }

    [[nodiscard]] inline ustr UString::as_ustr() const & noexcept {
        return ustr{view(), scalar_size_, ustr::ValidatedInput{}};
    }

    [[nodiscard]] inline ustr UString::slice() const & noexcept {
        return as_ustr();
    }

    [[nodiscard]] inline ustr UString::slice(usize scalar_start) const & {
        return slice(USlice{scalar_start});
    }

    [[nodiscard]] inline ustr UString::slice(usize scalar_start, usize scalar_end) const & {
        return slice(USlice{scalar_start, scalar_end});
    }

    [[nodiscard]] inline ustr UString::slice(USlice range) const & {
        const Detail::ResolvedSlice resolved = Detail::resolve_slice(range, scalar_size_, "UString");
        const usize first_byte = byte_index_of_unchecked(resolved.start);
        const usize last_byte = byte_index_of_unchecked(resolved.end);
        return ustr{string_view{storage_data() + first_byte, last_byte - first_byte}, resolved.scalar_count(), ustr::ValidatedInput{}};
    }

    [[nodiscard]] inline UString UString::slice(USlicePattern pattern) const {
        return as_ustr().slice(pattern);
    }

    [[nodiscard]] inline ustr UString::operator[](USlice range) const & {
        return slice(range);
    }

    [[nodiscard]] inline UString UString::operator[](USlicePattern pattern) const {
        return slice(pattern);
    }

    [[nodiscard]] inline UString::operator ustr() const & noexcept {
        return as_ustr();
    }

    inline UString &UString::assign(const ustr &text) {
        assign_validated_unaliased(text.view(), text.scalar_size());
        return *this;
    }

    inline UString &UString::append(const ustr &text) {
        return append_validated(text.view(), text.scalar_size());
    }

    inline UString &UString::operator+=(const ustr &text) {
        return append(text);
    }

    inline UString &UString::insert(usize scalar_index, const ustr &text) {
        return insert_validated(scalar_index, text.view(), text.scalar_size());
    }

    inline UString &UString::replace(usize scalar_index, usize scalar_count, const ustr &replacement) {
        return replace_validated(scalar_index, scalar_count, replacement.view(), replacement.scalar_size());
    }

    inline UString &UString::replace_all(const ustr &needle, const ustr &replacement) {
        if (needle.empty()) {
            throw invalid_argument{"UString::replace_all() requires a non-empty needle."};
        }

        usize search_from = 0;
        while (true) {
            const usize found = find(needle, search_from);
            if (found == npos) {
                break;
            }

            replace(found, needle.scalar_size(), replacement);
            search_from = found + replacement.scalar_size();
        }
        return *this;
    }

    [[nodiscard]] inline usize UString::find(const ustr &needle, usize scalar_position) const {
        return find_validated(needle.view(), scalar_position);
    }

    [[nodiscard]] inline usize UString::rfind(const ustr &needle, usize scalar_position) const {
        return rfind_validated(needle.view(), scalar_position);
    }

    [[nodiscard]] inline bool UString::contains(const ustr &needle) const noexcept {
        return view().find(needle.view()) != string_view::npos;
    }

    [[nodiscard]] inline bool UString::starts_with(const ustr &prefix) const noexcept {
        return view().starts_with(prefix.view());
    }

    [[nodiscard]] inline bool UString::ends_with(const ustr &suffix) const noexcept {
        return view().ends_with(suffix.view());
    }

    [[nodiscard]] inline int UString::compare(const ustr &other) const noexcept {
        return view().compare(other.view());
    }

    [[nodiscard]] inline bool operator==(const UString &lhs, const ustr &rhs) noexcept {
        return lhs.view() == rhs.view();
    }

    [[nodiscard]] inline bool operator==(const ustr &lhs, const UString &rhs) noexcept {
        return lhs.view() == rhs.view();
    }

    [[nodiscard]] inline strong_ordering operator<=>(const UString &lhs, const ustr &rhs) noexcept {
        const int result = lhs.compare(rhs);
        if (result < 0) {
            return strong_ordering::less;
        }
        if (result > 0) {
            return strong_ordering::greater;
        }
        return strong_ordering::equal;
    }

    [[nodiscard]] inline strong_ordering operator<=>(const ustr &lhs, const UString &rhs) noexcept {
        const int result = lhs.compare(rhs.as_ustr());
        if (result < 0) {
            return strong_ordering::less;
        }
        if (result > 0) {
            return strong_ordering::greater;
        }
        return strong_ordering::equal;
    }

    [[nodiscard]] inline UString operator+(UString lhs, const ustr &rhs) {
        lhs += rhs;
        return lhs;
    }

    [[nodiscard]] inline UString operator+(const ustr &lhs, const UString &rhs) {
        UString result{lhs};
        result += rhs;
        return result;
    }

    inline std::ostream &operator<<(std::ostream &os, const UString &value) {
        return os << value.view();
    }

    inline std::ostream &operator<<(std::ostream &os, const ustr &value) {
        return os << value.view();
    }

    inline std::ostream &operator<<(std::ostream &os, USlice value) {
        return os << Detail::display_string(value);
    }

    inline std::ostream &operator<<(std::ostream &os, USlicePattern value) {
        return os << Detail::display_string(value);
    }

    inline std::ostream &operator<<(std::ostream &os, UStringValidationError value) {
        return os << to_string(value);
    }

    inline std::ostream &operator<<(std::ostream &os, const UStringValidation &value) {
        return os << Detail::display_string(value);
    }

} // namespace SFT::Foundation

export [[nodiscard]] SFT::Foundation::ustr operator""_ustr(const char *text, size_t byte_count) {
    return SFT::Foundation::ustr{string_view{text, byte_count}};
}

export [[nodiscard]] SFT::Foundation::ustr operator""_ustr(const char8_t *text, size_t byte_count) {
    return SFT::Foundation::ustr{u8string_view{text, byte_count}};
}

export namespace SFT::Foundation {

    namespace Literals {
        using ::operator""_ustr;
    } // namespace Literals

} // namespace SFT::Foundation

export namespace SFT {

    using USlice [[maybe_unused]] = Foundation::USlice;
    using USlicePattern [[maybe_unused]] = Foundation::USlicePattern;
    using ustr [[maybe_unused]] = Foundation::ustr;
    using UString [[maybe_unused]] = Foundation::UString;
    using Foundation::slice_from;
    using Foundation::uslice;

} // namespace SFT

// `std::formatter` specializations for every own-type this partition exposes. Each delegates to the
// `string_view` formatter so the types work with `std::format`/`std::print` (including format specs like
// `{:>10}`); the `format` member is templated on the context so the types also satisfy `std::formattable`
// (and thus `Displayable`), which probes with a different context than `std::format` itself uses. They are
// intentionally left non-exported: explicit specializations are found by reachability rather than name
// lookup, so importing the module makes them usable without polluting the exported name set.
template <>
struct std::formatter<SFT::Foundation::UString> : std::formatter<std::string_view> {
    auto format(const SFT::Foundation::UString &value, auto &ctx) const {
        return std::formatter<std::string_view>::format(value.view(), ctx);
    }
};

template <>
struct std::formatter<SFT::Foundation::ustr> : std::formatter<std::string_view> {
    auto format(const SFT::Foundation::ustr &value, auto &ctx) const {
        return std::formatter<std::string_view>::format(value.view(), ctx);
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
struct std::formatter<SFT::Foundation::UStringValidation> : std::formatter<std::string_view> {
    auto format(const SFT::Foundation::UStringValidation &value, auto &ctx) const {
        return std::formatter<std::string_view>::format(SFT::Foundation::Detail::display_string(value), ctx);
    }
};

// Every own-type this partition exposes is `Displayable` (streams with `<<` and formats with
// `std::format`). Checked here, after the formatter specializations are in scope, so a regression in
// either facility is a hard compile error rather than a silent loss of printability.
static_assert(SFT::Foundation::Displayable<SFT::Foundation::UString>);
static_assert(SFT::Foundation::Displayable<SFT::Foundation::ustr>);
static_assert(SFT::Foundation::Displayable<SFT::Foundation::USlice>);
static_assert(SFT::Foundation::Displayable<SFT::Foundation::USlicePattern>);
static_assert(SFT::Foundation::Displayable<SFT::Foundation::UStringValidationError>);
static_assert(SFT::Foundation::Displayable<SFT::Foundation::UStringValidation>);
