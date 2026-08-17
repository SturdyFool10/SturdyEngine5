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

        /// Constructs a `USlice` from the supplied initialization values.
        ///
        /// @param start First position or element included in the operation.
        ///
        /// @note This function does not throw exceptions.
        constexpr explicit USlice(usize start) noexcept
            : start_(start) {
        }

        /// Constructs a `USlice` from the supplied initialization values.
        ///
        /// @param start First position or element included in the operation.
        /// @param end End boundary for the operation; where applicable this is one-past-the-last element.
        ///
        /// @note This function does not throw exceptions.
        constexpr USlice(usize start, usize end) noexcept
            : start_(start), end_(end) {
        }

        /// Starts the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @return Returns the current start value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr usize start() const noexcept {
            return start_;
        }

        /// Returns end when available, otherwise uses the supplied fallback.
        ///
        /// @param fallback Fallback value used when the primary value is unavailable.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr usize end_or(usize fallback) const noexcept {
            return has_end() ? end_ : fallback;
        }

        /// Reports whether this `USlice` has end.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr bool has_end() const noexcept {
            return end_ != npos;
        }

        /// Returns a copy of the `USlice` with its end boundary set to the supplied value.
        ///
        /// @param end End boundary for the operation; where applicable this is one-past-the-last element.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr USlice to(usize end) const noexcept {
            return USlice{start_, end};
        }

        /// Returns a copy of the `USlice` with its end boundary set to the supplied value.
        ///
        /// @param end End boundary for the operation; where applicable this is one-past-the-last element.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr USlice until(usize end) const noexcept {
            return to(end);
        }

        /// Returns a slice pattern using the supplied spread between selected groups.
        ///
        /// @param spread Spacing between selected groups in the slice pattern.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr USlicePattern spread_by(usize spread) const noexcept;
        /// Returns a slice pattern using the supplied spread between selected groups.
        ///
        /// @param spread Spacing between selected groups in the slice pattern.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr USlicePattern by(usize spread) const noexcept;
        /// Returns a slice pattern using the supplied grouping size.
        ///
        /// @param grouping Number of adjacent elements included in each selected group.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr USlicePattern grouped(usize grouping) const noexcept;
        /// Returns a slice pattern using the supplied grouping size.
        ///
        /// @param grouping Number of adjacent elements included in each selected group.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr USlicePattern group(usize grouping) const noexcept;

      private:
        usize start_ = 0;
        usize end_ = npos;
    };

    class USlicePattern {
      public:
        static constexpr usize npos = USlice::npos;

        /// Constructs a `USlicePattern` from the supplied initialization values.
        ///
        /// @param start First position or element included in the operation.
        ///
        /// @note This function does not throw exceptions.
        constexpr explicit USlicePattern(usize start) noexcept
            : start_(start) {
        }

        /// Constructs a `USlicePattern` from the supplied initialization values.
        ///
        /// @param start First position or element included in the operation.
        /// @param end End boundary for the operation; where applicable this is one-past-the-last element.
        ///
        /// @note This function does not throw exceptions.
        constexpr USlicePattern(usize start, usize end) noexcept
            : start_(start), end_(end) {
        }

        /// Constructs a `USlicePattern` from the supplied initialization values.
        ///
        /// @param range Range of values to process.
        ///
        /// @note This function does not throw exceptions.
        constexpr explicit USlicePattern(USlice range) noexcept
            : start_(range.start()), end_(range.end_or(npos)) {
        }

        /// Starts the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @return Returns the current start value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr usize start() const noexcept {
            return start_;
        }

        /// Returns end when available, otherwise uses the supplied fallback.
        ///
        /// @param fallback Fallback value used when the primary value is unavailable.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr usize end_or(usize fallback) const noexcept {
            return has_end() ? end_ : fallback;
        }

        /// Reports whether this `USlicePattern` has end.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr bool has_end() const noexcept {
            return end_ != npos;
        }

        /// Returns the current or globally available spread value.
        ///
        /// @return Returns the current spread value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr usize spread() const noexcept {
            return spread_;
        }

        /// Returns the current or globally available grouping value.
        ///
        /// @return Returns the current grouping value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr usize grouping() const noexcept {
            return grouping_;
        }

        /// Returns a copy of the `USlicePattern` with its end boundary set to the supplied value.
        ///
        /// @param end End boundary for the operation; where applicable this is one-past-the-last element.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr USlicePattern to(usize end) const noexcept {
            USlicePattern result = *this;
            result.end_ = end;
            return result;
        }

        /// Returns a copy of the `USlicePattern` with its end boundary set to the supplied value.
        ///
        /// @param end End boundary for the operation; where applicable this is one-past-the-last element.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr USlicePattern until(usize end) const noexcept {
            return to(end);
        }

        /// Returns a slice pattern using the supplied spread between selected groups.
        ///
        /// @param spread Spacing between selected groups in the slice pattern.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr USlicePattern spread_by(usize spread) const noexcept {
            USlicePattern result = *this;
            result.spread_ = spread;
            return result;
        }

        /// Returns a slice pattern using the supplied spread between selected groups.
        ///
        /// @param spread Spacing between selected groups in the slice pattern.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr USlicePattern by(usize spread) const noexcept {
            return spread_by(spread);
        }

        /// Returns a slice pattern using the supplied grouping size.
        ///
        /// @param grouping Number of adjacent elements included in each selected group.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr USlicePattern grouped(usize grouping) const noexcept {
            USlicePattern result = *this;
            result.grouping_ = grouping;
            return result;
        }

        /// Returns a slice pattern using the supplied grouping size.
        ///
        /// @param grouping Number of adjacent elements included in each selected group.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr USlicePattern group(usize grouping) const noexcept {
            return grouped(grouping);
        }

      private:
        usize start_ = 0;
        usize end_ = npos;
        usize spread_ = 0;
        usize grouping_ = 1;
    };

    /// Performs the spread by operation for `Foundation` using the supplied arguments.
    ///
    /// @param spread Spacing between selected groups in the slice pattern.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr USlicePattern USlice::spread_by(usize spread) const noexcept {
        return USlicePattern{*this}.spread_by(spread);
    }

    /// Performs the by operation for `Foundation` using the supplied arguments.
    ///
    /// @param spread Spacing between selected groups in the slice pattern.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr USlicePattern USlice::by(usize spread) const noexcept {
        return spread_by(spread);
    }

    /// Performs the grouped operation for `Foundation` using the supplied arguments.
    ///
    /// @param grouping Number of adjacent elements included in each selected group.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr USlicePattern USlice::grouped(usize grouping) const noexcept {
        return USlicePattern{*this}.grouped(grouping);
    }

    /// Performs the group operation for `Foundation` using the supplied arguments.
    ///
    /// @param grouping Number of adjacent elements included in each selected group.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr USlicePattern USlice::group(usize grouping) const noexcept {
        return grouped(grouping);
    }

    /// Performs the uslice operation using the supplied arguments.
    ///
    /// @param start First position or element included in the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr USlice uslice(usize start) noexcept {
        return USlice{start};
    }

    /// Performs the uslice operation using the supplied arguments.
    ///
    /// @param start First position or element included in the operation.
    /// @param end End boundary for the operation; where applicable this is one-past-the-last element.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr USlice uslice(usize start, usize end) noexcept {
        return USlice{start, end};
    }

    /// Performs the slice from operation using the supplied arguments.
    ///
    /// @param start First position or element included in the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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


    enum class TextConversionError : u8 {
        NonAscii,
    };

    struct UStringValidation {
        bool valid = true;
        UStringValidationError error = UStringValidationError::None;
        usize byte_index = 0;
        usize scalar_count = 0;

        /// Converts the `UStringValidation` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return valid;
        }
    };

    /// Converts the value to string representation.
    ///
    /// @param error Error value describing the failure.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
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

    /// Converts the value to string representation.
    ///
    /// @param error Error value describing the failure.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
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

            /// Returns the scalar count for this `ResolvedSlice`.
            ///
            /// @return Returns the current scalar count value.
            /// @note This function does not throw exceptions.
            [[nodiscard]] constexpr usize scalar_count() const noexcept {
                return end - start;
            }
        };

        struct ResolvedSlicePattern {
            ResolvedSlice range{};
            usize spread = 0;
            usize grouping = 1;
        };

        /// Resolves slice into the concrete value used by the engine.
        ///
        /// @param slice `slice` value used by the operation.
        /// @param scalar_size Requested or available size for the operation.
        /// @param owner Owner/context identifier used for validation or diagnostics.
        ///
        /// @return Returns the value produced by the operation.
        /// @throws `out_of_range` if `start > scalar_size`.
        /// @throws `out_of_range` if `end > scalar_size`.
        /// @throws `out_of_range` if `end < start`.
        [[nodiscard]] ResolvedSlice resolve_slice(USlice slice, usize scalar_size, string_view owner);

        /// Resolves slice into the concrete value used by the engine.
        ///
        /// @param slice `slice` value used by the operation.
        /// @param scalar_size Requested or available size for the operation.
        /// @param owner Owner/context identifier used for validation or diagnostics.
        ///
        /// @return Returns the value produced by the operation.
        /// @throws `invalid_argument` if `grouping == 0`.
        [[nodiscard]] ResolvedSlicePattern resolve_slice(USlicePattern slice, usize scalar_size, string_view owner);


        /// Performs the bounded c length operation using the supplied arguments.
        ///
        /// @param text Text consumed by the operation.
        /// @param max_bytes `max_bytes` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize bounded_c_length(const char *text, usize max_bytes) noexcept;


        /// Performs the display string operation using the supplied arguments.
        ///
        /// @param slice `slice` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] string display_string(USlice slice);

        /// Performs the display string operation using the supplied arguments.
        ///
        /// @param pattern `pattern` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] string display_string(USlicePattern pattern);

        /// Performs the display string operation using the supplied arguments.
        ///
        /// @param validation `validation` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] string display_string(const UStringValidation &validation);

    } // namespace Detail


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

            /// Constructs a `CodepointIterator` in its default state.
            ///
            /// @note This function does not throw exceptions.
            constexpr CodepointIterator() noexcept = default;

            /// Dereferences this iterator or handle.
            ///
            /// @return Returns the value or reference currently addressed by the iterator/handle.
            /// @pre `current_ != nullptr`; debug builds assert if this precondition is violated.
            /// @note This function does not throw exceptions.
            [[nodiscard]] char32_t operator*() const noexcept;

            /// Advances the `CodepointIterator` to its next value or element.
            ///
            /// @return Returns `*this` so the operation can be chained.
            /// @pre `current_ != nullptr`; debug builds assert if this precondition is violated.
            /// @note This function does not throw exceptions.
            CodepointIterator &operator++() noexcept;

            /// Advances the `CodepointIterator` to its next value or element.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function does not throw exceptions.
            CodepointIterator operator++(int) noexcept;

            /// Moves the `CodepointIterator` to its previous value or element.
            ///
            /// @return Returns `*this` so the operation can be chained.
            /// @pre `begin_ != nullptr`; debug builds assert if this precondition is violated.
            /// @pre `current_ != nullptr`; debug builds assert if this precondition is violated.
            /// @pre `current_ > begin_`; debug builds assert if this precondition is violated.
            /// @note This function does not throw exceptions.
            CodepointIterator &operator--() noexcept;

            /// Moves the `CodepointIterator` to its previous value or element.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function does not throw exceptions.
            CodepointIterator operator--(int) noexcept;

            /// Compares the operands for equality.
            ///
            /// @param lhs Left-hand operand.
            /// @param rhs Right-hand operand.
            ///
            /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
            /// @note This function does not throw exceptions.
            friend bool operator==(CodepointIterator lhs, CodepointIterator rhs) noexcept;

          private:
            friend class UString;
            friend class ustr;

            /// Constructs a `CodepointIterator` from the supplied initialization values.
            ///
            /// @param begin First position or element included in the operation.
            /// @param current `current` value used by the operation.
            ///
            /// @note This function does not throw exceptions.
            constexpr CodepointIterator(const char *begin, const char *current) noexcept
                : begin_(begin), current_(current) {
            }

            const char *begin_ = nullptr;
            const char *current_ = nullptr;
        };

        class CodepointView : public std::ranges::view_interface<CodepointView> {
          public:
            /// Constructs a `CodepointView` in its default state.
            ///
            /// @note This function does not throw exceptions.
            constexpr CodepointView() noexcept = default;

            /// Returns an iterator to the first element in the range.
            ///
            /// @return Returns an iterator referring to the first element.
            /// @note This function does not throw exceptions.
            [[nodiscard]] constexpr CodepointIterator begin() const noexcept {
                return CodepointIterator{data_, data_};
            }

            /// Returns the one-past-the-end iterator for the range.
            ///
            /// @return Returns the one-past-the-end iterator.
            /// @note This function does not throw exceptions.
            [[nodiscard]] constexpr CodepointIterator end() const noexcept {
                return CodepointIterator{data_, data_ + byte_size_};
            }

            /// Returns the size for this `CodepointView`.
            ///
            /// @return Returns the current size value.
            /// @note This function does not throw exceptions.
            /// Returns the size for this `CodepointView`.
            ///
            /// @return Returns the current size value.
            /// @note This function does not throw exceptions.
            [[nodiscard]] constexpr usize size() const noexcept {
                return scalar_size_;
            }

          private:
            friend class UString;
            friend class ustr;

            /// Constructs a `CodepointView` from the supplied initialization values.
            ///
            /// @param data Data consumed or referenced by the operation.
            /// @param byte_size Requested or available size for the operation.
            /// @param scalar_size Requested or available size for the operation.
            ///
            /// @note This function does not throw exceptions.
            constexpr CodepointView(const char *data, usize byte_size, usize scalar_size) noexcept
                : data_(data), byte_size_(byte_size), scalar_size_(scalar_size) {
            }

            const char *data_ = "";
            usize byte_size_ = 0;
            usize scalar_size_ = 0;
        };

        /// Constructs a `UString` in its default state.
        ///
        /// @note This function does not throw exceptions.
        UString() noexcept;

        /// Disables this construction form for `UString`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        UString(std::nullptr_t) = delete;

        /// Constructs a `UString` from the supplied initialization values.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @throws `invalid_argument` if `text == nullptr`.
        UString(const char *text);

        /// Constructs a `UString` from the supplied initialization values.
        ///
        /// @param text Text consumed by the operation.
        /// @param byte_count Number of elements or operations to process.
        ///
        /// @throws `invalid_argument` if `text == nullptr && byte_count != 0`.
        UString(const char *text, usize byte_count);

        /// Constructs a `UString` from the supplied initialization values.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString(string_view text);


        /// Constructs a `UString` from the supplied initialization values.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @note This function does not throw exceptions.
        UString(const string &text) noexcept;

        /// Constructs a `UString` from the supplied initialization values.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString(u8string_view text);

        /// Constructs a `UString` from the supplied initialization values.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString(const ustr &text);

        /// Constructs a `UString` from another instance.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString(const UString &other);

        /// Constructs a `UString` from another instance.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @note This function does not throw exceptions.
        UString(UString &&other) noexcept;

        /// Destroys the `UString` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~UString() noexcept;

        /// Assigns a new value to this `UString`.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        UString &operator=(UString other) noexcept;

        /// Assigns a new value to this `UString`.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &operator=(string_view text);

        /// Assigns a new value to this `UString`.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @throws `invalid_argument` if `text == nullptr`.
        UString &operator=(const char *text);

        /// Assigns a new value to this `UString`.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &operator=(u8string_view text);

        /// Assigns a new value to this `UString`.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &operator=(const ustr &text);

        /// Returns the max size for this `UString`.
        ///
        /// @return Returns the current max size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr usize max_size() noexcept {
            return std::numeric_limits<usize>::max() / 2;
        }

        /// Validates UTF-8.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static UStringValidation validate_utf8(string_view text) noexcept;

        /// Reports whether valid UTF-8 holds for this `UString`.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static bool is_valid_utf8(string_view text) noexcept;

        /// Attempts to from UTF-8 without requiring normal failure to be exceptional.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] static optional<UString> try_from_utf8(string_view text);

        /// Attempts to from UTF-8 without requiring normal failure to be exceptional.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        [[nodiscard]] static optional<UString> try_from_utf8(u8string_view text);

        /// Creates or converts a value from UTF-8 range representation.
        ///
        /// @return Returns the newly constructed or converted value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <std::ranges::input_range Range>
            requires std::convertible_to<std::ranges::range_reference_t<Range>, char>
        [[nodiscard]] static UString from_utf8_range(Range &&range) {
            UString value;
            value.assign_range(std::forward<Range>(range));
            return value;
        }

        /// Creates or converts a value from codepoints representation.
        ///
        /// @return Returns the newly constructed or converted value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <std::ranges::input_range Range>
            requires std::convertible_to<std::ranges::range_reference_t<Range>, char32_t>
        [[nodiscard]] static UString from_codepoints(Range &&range) {
            UString value;
            value.append_codepoints(std::forward<Range>(range));
            return value;
        }


        /// Creates or converts a value from c str representation.
        ///
        /// @param buffer Buffer used or affected by the operation.
        ///
        /// @return Returns the newly constructed or converted value.
        /// @throws `invalid_argument` if `buffer == nullptr`.
        [[nodiscard]] static UString from_c_str(const char *buffer);


        /// Creates or converts a value from c str representation.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param max_bytes `max_bytes` value used by the operation.
        ///
        /// @return Returns the newly constructed or converted value.
        /// @throws `invalid_argument` if `buffer == nullptr && max_bytes != 0`.
        [[nodiscard]] static UString from_c_str(const char *buffer, usize max_bytes);


        /// Attempts to from c str without requiring normal failure to be exceptional.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param max_bytes `max_bytes` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] static optional<UString> try_from_c_str(const char *buffer, usize max_bytes);


        /// Creates or converts a value from UTF-16 representation.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns the newly constructed or converted value.
        /// @throws `invalid_argument` if `index + 1 >= text.size()`.
        /// @throws `invalid_argument` if `low < 0xDC00 || low > 0xDFFF`.
        /// @throws `invalid_argument` if `unit >= 0xDC00 && unit <= 0xDFFF`.
        [[nodiscard]] static UString from_utf16(u16string_view text);


        /// Creates or converts a value from UTF-32 representation.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns the newly constructed or converted value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static UString from_utf32(u32string_view text);


        /// Creates or converts a value from wstring representation.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns the newly constructed or converted value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static UString from_wstring(wstring_view text);

        /// Returns the data associated with this `UString`.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const char *data() const noexcept;


        /// Returns the c str associated with this `UString`.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const char *c_str() const noexcept;

        /// Returns the current or globally available C++ string view value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] string_view cpp_string_view() const noexcept;

        /// Computes the C++ bytes required by the supplied values.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] string_view cpp_bytes() const noexcept;

        /// Returns the current or globally available as ustr value.
        ///
        /// @return Returns the current as ustr value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ustr as_ustr() const & noexcept;
        /// Returns the current or globally available as ustr value.
        ///
        /// @return Returns the current as ustr value.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        [[nodiscard]] ustr as_ustr() const && = delete;

        /// Returns the current or globally available slice value.
        ///
        /// @return Returns the current slice value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ustr slice() const & noexcept;
        /// Returns the current or globally available slice value.
        ///
        /// @return Returns the current slice value.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        [[nodiscard]] ustr slice() const && = delete;
        /// Performs the slice operation for `UString` using the supplied arguments.
        ///
        /// @param scalar_start `scalar_start` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] ustr slice(usize scalar_start) const &;
        /// Performs the slice operation for `UString` using the supplied arguments.
        ///
        /// @param scalar_start `scalar_start` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        [[nodiscard]] ustr slice(usize scalar_start) const && = delete;
        /// Performs the slice operation for `UString` using the supplied arguments.
        ///
        /// @param scalar_start `scalar_start` value used by the operation.
        /// @param scalar_end `scalar_end` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] ustr slice(usize scalar_start, usize scalar_end) const &;
        /// Performs the slice operation for `UString` using the supplied arguments.
        ///
        /// @param scalar_start `scalar_start` value used by the operation.
        /// @param scalar_end `scalar_end` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        [[nodiscard]] ustr slice(usize scalar_start, usize scalar_end) const && = delete;
        /// Performs the slice operation for `UString` using the supplied arguments.
        ///
        /// @param range Range of values to process.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] ustr slice(USlice range) const &;
        /// Performs the slice operation for `UString` using the supplied arguments.
        ///
        /// @param range Range of values to process.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        [[nodiscard]] ustr slice(USlice range) const && = delete;
        /// Performs the slice operation for `UString` using the supplied arguments.
        ///
        /// @param pattern `pattern` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString slice(USlicePattern pattern) const;

        /// Converts the `UString` to `ustr`.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] operator ustr() const & noexcept;
        /// Converts the `UString` to `ustr`.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        [[nodiscard]] operator ustr() const && = delete;

        /// Returns the current or globally available C++ string value.
        ///
        /// @return Returns the current C++ string value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] string cpp_string() const;


        /// Converts the value to std string representation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `TextConversionError::NonAscii`.
        [[nodiscard]] expected<string, TextConversionError> to_std_string() const;


        /// Converts the value to std string unchecked representation.
        ///
        /// @return Returns the current to std string unchecked value.
        /// @note Terminates the process if an invariant required by this unchecked operation is violated.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] string to_std_string_unchecked() const;


        /// Returns the current or globally available C++ u8string view value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u8string_view cpp_u8string_view() const noexcept;


        /// Returns the current or globally available C++ u8string value.
        ///
        /// @return Returns the current C++ u8string value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] u8string cpp_u8string() const;


        /// Returns the current or globally available C++ u16string value.
        ///
        /// @return Returns the current C++ u16string value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] u16string cpp_u16string() const;
        /// Returns the current or globally available C++ u32string value.
        ///
        /// @return Returns the current C++ u32string value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] u32string cpp_u32string() const;
        /// Returns the current or globally available C++ wstring value.
        ///
        /// @return Returns the current C++ wstring value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] wstring cpp_wstring() const;

        /// Converts the `UString` to `basic_string_view`.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] operator string_view() const noexcept;

        /// Reports whether this `UString` contains no elements or payload.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool empty() const noexcept;

        /// Reports whether ascii holds for this `UString`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_ascii() const noexcept;

        /// Returns the size for this `UString`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        /// Returns the size for this `UString`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize size() const noexcept;

        /// Returns the length for this `UString`.
        ///
        /// @return Returns the current length value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize length() const noexcept;

        /// Returns the scalar size for this `UString`.
        ///
        /// @return Returns the current scalar size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize scalar_size() const noexcept;

        /// Returns the codepoint size for this `UString`.
        ///
        /// @return Returns the current codepoint size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize codepoint_size() const noexcept;

        /// Returns the byte size for this `UString`.
        ///
        /// @return Returns the current byte size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize byte_size() const noexcept;

        /// Computes the size bytes required by the supplied values.
        ///
        /// @return Returns the current size bytes value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize size_bytes() const noexcept;

        /// Returns the current or globally available capacity value.
        ///
        /// @return Returns the current capacity value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize capacity() const noexcept;

        /// Reports whether small holds for this `UString`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_small() const noexcept;

        /// Returns an iterator to the first element in the range.
        ///
        /// @return Returns an iterator referring to the first element.
        /// @note This function does not throw exceptions.
        [[nodiscard]] CodepointIterator begin() const noexcept;

        /// Returns the one-past-the-end iterator for the range.
        ///
        /// @return Returns the one-past-the-end iterator.
        /// @note This function does not throw exceptions.
        [[nodiscard]] CodepointIterator end() const noexcept;

        /// Returns the current or globally available codepoints value.
        ///
        /// @return Returns the current codepoints value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] CodepointView codepoints() const noexcept;

        /// Returns the current or globally available byte begin value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const char *byte_begin() const noexcept;

        /// Returns the current or globally available byte end value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const char *byte_end() const noexcept;

        /// Returns the current or globally available front value.
        ///
        /// @return Returns the current front value.
        /// @throws `out_of_range` if `empty()`.
        [[nodiscard]] char32_t front() const;

        /// Returns the current or globally available back value.
        ///
        /// @return Returns the current back value.
        /// @throws `out_of_range` if `empty()`.
        [[nodiscard]] char32_t back() const;

        /// Performs the at operation for `UString` using the supplied arguments.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @throws `out_of_range` if `scalar_index >= scalar_size_`.
        [[nodiscard]] char32_t at(usize scalar_index) const;

        /// Accesses an element of the `UString` by index or key.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the selected element or a reference/proxy referring to it.
        /// @pre `scalar_index < scalar_size_`; debug builds assert if this precondition is violated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] char32_t operator[](usize scalar_index) const noexcept;

        /// Accesses an element of the `UString` by index or key.
        ///
        /// @param range Range of values to process.
        ///
        /// @return Returns the selected element or a reference/proxy referring to it.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] ustr operator[](USlice range) const &;
        /// Accesses an element of the `UString` by index or key.
        ///
        /// @param range Range of values to process.
        ///
        /// @return Returns the selected element or a reference/proxy referring to it.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        [[nodiscard]] ustr operator[](USlice range) const && = delete;
        /// Accesses an element of the `UString` by index or key.
        ///
        /// @param pattern `pattern` value used by the operation.
        ///
        /// @return Returns the selected element or a reference/proxy referring to it.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString operator[](USlicePattern pattern) const;

        /// Assigns the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &assign(string_view text);

        /// Assigns the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &assign(u8string_view text);

        /// Assigns the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &assign(const ustr &text);

        /// Assigns the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @throws `invalid_argument` if `text == nullptr`.
        UString &assign(const char *text);

        /// Assigns range using the supplied arguments and current state.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <std::ranges::input_range Range>
            requires std::convertible_to<std::ranges::range_reference_t<Range>, char>
        UString &assign_range(Range &&range) {
            UString replacement;
            replacement.append_range(std::forward<Range>(range));
            swap(replacement);
            return *this;
        }

        /// Assigns codepoints using the supplied arguments and current state.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <std::ranges::input_range Range>
            requires std::convertible_to<std::ranges::range_reference_t<Range>, char32_t>
        UString &assign_codepoints(Range &&range) {
            UString replacement;
            replacement.append_codepoints(std::forward<Range>(range));
            swap(replacement);
            return *this;
        }

        /// Clears the stored state or contents.
        ///
        /// @note This function does not throw exceptions.
        void clear() noexcept;

        /// Reserves storage for at least the requested capacity without changing the logical contents.
        ///
        /// @param requested_capacity `requested_capacity` value used by the operation.
        ///
        /// @throws `length_error` if `requested_capacity > max_size()`.
        void reserve(usize requested_capacity);

        /// Performs the shrink to fit operation for `UString` using the supplied arguments.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void shrink_to_fit();

        /// Appends the supplied value or range to the current contents.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &append(const UString &text);

        /// Appends the supplied value or range to the current contents.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &append(const ustr &text);

        /// Appends the supplied value or range to the current contents.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &append(string_view text);

        /// Appends the supplied value or range to the current contents.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &append(u8string_view text);

        /// Appends the supplied value or range to the current contents.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @throws `invalid_argument` if `text == nullptr`.
        UString &append(const char *text);

        /// Appends the supplied value or range to the current contents.
        ///
        /// @param scalar `scalar` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &append(char32_t scalar);

        /// Appends the supplied value or range to the current contents.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

        /// Appends the supplied value or range to the current contents.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <std::ranges::input_range Range>
            requires std::convertible_to<std::ranges::range_reference_t<Range>, char32_t>
        UString &append_codepoints(Range &&range) {
            UString pending;
            for (auto &&value : range) {
                pending.append(static_cast<char32_t>(value));
            }
            return append(pending);
        }

        /// Adds the supplied value to the end or work queue.
        ///
        /// @param scalar `scalar` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &push_back(char32_t scalar);

        /// Removes and returns or discards the next value from the container or queue.
        ///
        /// @throws `out_of_range` if `empty()`.
        void pop_back();

        /// Adds the right-hand value to this object in place.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &operator+=(const UString &text);

        /// Adds the right-hand value to this object in place.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &operator+=(const ustr &text);

        /// Adds the right-hand value to this object in place.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &operator+=(string_view text);

        /// Adds the right-hand value to this object in place.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &operator+=(const char *text);

        /// Adds the right-hand value to this object in place.
        ///
        /// @param scalar `scalar` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &operator+=(char32_t scalar);

        /// Inserts the supplied value or range at the requested position.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &insert(usize scalar_index, const UString &text);

        /// Inserts the supplied value or range at the requested position.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &insert(usize scalar_index, const ustr &text);

        /// Inserts the supplied value or range at the requested position.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &insert(usize scalar_index, string_view text);

        /// Inserts the supplied value or range at the requested position.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &insert(usize scalar_index, u8string_view text);

        /// Inserts the supplied value or range at the requested position.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        /// @param scalar `scalar` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &insert(usize scalar_index, char32_t scalar);

        /// Erases the selected element or range from the container.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        /// @param scalar_count Number of elements or operations to process.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @throws `out_of_range` if `scalar_index > scalar_size_`.
        UString &erase(usize scalar_index = 0, usize scalar_count = npos);

        /// Replaces the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        /// @param scalar_count Number of elements or operations to process.
        /// @param replacement `replacement` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &replace(usize scalar_index, usize scalar_count, const UString &replacement);

        /// Replaces the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        /// @param scalar_count Number of elements or operations to process.
        /// @param replacement `replacement` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &replace(usize scalar_index, usize scalar_count, const ustr &replacement);

        /// Replaces the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        /// @param scalar_count Number of elements or operations to process.
        /// @param replacement `replacement` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &replace(usize scalar_index, usize scalar_count, string_view replacement);

        /// Replaces the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        /// @param scalar_count Number of elements or operations to process.
        /// @param replacement `replacement` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &replace(usize scalar_index, usize scalar_count, u8string_view replacement);

        /// Replaces all using the supplied arguments and current state.
        ///
        /// @param needle `needle` value used by the operation.
        /// @param replacement `replacement` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @throws `invalid_argument` if `needle.empty()`.
        UString &replace_all(const ustr &needle, const ustr &replacement);

        /// Replaces all using the supplied arguments and current state.
        ///
        /// @param needle `needle` value used by the operation.
        /// @param replacement `replacement` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @throws `invalid_argument` if `needle.empty()`.
        UString &replace_all(const UString &needle, const UString &replacement);

        /// Changes the logical size to the requested value, creating or removing elements as needed.
        ///
        /// @param requested_scalar_size Requested or available size for the operation.
        /// @param fill `fill` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &resize(usize requested_scalar_size, char32_t fill = U' ');

        /// Performs the substr operation for `UString` using the supplied arguments.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        /// @param scalar_count Number of elements or operations to process.
        ///
        /// @return Returns the value produced by the operation.
        /// @throws `out_of_range` if `scalar_index > scalar_size_`.
        [[nodiscard]] UString substr(usize scalar_index = 0, usize scalar_count = npos) const;

        /// Finds the requested entry in the available state.
        ///
        /// @param needle `needle` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize find(const UString &needle, usize scalar_position = 0) const;

        /// Finds the requested entry in the available state.
        ///
        /// @param needle `needle` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize find(const ustr &needle, usize scalar_position = 0) const;

        /// Finds the requested entry in the available state.
        ///
        /// @param needle `needle` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize find(string_view needle, usize scalar_position = 0) const;

        /// Finds the requested entry in the available state.
        ///
        /// @param needle `needle` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize find(u8string_view needle, usize scalar_position = 0) const;

        /// Finds the last matching occurrence in the available range.
        ///
        /// @param needle `needle` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize rfind(const UString &needle, usize scalar_position = npos) const;

        /// Finds the last matching occurrence in the available range.
        ///
        /// @param needle `needle` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize rfind(const ustr &needle, usize scalar_position = npos) const;

        /// Finds the last matching occurrence in the available range.
        ///
        /// @param needle `needle` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize rfind(string_view needle, usize scalar_position = npos) const;

        /// Finds the last matching occurrence in the available range.
        ///
        /// @param needle `needle` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize rfind(u8string_view needle, usize scalar_position = npos) const;

        /// Reports whether contains holds for this `UString`.
        ///
        /// @param needle `needle` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool contains(const UString &needle) const noexcept;

        /// Reports whether contains holds for this `UString`.
        ///
        /// @param needle `needle` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool contains(const ustr &needle) const noexcept;

        /// Reports whether contains holds for this `UString`.
        ///
        /// @param needle `needle` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool contains(string_view needle) const;

        /// Reports whether contains holds for this `UString`.
        ///
        /// @param needle `needle` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool contains(u8string_view needle) const;

        /// Reports whether contains holds for this `UString`.
        ///
        /// @param scalar `scalar` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool contains(char32_t scalar) const;

        /// Reports whether the value begins with the supplied prefix.
        ///
        /// @param prefix `prefix` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool starts_with(const UString &prefix) const noexcept;

        /// Reports whether the value begins with the supplied prefix.
        ///
        /// @param prefix `prefix` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool starts_with(const ustr &prefix) const noexcept;

        /// Reports whether the value begins with the supplied prefix.
        ///
        /// @param prefix `prefix` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool starts_with(string_view prefix) const;

        /// Reports whether the value begins with the supplied prefix.
        ///
        /// @param prefix `prefix` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool starts_with(u8string_view prefix) const;

        /// Reports whether the value ends with the supplied suffix.
        ///
        /// @param suffix `suffix` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool ends_with(const UString &suffix) const noexcept;

        /// Reports whether the value ends with the supplied suffix.
        ///
        /// @param suffix `suffix` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool ends_with(const ustr &suffix) const noexcept;

        /// Reports whether the value ends with the supplied suffix.
        ///
        /// @param suffix `suffix` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool ends_with(string_view suffix) const;

        /// Reports whether the value ends with the supplied suffix.
        ///
        /// @param suffix `suffix` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool ends_with(u8string_view suffix) const;

        /// Finds first of in the available state.
        ///
        /// @param scalars `scalars` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @throws `out_of_range` if `scalar_position > scalar_size_`.
        [[nodiscard]] usize find_first_of(const UString &scalars, usize scalar_position = 0) const;

        /// Finds first of in the available state.
        ///
        /// @param scalars `scalars` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize find_first_of(string_view scalars, usize scalar_position = 0) const;

        /// Finds first not of in the available state.
        ///
        /// @param scalars `scalars` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @throws `out_of_range` if `scalar_position > scalar_size_`.
        [[nodiscard]] usize find_first_not_of(const UString &scalars, usize scalar_position = 0) const;

        /// Finds first not of in the available state.
        ///
        /// @param scalars `scalars` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize find_first_not_of(string_view scalars, usize scalar_position = 0) const;

        /// Finds last of in the available state.
        ///
        /// @param scalars `scalars` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize find_last_of(const UString &scalars, usize scalar_position = npos) const;

        /// Finds last of in the available state.
        ///
        /// @param scalars `scalars` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize find_last_of(string_view scalars, usize scalar_position = npos) const;

        /// Finds last not of in the available state.
        ///
        /// @param scalars `scalars` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize find_last_not_of(const UString &scalars, usize scalar_position = npos) const;

        /// Finds last not of in the available state.
        ///
        /// @param scalars `scalars` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize find_last_not_of(string_view scalars, usize scalar_position = npos) const;

        /// Reports whether codepoint boundary holds for this `UString`.
        ///
        /// @param byte_index Zero-based index of the target element or entry.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_codepoint_boundary(usize byte_index) const noexcept;

        /// Performs the byte index of operation for `UString` using the supplied arguments.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @throws `out_of_range` if `scalar_index > scalar_size_`.
        [[nodiscard]] usize byte_index_of(usize scalar_index) const;

        /// Performs the scalar index of byte operation for `UString` using the supplied arguments.
        ///
        /// @param byte_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @throws `out_of_range` if `!is_codepoint_boundary(byte_index)`.
        [[nodiscard]] usize scalar_index_of_byte(usize byte_index) const;

        /// Compares the supplied values and returns their relative ordering or match result.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] int compare(const UString &other) const noexcept;

        /// Compares the supplied values and returns their relative ordering or match result.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] int compare(const ustr &other) const noexcept;

        /// Compares the supplied values and returns their relative ordering or match result.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] int compare(string_view other) const;

        /// Exchanges this object state with the supplied object.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void swap(UString &other) noexcept;

        /// Compares the operands for equality.
        ///
        /// @param lhs Left-hand operand.
        /// @param rhs Right-hand operand.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        friend bool operator==(const UString &lhs, const UString &rhs) noexcept;

        /// Compares the operands and produces their ordering.
        ///
        /// @param lhs Left-hand operand.
        /// @param rhs Right-hand operand.
        ///
        /// @return Returns the comparison category describing the ordering of the operands.
        /// @note This function does not throw exceptions.
        friend strong_ordering operator<=>(const UString &lhs, const UString &rhs) noexcept;

        /// Adds the operands and returns the result.
        ///
        /// @param lhs Left-hand operand.
        /// @param rhs Right-hand operand.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        friend UString operator+(UString lhs, const UString &rhs);

        /// Adds the operands and returns the result.
        ///
        /// @param lhs Left-hand operand.
        /// @param rhs Right-hand operand.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        friend UString operator+(UString lhs, string_view rhs);

        /// Adds the operands and returns the result.
        ///
        /// @param lhs Left-hand operand.
        /// @param rhs Right-hand operand.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        friend UString operator+(string_view lhs, const UString &rhs);

        /// Adds the operands and returns the result.
        ///
        /// @param lhs Left-hand operand.
        /// @param rhs Right-hand operand.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        friend UString operator+(UString lhs, char32_t rhs);

      private:
        struct ValidatedInput {};

        static constexpr u32 max_unicode_scalar = 0x10FFFF;

        /// Constructs a `UString` from the supplied initialization values.
        ///
        /// @param text Text consumed by the operation.
        /// @param scalar_count Number of elements or operations to process.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString(string_view text, usize scalar_count, ValidatedInput);

        /// Reports whether continuation byte holds for this `UString`.
        ///
        /// @param byte `byte` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr bool is_continuation_byte(unsigned char byte) noexcept {
            return (byte & 0xC0u) == 0x80u;
        }

        /// Reports whether surrogate holds for this `UString`.
        ///
        /// @param scalar `scalar` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr bool is_surrogate(u32 scalar) noexcept {
            return scalar >= 0xD800 && scalar <= 0xDFFF;
        }

        /// Performs the failure operation for `UString` using the supplied arguments.
        ///
        /// @param error Error value describing the failure.
        /// @param byte_index Zero-based index of the target element or entry.
        /// @param scalar_count Number of elements or operations to process.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr UStringValidation failure(UStringValidationError error, usize byte_index, usize scalar_count) noexcept {
            return UStringValidation{.valid = false, .error = error, .byte_index = byte_index, .scalar_count = scalar_count};
        }

        /// Performs the as char view operation for `UString` using the supplied arguments.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static string_view as_char_view(u8string_view text) noexcept;

        /// Validates or throw.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @throws `invalid_argument` if `!validation`.
        static UStringValidation validate_or_throw(string_view text);

        /// Validates scalar or throw.
        ///
        /// @param scalar `scalar` value used by the operation.
        ///
        /// @throws `invalid_argument` if `codepoint == 0`.
        /// @throws `invalid_argument` if `is_surrogate(codepoint)`.
        /// @throws `invalid_argument` if `codepoint > max_unicode_scalar`.
        static void validate_scalar_or_throw(char32_t scalar);

        /// Encodes scalar or throw into the target representation.
        ///
        /// @param scalar `scalar` value used by the operation.
        /// @param buffer Buffer used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static usize encode_scalar_or_throw(char32_t scalar, char *buffer);

        /// Encodes the supplied value into the target representation.
        ///
        /// @param lead `lead` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static usize encoded_length_from_lead(unsigned char lead) noexcept;

        /// Encodes the supplied value into the target representation.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static usize encoded_length_unchecked(const char *text) noexcept;

        /// Decodes unchecked.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static char32_t decode_unchecked(const char *text) noexcept;

        /// Performs the previous codepoint operation for `UString` using the supplied arguments.
        ///
        /// @param begin First position or element included in the operation.
        /// @param cursor `cursor` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static const char *previous_codepoint(const char *begin, const char *cursor) noexcept;

        /// Allocates buffer.
        ///
        /// @param capacity `capacity` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @throws `length_error` if `capacity > max_size() || capacity == std::numeric_limits<usize>::max()`.
        [[nodiscard]] static char *allocate_buffer(usize capacity);

        /// Returns the storage data associated with this `UString`.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const char *storage_data() const noexcept;

        /// Returns the mutable data associated with this `UString`.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] char *mutable_data() noexcept;

        /// Performs the overlaps storage operation for `UString` using the supplied arguments.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool overlaps_storage(string_view text) const noexcept;

        /// Moves from using the supplied arguments and current state.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void move_from(UString &&other) noexcept;

        /// Assigns validated unaliased using the supplied arguments and current state.
        ///
        /// @param text Text consumed by the operation.
        /// @param scalar_count Number of elements or operations to process.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void assign_validated_unaliased(string_view text, usize scalar_count);

        /// Finds or creates the capacity required by the operation.
        ///
        /// @param requested_capacity `requested_capacity` value used by the operation.
        ///
        /// @throws `length_error` if `requested_capacity > max_size()`.
        void ensure_capacity(usize requested_capacity);

        /// Returns the checked total byte size for this `UString`.
        ///
        /// @param additional_bytes `additional_bytes` value used by the operation.
        ///
        /// @return Returns the requested count or size.
        /// @throws `length_error` if `additional_bytes > max_size() - byte_size_`.
        [[nodiscard]] usize checked_total_byte_size(usize additional_bytes) const;

        /// Appends the supplied value or range to the current contents.
        ///
        /// @param text Text consumed by the operation.
        /// @param scalar_count Number of elements or operations to process.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UString &append_validated(string_view text, usize scalar_count);

        /// Appends the supplied value or range to the current contents.
        ///
        /// @param text Text consumed by the operation.
        /// @param scalar_count Number of elements or operations to process.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void append_validated_unaliased(string_view text, usize scalar_count);

        /// Appends the supplied value or range to the current contents.
        ///
        /// @param encoded `encoded` value used by the operation.
        /// @param encoded_size Requested or available size for the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void append_encoded_scalar(const char *encoded, usize encoded_size);

        /// Inserts the supplied value or range at the requested position.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        /// @param text Text consumed by the operation.
        /// @param scalar_count Number of elements or operations to process.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @throws `out_of_range` if `scalar_index > scalar_size_`.
        UString &insert_validated(usize scalar_index, string_view text, usize scalar_count);

        /// Replaces validated using the supplied arguments and current state.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        /// @param scalar_count Number of elements or operations to process.
        /// @param replacement `replacement` value used by the operation.
        /// @param replacement_scalar_count Number of elements or operations to process.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @throws `out_of_range` if `scalar_index > scalar_size_`.
        /// @throws `length_error` if `replacement.size() > max_size() - preserved_bytes`.
        UString &replace_validated(usize scalar_index, usize scalar_count, string_view replacement, usize replacement_scalar_count);

        /// Performs the byte index of unchecked operation for `UString` using the supplied arguments.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize byte_index_of_unchecked(usize scalar_index) const noexcept;

        /// Performs the scalar index of byte unchecked operation for `UString` using the supplied arguments.
        ///
        /// @param byte_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize scalar_index_of_byte_unchecked(usize byte_index) const noexcept;

        /// Performs the iterator at unchecked operation for `UString` using the supplied arguments.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] CodepointIterator iterator_at_unchecked(usize scalar_index) const noexcept;

        /// Reports whether scalar unchecked holds for this `UString`.
        ///
        /// @param scalar `scalar` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool contains_scalar_unchecked(char32_t scalar) const noexcept;

        /// Finds validated in the available state.
        ///
        /// @param needle `needle` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @throws `out_of_range` if `scalar_position > scalar_size_`.
        [[nodiscard]] usize find_validated(string_view needle, usize scalar_position) const;

        /// Finds the last matching occurrence in the available range.
        ///
        /// @param needle `needle` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize rfind_validated(string_view needle, usize scalar_position) const;

        usize byte_size_ = 0;
        usize scalar_size_ = 0;
        usize capacity_ = sso_capacity;
        char *heap_ = nullptr;
        array<char, sso_capacity + 1> small_{};
    };

    /// Exchanges this object state with the supplied object.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @note This function does not throw exceptions.
    void swap(UString &lhs, UString &rhs) noexcept;


    class ustr final {
      public:
        using value_type = char32_t;
        using size_type = usize;
        using difference_type = isize;
        using CodepointIterator = UString::CodepointIterator;
        using CodepointView = UString::CodepointView;

        static constexpr usize npos = UString::npos;

        /// Constructs a `ustr` in its default state.
        ///
        /// @note This function does not throw exceptions.
        ustr() noexcept = default;
        /// Disables this construction form for `ustr`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ustr(std::nullptr_t) = delete;
        /// Disables this construction form for `ustr`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ustr(const ustr &) = delete;
        /// Disables this construction form for `ustr`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ustr(ustr &&) = delete;
        /// Assigns a new value to this `ustr`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ustr &operator=(const ustr &) = delete;
        /// Assigns a new value to this `ustr`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ustr &operator=(ustr &&) = delete;

        /// Constructs a `ustr` from the supplied initialization values.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @throws `invalid_argument` if `text == nullptr`.
        ustr(const char *text);

        /// Constructs a `ustr` from the supplied initialization values.
        ///
        /// @param text Text consumed by the operation.
        /// @param byte_count Number of elements or operations to process.
        ///
        /// @throws `invalid_argument` if `text == nullptr && byte_count != 0`.
        ustr(const char *text, usize byte_count);

        /// Constructs a `ustr` from the supplied initialization values.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        ustr(string_view text);

        /// Constructs a `ustr` from the supplied initialization values.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        ustr(const string &text);

        /// Disables this construction form for `ustr`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ustr(string &&) = delete;
        /// Disables this construction form for `ustr`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ustr(const string &&) = delete;

        /// Constructs a `ustr` from the supplied initialization values.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        ustr(u8string_view text);

        /// Constructs a `ustr` from the supplied initialization values.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit ustr(const UString &text) noexcept;

        /// Disables this construction form for `ustr`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ustr(UString &&) = delete;
        /// Disables this construction form for `ustr`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ustr(const UString &&) = delete;

        /// Validates UTF-8.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static UStringValidation validate_utf8(string_view text) noexcept;

        /// Reports whether valid UTF-8 holds for this `ustr`.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static bool is_valid_utf8(string_view text) noexcept;


        /// Creates or converts a value from c str representation.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param max_bytes `max_bytes` value used by the operation.
        ///
        /// @return Returns the newly constructed or converted value.
        /// @throws `invalid_argument` if `buffer == nullptr && max_bytes != 0`.
        [[nodiscard]] static ustr from_c_str(const char *buffer, usize max_bytes);

        /// Returns the data associated with this `ustr`.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const char *data() const noexcept;


        /// Returns the c str associated with this `ustr`.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        const char *c_str() const = delete;

        /// Returns the current or globally available C++ string view value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] string_view cpp_string_view() const noexcept;

        /// Computes the C++ bytes required by the supplied values.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] string_view cpp_bytes() const noexcept;

        /// Returns the current or globally available C++ string value.
        ///
        /// @return Returns the current C++ string value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] string cpp_string() const;

        /// Converts the value to std string representation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `TextConversionError::NonAscii`.
        [[nodiscard]] expected<string, TextConversionError> to_std_string() const;

        /// Converts the value to std string unchecked representation.
        ///
        /// @return Returns the current to std string unchecked value.
        /// @note Terminates the process if an invariant required by this unchecked operation is violated.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] string to_std_string_unchecked() const;

        /// Converts the value to owned representation.
        ///
        /// @return Returns the current to owned value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString to_owned() const;


        /// Returns the current or globally available C++ u8string view value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u8string_view cpp_u8string_view() const noexcept;

        /// Returns the current or globally available C++ u8string value.
        ///
        /// @return Returns the current C++ u8string value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] u8string cpp_u8string() const;


        /// Returns the current or globally available C++ u16string value.
        ///
        /// @return Returns the current C++ u16string value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] u16string cpp_u16string() const;
        /// Returns the current or globally available C++ u32string value.
        ///
        /// @return Returns the current C++ u32string value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] u32string cpp_u32string() const;
        /// Returns the current or globally available C++ wstring value.
        ///
        /// @return Returns the current C++ wstring value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] wstring cpp_wstring() const;

        /// Converts the `ustr` to `basic_string_view`.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] operator string_view() const noexcept;

        /// Reports whether this `ustr` contains no elements or payload.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool empty() const noexcept;

        /// Reports whether ascii holds for this `ustr`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_ascii() const noexcept;

        /// Returns the size for this `ustr`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        /// Returns the size for this `ustr`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize size() const noexcept;

        /// Returns the length for this `ustr`.
        ///
        /// @return Returns the current length value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize length() const noexcept;

        /// Returns the scalar size for this `ustr`.
        ///
        /// @return Returns the current scalar size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize scalar_size() const noexcept;

        /// Returns the codepoint size for this `ustr`.
        ///
        /// @return Returns the current codepoint size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize codepoint_size() const noexcept;

        /// Returns the byte size for this `ustr`.
        ///
        /// @return Returns the current byte size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize byte_size() const noexcept;

        /// Computes the size bytes required by the supplied values.
        ///
        /// @return Returns the current size bytes value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize size_bytes() const noexcept;

        /// Returns an iterator to the first element in the range.
        ///
        /// @return Returns an iterator referring to the first element.
        /// @note This function does not throw exceptions.
        [[nodiscard]] CodepointIterator begin() const noexcept;

        /// Returns the one-past-the-end iterator for the range.
        ///
        /// @return Returns the one-past-the-end iterator.
        /// @note This function does not throw exceptions.
        [[nodiscard]] CodepointIterator end() const noexcept;

        /// Returns the current or globally available codepoints value.
        ///
        /// @return Returns the current codepoints value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] CodepointView codepoints() const noexcept;

        /// Returns the current or globally available byte begin value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const char *byte_begin() const noexcept;

        /// Returns the current or globally available byte end value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const char *byte_end() const noexcept;

        /// Returns the current or globally available front value.
        ///
        /// @return Returns the current front value.
        /// @throws `out_of_range` if `empty()`.
        [[nodiscard]] char32_t front() const;

        /// Returns the current or globally available back value.
        ///
        /// @return Returns the current back value.
        /// @throws `out_of_range` if `empty()`.
        [[nodiscard]] char32_t back() const;

        /// Performs the at operation for `ustr` using the supplied arguments.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @throws `out_of_range` if `scalar_index >= scalar_size_`.
        [[nodiscard]] char32_t at(usize scalar_index) const;

        /// Accesses an element of the `ustr` by index or key.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the selected element or a reference/proxy referring to it.
        /// @pre `scalar_index < scalar_size_`; debug builds assert if this precondition is violated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] char32_t operator[](usize scalar_index) const noexcept;

        /// Accesses an element of the `ustr` by index or key.
        ///
        /// @param range Range of values to process.
        ///
        /// @return Returns the selected element or a reference/proxy referring to it.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] ustr operator[](USlice range) const;

        /// Accesses an element of the `ustr` by index or key.
        ///
        /// @param pattern `pattern` value used by the operation.
        ///
        /// @return Returns the selected element or a reference/proxy referring to it.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString operator[](USlicePattern pattern) const;

        /// Performs the slice operation for `ustr` using the supplied arguments.
        ///
        /// @param scalar_start `scalar_start` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] ustr slice(usize scalar_start) const;

        /// Performs the slice operation for `ustr` using the supplied arguments.
        ///
        /// @param scalar_start `scalar_start` value used by the operation.
        /// @param scalar_end `scalar_end` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] ustr slice(usize scalar_start, usize scalar_end) const;

        /// Performs the slice operation for `ustr` using the supplied arguments.
        ///
        /// @param range Range of values to process.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] ustr slice(USlice range) const;

        /// Performs the slice operation for `ustr` using the supplied arguments.
        ///
        /// @param pattern `pattern` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString slice(USlicePattern pattern) const;

        /// Performs the substr operation for `ustr` using the supplied arguments.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        /// @param scalar_count Number of elements or operations to process.
        ///
        /// @return Returns the value produced by the operation.
        /// @throws `out_of_range` if `scalar_index > scalar_size_`.
        [[nodiscard]] ustr substr(usize scalar_index = 0, usize scalar_count = npos) const;

        /// Finds the requested entry in the available state.
        ///
        /// @param needle `needle` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @throws `out_of_range` if `scalar_position > scalar_size_`.
        [[nodiscard]] usize find(const ustr &needle, usize scalar_position = 0) const;

        /// Finds the requested entry in the available state.
        ///
        /// @param needle `needle` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize find(string_view needle, usize scalar_position = 0) const;

        /// Finds the requested entry in the available state.
        ///
        /// @param needle `needle` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize find(u8string_view needle, usize scalar_position = 0) const;

        /// Finds the last matching occurrence in the available range.
        ///
        /// @param needle `needle` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize rfind(const ustr &needle, usize scalar_position = npos) const;

        /// Finds the last matching occurrence in the available range.
        ///
        /// @param needle `needle` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize rfind(string_view needle, usize scalar_position = npos) const;

        /// Finds the last matching occurrence in the available range.
        ///
        /// @param needle `needle` value used by the operation.
        /// @param scalar_position `scalar_position` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] usize rfind(u8string_view needle, usize scalar_position = npos) const;

        /// Reports whether contains holds for this `ustr`.
        ///
        /// @param needle `needle` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool contains(const ustr &needle) const noexcept;

        /// Reports whether contains holds for this `ustr`.
        ///
        /// @param needle `needle` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool contains(string_view needle) const;

        /// Reports whether contains holds for this `ustr`.
        ///
        /// @param needle `needle` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool contains(u8string_view needle) const;

        /// Reports whether contains holds for this `ustr`.
        ///
        /// @param scalar `scalar` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool contains(char32_t scalar) const;

        /// Reports whether the value begins with the supplied prefix.
        ///
        /// @param prefix `prefix` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool starts_with(const ustr &prefix) const noexcept;

        /// Reports whether the value begins with the supplied prefix.
        ///
        /// @param prefix `prefix` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool starts_with(string_view prefix) const;

        /// Reports whether the value begins with the supplied prefix.
        ///
        /// @param prefix `prefix` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool starts_with(u8string_view prefix) const;

        /// Reports whether the value ends with the supplied suffix.
        ///
        /// @param suffix `suffix` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool ends_with(const ustr &suffix) const noexcept;

        /// Reports whether the value ends with the supplied suffix.
        ///
        /// @param suffix `suffix` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool ends_with(string_view suffix) const;

        /// Reports whether the value ends with the supplied suffix.
        ///
        /// @param suffix `suffix` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool ends_with(u8string_view suffix) const;

        /// Reports whether codepoint boundary holds for this `ustr`.
        ///
        /// @param byte_index Zero-based index of the target element or entry.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_codepoint_boundary(usize byte_index) const noexcept;

        /// Performs the byte index of operation for `ustr` using the supplied arguments.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @throws `out_of_range` if `scalar_index > scalar_size_`.
        [[nodiscard]] usize byte_index_of(usize scalar_index) const;

        /// Performs the scalar index of byte operation for `ustr` using the supplied arguments.
        ///
        /// @param byte_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @throws `out_of_range` if `!is_codepoint_boundary(byte_index)`.
        [[nodiscard]] usize scalar_index_of_byte(usize byte_index) const;

        /// Compares the supplied values and returns their relative ordering or match result.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] int compare(const ustr &other) const noexcept;

        /// Compares the supplied values and returns their relative ordering or match result.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] int compare(string_view other) const;

        /// Compares the operands for equality.
        ///
        /// @param lhs Left-hand operand.
        /// @param rhs Right-hand operand.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        friend bool operator==(const ustr &lhs, const ustr &rhs) noexcept;

        /// Compares the operands and produces their ordering.
        ///
        /// @param lhs Left-hand operand.
        /// @param rhs Right-hand operand.
        ///
        /// @return Returns the comparison category describing the ordering of the operands.
        /// @note This function does not throw exceptions.
        friend strong_ordering operator<=>(const ustr &lhs, const ustr &rhs) noexcept;

      private:
        friend class UString;

        struct ValidatedInput {};

        /// Constructs a `ustr` from the supplied initialization values.
        ///
        /// @param text Text consumed by the operation.
        /// @param scalar_count Number of elements or operations to process.
        ///
        /// @note This function does not throw exceptions.
        constexpr ustr(string_view text, usize scalar_count, ValidatedInput) noexcept
            : bytes_(text), scalar_size_(scalar_count) {
        }

        /// Assigns validated using the supplied arguments and current state.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void assign_validated(string_view text);

        /// Performs the as char view operation for `ustr` using the supplied arguments.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static string_view as_char_view(u8string_view text) noexcept;

        /// Validates or throw.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @throws `invalid_argument` if `!validation`.
        static UStringValidation validate_or_throw(string_view text);

        /// Reports whether continuation byte holds for this `ustr`.
        ///
        /// @param byte `byte` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr bool is_continuation_byte(unsigned char byte) noexcept {
            return (byte & 0xC0u) == 0x80u;
        }

        /// Validates scalar or throw.
        ///
        /// @param scalar `scalar` value used by the operation.
        ///
        /// @throws `invalid_argument` if `codepoint == 0`.
        /// @throws `invalid_argument` if `codepoint >= 0xD800 && codepoint <= 0xDFFF`.
        /// @throws `invalid_argument` if `codepoint > 0x10FFFF`.
        static void validate_scalar_or_throw(char32_t scalar);

        /// Encodes the supplied value into the target representation.
        ///
        /// @param lead `lead` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static usize encoded_length_from_lead(unsigned char lead) noexcept;

        /// Encodes the supplied value into the target representation.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static usize encoded_length_unchecked(const char *text) noexcept;

        /// Decodes unchecked.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static char32_t decode_unchecked(const char *text) noexcept;

        /// Performs the previous codepoint operation for `ustr` using the supplied arguments.
        ///
        /// @param begin First position or element included in the operation.
        /// @param cursor `cursor` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static const char *previous_codepoint(const char *begin, const char *cursor) noexcept;

        /// Performs the byte index of unchecked operation for `ustr` using the supplied arguments.
        ///
        /// @param scalar_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize byte_index_of_unchecked(usize scalar_index) const noexcept;

        /// Performs the scalar index of byte unchecked operation for `ustr` using the supplied arguments.
        ///
        /// @param byte_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize scalar_index_of_byte_unchecked(usize byte_index) const noexcept;

        /// Reports whether scalar unchecked holds for this `ustr`.
        ///
        /// @param scalar `scalar` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool contains_scalar_unchecked(char32_t scalar) const noexcept;

        string_view bytes_{};
        usize scalar_size_ = 0;
    };

    namespace Detail {


        /// Converts the value to UTF-32 representation.
        ///
        /// @param codepoints `codepoints` value used by the operation.
        ///
        /// @return Returns the value converted to UTF-32 representation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] u32string to_utf32(UString::CodepointView codepoints);

        /// Converts the value to UTF-16 representation.
        ///
        /// @param codepoints `codepoints` value used by the operation.
        ///
        /// @return Returns the value converted to UTF-16 representation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] u16string to_utf16(UString::CodepointView codepoints);

    } // namespace Detail


    /// Compares the operands for equality.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool operator==(const UString &lhs, const ustr &rhs) noexcept;

    /// Compares the operands for equality.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool operator==(const ustr &lhs, const UString &rhs) noexcept;

    /// Compares the operands and produces their ordering.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns the comparison category describing the ordering of the operands.
    /// @note This function does not throw exceptions.
    [[nodiscard]] strong_ordering operator<=>(const UString &lhs, const ustr &rhs) noexcept;

    /// Compares the operands and produces their ordering.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns the comparison category describing the ordering of the operands.
    /// @note This function does not throw exceptions.
    [[nodiscard]] strong_ordering operator<=>(const ustr &lhs, const UString &rhs) noexcept;

    /// Adds the operands and returns the result.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] UString operator+(UString lhs, const ustr &rhs);

    /// Adds the operands and returns the result.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] UString operator+(const ustr &lhs, const UString &rhs);

    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::ostream &operator<<(std::ostream &os, const UString &value);

    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::ostream &operator<<(std::ostream &os, const ustr &value);

    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::ostream &operator<<(std::ostream &os, USlice value);

    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::ostream &operator<<(std::ostream &os, USlicePattern value);

    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::ostream &operator<<(std::ostream &os, UStringValidationError value);

    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::ostream &operator<<(std::ostream &os, TextConversionError value);

    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::ostream &operator<<(std::ostream &os, const UStringValidation &value);

} // namespace SFT::Foundation

/// Implements `operator""_ustr` for `/mnt/data/SE_docpass/SturdyEngine5-Header-Cleaned/Foundation/src/UString.hpp`.
///
/// @param text Text consumed by the operation.
/// @param byte_count Number of elements or operations to process.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] SFT::Foundation::ustr operator""_ustr(const char *text, size_t byte_count);

/// Implements `operator""_ustr` for `/mnt/data/SE_docpass/SturdyEngine5-Header-Cleaned/Foundation/src/UString.hpp`.
///
/// @param text Text consumed by the operation.
/// @param byte_count Number of elements or operations to process.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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


template <>
struct std::formatter<SFT::Foundation::UString> : std::formatter<std::string_view> {
    /// Formats the supplied value into the provided formatting context.
    ///
    /// @return Returns the formatting context iterator/result produced by the underlying formatter.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    auto format(const SFT::Foundation::UString &value, auto &ctx) const {
        return std::formatter<std::string_view>::format(value.cpp_string_view(), ctx);
    }
};

template <>
struct std::formatter<SFT::Foundation::ustr> : std::formatter<std::string_view> {
    /// Formats the supplied value into the provided formatting context.
    ///
    /// @return Returns the formatting context iterator/result produced by the underlying formatter.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    auto format(const SFT::Foundation::ustr &value, auto &ctx) const {
        return std::formatter<std::string_view>::format(value.cpp_string_view(), ctx);
    }
};

template <>
struct std::formatter<SFT::Foundation::USlice> : std::formatter<std::string_view> {
    /// Formats the supplied value into the provided formatting context.
    ///
    /// @return Returns the formatting context iterator/result produced by the underlying formatter.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    auto format(SFT::Foundation::USlice value, auto &ctx) const {
        return std::formatter<std::string_view>::format(SFT::Foundation::Detail::display_string(value), ctx);
    }
};

template <>
struct std::formatter<SFT::Foundation::USlicePattern> : std::formatter<std::string_view> {
    /// Formats the supplied value into the provided formatting context.
    ///
    /// @return Returns the formatting context iterator/result produced by the underlying formatter.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    auto format(SFT::Foundation::USlicePattern value, auto &ctx) const {
        return std::formatter<std::string_view>::format(SFT::Foundation::Detail::display_string(value), ctx);
    }
};

template <>
struct std::formatter<SFT::Foundation::UStringValidationError> : std::formatter<std::string_view> {
    /// Formats the supplied value into the provided formatting context.
    ///
    /// @return Returns the formatting context iterator/result produced by the underlying formatter.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    auto format(SFT::Foundation::UStringValidationError value, auto &ctx) const {
        return std::formatter<std::string_view>::format(SFT::Foundation::to_string(value), ctx);
    }
};

template <>
struct std::formatter<SFT::Foundation::TextConversionError> : std::formatter<std::string_view> {
    /// Formats the supplied value into the provided formatting context.
    ///
    /// @return Returns the formatting context iterator/result produced by the underlying formatter.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    auto format(SFT::Foundation::TextConversionError value, auto &ctx) const {
        return std::formatter<std::string_view>::format(SFT::Foundation::to_string(value), ctx);
    }
};

template <>
struct std::formatter<SFT::Foundation::UStringValidation> : std::formatter<std::string_view> {
    /// Formats the supplied value into the provided formatting context.
    ///
    /// @return Returns the formatting context iterator/result produced by the underlying formatter.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    auto format(const SFT::Foundation::UStringValidation &value, auto &ctx) const {
        return std::formatter<std::string_view>::format(SFT::Foundation::Detail::display_string(value), ctx);
    }
};


template <>
struct fmt::formatter<SFT::Foundation::UString> : fmt::formatter<std::string_view> {
    /// Formats the supplied value into the provided formatting context.
    ///
    /// @param value Value consumed by the operation.
    /// @param ctx `ctx` value used by the operation.
    ///
    /// @return Returns the formatting context iterator/result produced by the underlying formatter.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    fmt::format_context::iterator format(const SFT::Foundation::UString &value, fmt::format_context &ctx) const;
};

template <>
struct fmt::formatter<SFT::Foundation::ustr> : fmt::formatter<std::string_view> {
    /// Formats the supplied value into the provided formatting context.
    ///
    /// @param value Value consumed by the operation.
    /// @param ctx `ctx` value used by the operation.
    ///
    /// @return Returns the formatting context iterator/result produced by the underlying formatter.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    fmt::format_context::iterator format(const SFT::Foundation::ustr &value, fmt::format_context &ctx) const;
};

template <>
struct fmt::formatter<SFT::Foundation::USlice> : fmt::formatter<std::string_view> {
    /// Formats the supplied value into the provided formatting context.
    ///
    /// @param value Value consumed by the operation.
    /// @param ctx `ctx` value used by the operation.
    ///
    /// @return Returns the formatting context iterator/result produced by the underlying formatter.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    fmt::format_context::iterator format(SFT::Foundation::USlice value, fmt::format_context &ctx) const;
};

template <>
struct fmt::formatter<SFT::Foundation::USlicePattern> : fmt::formatter<std::string_view> {
    /// Formats the supplied value into the provided formatting context.
    ///
    /// @param value Value consumed by the operation.
    /// @param ctx `ctx` value used by the operation.
    ///
    /// @return Returns the formatting context iterator/result produced by the underlying formatter.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    fmt::format_context::iterator format(SFT::Foundation::USlicePattern value, fmt::format_context &ctx) const;
};

template <>
struct fmt::formatter<SFT::Foundation::UStringValidationError> : fmt::formatter<std::string_view> {
    /// Formats the supplied value into the provided formatting context.
    ///
    /// @param value Value consumed by the operation.
    /// @param ctx `ctx` value used by the operation.
    ///
    /// @return Returns the formatting context iterator/result produced by the underlying formatter.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    fmt::format_context::iterator format(SFT::Foundation::UStringValidationError value, fmt::format_context &ctx) const;
};

template <>
struct fmt::formatter<SFT::Foundation::TextConversionError> : fmt::formatter<std::string_view> {
    /// Formats the supplied value into the provided formatting context.
    ///
    /// @param value Value consumed by the operation.
    /// @param ctx `ctx` value used by the operation.
    ///
    /// @return Returns the formatting context iterator/result produced by the underlying formatter.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    fmt::format_context::iterator format(SFT::Foundation::TextConversionError value, fmt::format_context &ctx) const;
};

template <>
struct fmt::formatter<SFT::Foundation::UStringValidation> : fmt::formatter<std::string_view> {
    /// Formats the supplied value into the provided formatting context.
    ///
    /// @param value Value consumed by the operation.
    /// @param ctx `ctx` value used by the operation.
    ///
    /// @return Returns the formatting context iterator/result produced by the underlying formatter.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    fmt::format_context::iterator format(const SFT::Foundation::UStringValidation &value, fmt::format_context &ctx) const;
};


template <>
struct std::hash<SFT::Foundation::UString> {
    /// Invokes the callable behavior provided by `hash`.
    ///
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] std::size_t operator()(const SFT::Foundation::UString &value) const noexcept;
};

template <>
struct std::hash<SFT::Foundation::ustr> {
    /// Invokes the callable behavior provided by `hash`.
    ///
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] std::size_t operator()(const SFT::Foundation::ustr &value) const noexcept;
};


static_assert(SFT::Foundation::Displayable<SFT::Foundation::UString>);
static_assert(SFT::Foundation::Displayable<SFT::Foundation::ustr>);
static_assert(SFT::Foundation::Displayable<SFT::Foundation::USlice>);
static_assert(SFT::Foundation::Displayable<SFT::Foundation::USlicePattern>);
static_assert(SFT::Foundation::Displayable<SFT::Foundation::UStringValidationError>);
static_assert(SFT::Foundation::Displayable<SFT::Foundation::UStringValidation>);
static_assert(SFT::Foundation::Displayable<SFT::Foundation::TextConversionError>);


static_assert(SFT::Foundation::Hashable<SFT::Foundation::UString>);
static_assert(SFT::Foundation::Hashable<SFT::Foundation::ustr>);
static_assert(noexcept(SFT::Foundation::UString{std::declval<const std::string &>()}));
