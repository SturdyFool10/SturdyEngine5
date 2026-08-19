#include <Foundation/UString.hpp>


namespace SFT::Foundation::Detail {

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
    ResolvedSlice resolve_slice(USlice slice, usize scalar_size, string_view owner) {
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

    /// Resolves slice into the concrete value used by the engine.
    ///
    /// @param slice `slice` value used by the operation.
    /// @param scalar_size Requested or available size for the operation.
    /// @param owner Owner/context identifier used for validation or diagnostics.
    ///
    /// @return Returns the value produced by the operation.
    /// @throws `invalid_argument` if `grouping == 0`.
    ResolvedSlicePattern resolve_slice(USlicePattern slice, usize scalar_size, string_view owner) {
        const ResolvedSlice range = resolve_slice(USlice{slice.start(), slice.end_or(USlice::npos)}, scalar_size, owner);
        const usize grouping = slice.grouping();
        if (grouping == 0) {
            throw invalid_argument{format("{} slice grouping must be greater than zero.", owner)};
        }

        return ResolvedSlicePattern{.range = range, .spread = slice.spread(), .grouping = grouping};
    }

    /// Performs the bounded c length operation for `Detail` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    /// @param max_bytes `max_bytes` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    usize bounded_c_length(const char *text, usize max_bytes) noexcept {
        if (text == nullptr || max_bytes == 0) {
            return 0;
        }
        const void *terminator = std::memchr(text, '\0', max_bytes);
        return terminator == nullptr ? max_bytes : static_cast<usize>(static_cast<const char *>(terminator) - text);
    }

    /// Performs the display string operation for `Detail` using the supplied arguments.
    ///
    /// @param slice `slice` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    string display_string(USlice slice) {
        if (slice.has_end()) {
            return format("USlice({}..{})", slice.start(), slice.end_or(0));
        }
        return format("USlice({}..)", slice.start());
    }

    /// Performs the display string operation for `Detail` using the supplied arguments.
    ///
    /// @param pattern `pattern` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    string display_string(USlicePattern pattern) {
        if (pattern.has_end()) {
            return format("USlicePattern({}..{}, group={}, spread={})", pattern.start(), pattern.end_or(0), pattern.grouping(), pattern.spread());
        }
        return format("USlicePattern({}.., group={}, spread={})", pattern.start(), pattern.grouping(), pattern.spread());
    }

    /// Performs the display string operation for `Detail` using the supplied arguments.
    ///
    /// @param validation `validation` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    string display_string(const UStringValidation &validation) {
        if (validation.valid) {
            return format("UStringValidation(valid, {} scalars)", validation.scalar_count);
        }
        return format("UStringValidation(invalid: {} at byte {}, {} scalars)", to_string(validation.error), validation.byte_index, validation.scalar_count);
    }

    /// Converts the value to UTF-32 representation.
    ///
    /// @param codepoints `codepoints` value used by the operation.
    ///
    /// @return Returns the value converted to UTF-32 representation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    u32string to_utf32(UString::CodepointView codepoints) {
        u32string result;
        result.reserve(codepoints.size());
        for (char32_t scalar : codepoints) {
            result.push_back(scalar);
        }
        return result;
    }

    /// Converts the value to UTF-16 representation.
    ///
    /// @param codepoints `codepoints` value used by the operation.
    ///
    /// @return Returns the value converted to UTF-16 representation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    u16string to_utf16(UString::CodepointView codepoints) {
        u16string result;
        result.reserve(codepoints.size());
        for (char32_t scalar : codepoints) {
            const auto value = static_cast<u32>(scalar);
            if (value <= 0xFFFF) {
                result.push_back(static_cast<char16_t>(value));
            } else {
                const u32 offset = value - 0x10000u;
                result.push_back(static_cast<char16_t>(0xD800u + (offset >> 10u)));
                result.push_back(static_cast<char16_t>(0xDC00u + (offset & 0x3FFu)));
            }
        }
        return result;
    }

} // namespace SFT::Foundation::Detail

namespace SFT::Foundation {

    /// Dereferences this iterator or handle.
    ///
    /// @return Returns the value or reference currently addressed by the iterator/handle.
    /// @pre `current_ != nullptr`; debug builds assert if this precondition is violated.
    /// @note This function does not throw exceptions.
    char32_t UString::CodepointIterator::operator*() const noexcept {
        assert(current_ != nullptr);
        return UString::decode_unchecked(current_);
    }

    /// Advances the `Foundation` to its next value or element.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @pre `current_ != nullptr`; debug builds assert if this precondition is violated.
    /// @note This function does not throw exceptions.
    UString::CodepointIterator &UString::CodepointIterator::operator++() noexcept {
        assert(current_ != nullptr);
        current_ += UString::encoded_length_unchecked(current_);
        return *this;
    }

    /// Advances the `Foundation` to its next value or element.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    UString::CodepointIterator UString::CodepointIterator::operator++(int) noexcept {
        CodepointIterator copy = *this;
        ++(*this);
        return copy;
    }

    /// Moves the `Foundation` to its previous value or element.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @pre `begin_ != nullptr`; debug builds assert if this precondition is violated.
    /// @pre `current_ != nullptr`; debug builds assert if this precondition is violated.
    /// @pre `current_ > begin_`; debug builds assert if this precondition is violated.
    /// @note This function does not throw exceptions.
    UString::CodepointIterator &UString::CodepointIterator::operator--() noexcept {
        assert(begin_ != nullptr);
        assert(current_ != nullptr);
        assert(current_ > begin_);
        do {
            --current_;
        } while (current_ > begin_ && UString::is_continuation_byte(static_cast<unsigned char>(*current_)));
        return *this;
    }

    /// Moves the `Foundation` to its previous value or element.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    UString::CodepointIterator UString::CodepointIterator::operator--(int) noexcept {
        CodepointIterator copy = *this;
        --(*this);
        return copy;
    }

    /// Compares the operands for equality.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    bool operator==(UString::CodepointIterator lhs, UString::CodepointIterator rhs) noexcept {
        return lhs.begin_ == rhs.begin_ && lhs.current_ == rhs.current_;
    }

    /// Assigns a new value to this `Foundation`.
    ///
    /// @param other Other object used by the operation.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @note This function does not throw exceptions.
    UString &UString::operator=(UString other) noexcept {
        swap(other);
        return *this;
    }

    /// Assigns a new value to this `Foundation`.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::operator=(string_view text) {
        return assign(text);
    }

    /// Assigns a new value to this `Foundation`.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @throws `invalid_argument` if `text == nullptr`.
    UString &UString::operator=(const char *text) {
        if (text == nullptr) {
            throw invalid_argument{"UString cannot be assigned from a null pointer."};
        }
        return assign(string_view{text});
    }

    /// Assigns a new value to this `Foundation`.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::operator=(u8string_view text) {
        return assign(as_char_view(text));
    }

    /// Validates UTF-8.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    UStringValidation UString::validate_utf8(string_view text) noexcept {
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

    /// Reports whether valid UTF-8 holds for this `Foundation`.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool UString::is_valid_utf8(string_view text) noexcept {
        return static_cast<bool>(validate_utf8(text));
    }

    /// Attempts to from UTF-8 without requiring normal failure to be exceptional.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    optional<UString> UString::try_from_utf8(string_view text) {
        const UStringValidation validation = validate_utf8(text);
        if (!validation) {
            return nullopt;
        }
        return UString{text, validation.scalar_count, ValidatedInput{}};
    }

    /// Attempts to from UTF-8 without requiring normal failure to be exceptional.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    optional<UString> UString::try_from_utf8(u8string_view text) {
        return try_from_utf8(as_char_view(text));
    }

    /// Returns the current or globally available C++ string value.
    ///
    /// @return Returns the current C++ string value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    string UString::cpp_string() const {
        return string{cpp_string_view()};
    }

    /// Converts the value to std string representation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `TextConversionError::NonAscii`.
    expected<string, TextConversionError> UString::to_std_string() const {
        if (!is_ascii()) {
            return unexpected(TextConversionError::NonAscii);
        }
        return string{cpp_string_view()};
    }

    /// Converts the value to std string unchecked representation.
    ///
    /// @return Returns the current to std string unchecked value.
    /// @note Terminates the process if an invariant required by this unchecked operation is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    string UString::to_std_string_unchecked() const {
        if (!is_ascii()) {
            std::terminate();
        }
        return string{cpp_string_view()};
    }

    /// Returns the current or globally available C++ u8string view value.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    u8string_view UString::cpp_u8string_view() const noexcept {
        return u8string_view{reinterpret_cast<const char8_t *>(storage_data()), byte_size_};
    }

    /// Returns the current or globally available C++ u8string value.
    ///
    /// @return Returns the current C++ u8string value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    u8string UString::cpp_u8string() const {
        return u8string{cpp_u8string_view()};
    }

    /// Returns the current or globally available C++ wstring value.
    ///
    /// @return Returns the current C++ wstring value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    wstring UString::cpp_wstring() const {
        if constexpr (sizeof(wchar_t) == 2) {
            const u16string units = cpp_u16string();
            return wstring{units.begin(), units.end()};
        } else {
            const u32string units = cpp_u32string();
            return wstring{units.begin(), units.end()};
        }
    }

    /// Assigns the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::assign(string_view text) {
        const UStringValidation validation = validate_or_throw(text);
        if (overlaps_storage(text)) {
            UString replacement{text, validation.scalar_count, ValidatedInput{}};
            swap(replacement);
            return *this;
        }
        assign_validated_unaliased(text, validation.scalar_count);
        return *this;
    }

    /// Assigns the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::assign(u8string_view text) {
        return assign(as_char_view(text));
    }

    /// Assigns the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @throws `invalid_argument` if `text == nullptr`.
    UString &UString::assign(const char *text) {
        if (text == nullptr) {
            throw invalid_argument{"UString cannot be assigned from a null pointer."};
        }
        return assign(string_view{text});
    }

    /// Appends the supplied value or range to the current contents.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::append(string_view text) {
        const UStringValidation validation = validate_or_throw(text);
        return append_validated(text, validation.scalar_count);
    }

    /// Appends the supplied value or range to the current contents.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::append(u8string_view text) {
        return append(as_char_view(text));
    }

    /// Appends the supplied value or range to the current contents.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @throws `invalid_argument` if `text == nullptr`.
    UString &UString::append(const char *text) {
        if (text == nullptr) {
            throw invalid_argument{"UString cannot append a null pointer."};
        }
        return append(string_view{text});
    }

    /// Appends the supplied value or range to the current contents.
    ///
    /// @param scalar `scalar` value used by the operation.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::append(char32_t scalar) {
        char buffer[4]{};
        const usize encoded_size = encode_scalar_or_throw(scalar, buffer);
        append_encoded_scalar(buffer, encoded_size);
        return *this;
    }

    /// Adds the right-hand value to this object in place.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::operator+=(string_view text) {
        return append(text);
    }

    /// Adds the right-hand value to this object in place.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::operator+=(const char *text) {
        return append(text);
    }

    /// Adds the right-hand value to this object in place.
    ///
    /// @param scalar `scalar` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::operator+=(char32_t scalar) {
        return append(scalar);
    }

    /// Inserts the supplied value or range at the requested position.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::insert(usize scalar_index, const UString &text) {
        return insert_validated(scalar_index, text.cpp_string_view(), text.scalar_size_);
    }

    /// Inserts the supplied value or range at the requested position.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::insert(usize scalar_index, string_view text) {
        const UStringValidation validation = validate_or_throw(text);
        return insert_validated(scalar_index, text, validation.scalar_count);
    }

    /// Inserts the supplied value or range at the requested position.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::insert(usize scalar_index, u8string_view text) {
        return insert(scalar_index, as_char_view(text));
    }

    /// Inserts the supplied value or range at the requested position.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    /// @param scalar `scalar` value used by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::insert(usize scalar_index, char32_t scalar) {
        char buffer[4]{};
        const usize encoded_size = encode_scalar_or_throw(scalar, buffer);
        return insert_validated(scalar_index, string_view{buffer, encoded_size}, 1);
    }

    /// Erases the selected element or range from the container.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    /// @param scalar_count Number of elements or operations to process.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @throws `out_of_range` if `scalar_index > scalar_size_`.
    UString &UString::erase(usize scalar_index, usize scalar_count) {
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

    /// Replaces the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    /// @param scalar_count Number of elements or operations to process.
    /// @param replacement `replacement` value used by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::replace(usize scalar_index, usize scalar_count, const UString &replacement) {
        return replace_validated(scalar_index, scalar_count, replacement.cpp_string_view(), replacement.scalar_size_);
    }

    /// Replaces the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    /// @param scalar_count Number of elements or operations to process.
    /// @param replacement `replacement` value used by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::replace(usize scalar_index, usize scalar_count, string_view replacement) {
        const UStringValidation validation = validate_or_throw(replacement);
        return replace_validated(scalar_index, scalar_count, replacement, validation.scalar_count);
    }

    /// Replaces the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    /// @param scalar_count Number of elements or operations to process.
    /// @param replacement `replacement` value used by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::replace(usize scalar_index, usize scalar_count, u8string_view replacement) {
        return replace(scalar_index, scalar_count, as_char_view(replacement));
    }

    /// Replaces all using the supplied arguments and current state.
    ///
    /// @param needle `needle` value used by the operation.
    /// @param replacement `replacement` value used by the operation.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @throws `invalid_argument` if `needle.empty()`.
    UString &UString::replace_all(const UString &needle, const UString &replacement) {
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

    /// Changes the logical size to the requested value, creating or removing elements as needed.
    ///
    /// @param requested_scalar_size Requested or available size for the operation.
    /// @param fill `fill` value used by the operation.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::resize(usize requested_scalar_size, char32_t fill) {
        if (requested_scalar_size < scalar_size_) {
            erase(requested_scalar_size);
            return *this;
        }

        while (scalar_size_ < requested_scalar_size) {
            append(fill);
        }
        return *this;
    }

    /// Performs the substr operation for `Foundation` using the supplied arguments.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    /// @param scalar_count Number of elements or operations to process.
    ///
    /// @return Returns the value produced by the operation.
    /// @throws `out_of_range` if `scalar_index > scalar_size_`.
    UString UString::substr(usize scalar_index, usize scalar_count) const {
        if (scalar_index > scalar_size_) {
            throw out_of_range{format("UString substr index {} is out of range for size {}.", scalar_index, scalar_size_)};
        }

        const usize actual_scalar_count = scalar_count == npos ? scalar_size_ - scalar_index : std::min(scalar_count, scalar_size_ - scalar_index);
        const usize first_byte = byte_index_of_unchecked(scalar_index);
        const usize last_byte = byte_index_of_unchecked(scalar_index + actual_scalar_count);
        return UString{string_view{storage_data() + first_byte, last_byte - first_byte}, actual_scalar_count, ValidatedInput{}};
    }

    /// Finds the requested entry in the available state.
    ///
    /// @param needle `needle` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize UString::find(const UString &needle, usize scalar_position) const {
        return find_validated(needle.cpp_string_view(), scalar_position);
    }

    /// Finds the requested entry in the available state.
    ///
    /// @param needle `needle` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize UString::find(string_view needle, usize scalar_position) const {
        validate_or_throw(needle);
        return find_validated(needle, scalar_position);
    }

    /// Finds the requested entry in the available state.
    ///
    /// @param needle `needle` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize UString::find(u8string_view needle, usize scalar_position) const {
        return find(as_char_view(needle), scalar_position);
    }

    /// Finds the last matching occurrence in the available range.
    ///
    /// @param needle `needle` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize UString::rfind(const UString &needle, usize scalar_position) const {
        return rfind_validated(needle.cpp_string_view(), scalar_position);
    }

    /// Finds the last matching occurrence in the available range.
    ///
    /// @param needle `needle` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize UString::rfind(string_view needle, usize scalar_position) const {
        validate_or_throw(needle);
        return rfind_validated(needle, scalar_position);
    }

    /// Finds the last matching occurrence in the available range.
    ///
    /// @param needle `needle` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize UString::rfind(u8string_view needle, usize scalar_position) const {
        return rfind(as_char_view(needle), scalar_position);
    }

    /// Reports whether contains holds for this `Foundation`.
    ///
    /// @param needle `needle` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool UString::contains(const UString &needle) const noexcept {
        return cpp_string_view().find(needle.cpp_string_view()) != string_view::npos;
    }

    /// Reports whether contains holds for this `Foundation`.
    ///
    /// @param needle `needle` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool UString::contains(string_view needle) const {
        return find(needle) != npos;
    }

    /// Reports whether contains holds for this `Foundation`.
    ///
    /// @param needle `needle` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool UString::contains(u8string_view needle) const {
        return contains(as_char_view(needle));
    }

    /// Reports whether contains holds for this `Foundation`.
    ///
    /// @param scalar `scalar` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool UString::contains(char32_t scalar) const {
        validate_scalar_or_throw(scalar);
        return contains_scalar_unchecked(scalar);
    }

    /// Reports whether the value begins with the supplied prefix.
    ///
    /// @param prefix `prefix` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool UString::starts_with(const UString &prefix) const noexcept {
        return cpp_string_view().starts_with(prefix.cpp_string_view());
    }

    /// Reports whether the value begins with the supplied prefix.
    ///
    /// @param prefix `prefix` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool UString::starts_with(string_view prefix) const {
        validate_or_throw(prefix);
        return cpp_string_view().starts_with(prefix);
    }

    /// Reports whether the value begins with the supplied prefix.
    ///
    /// @param prefix `prefix` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool UString::starts_with(u8string_view prefix) const {
        return starts_with(as_char_view(prefix));
    }

    /// Reports whether the value ends with the supplied suffix.
    ///
    /// @param suffix `suffix` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool UString::ends_with(const UString &suffix) const noexcept {
        return cpp_string_view().ends_with(suffix.cpp_string_view());
    }

    /// Reports whether the value ends with the supplied suffix.
    ///
    /// @param suffix `suffix` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool UString::ends_with(string_view suffix) const {
        validate_or_throw(suffix);
        return cpp_string_view().ends_with(suffix);
    }

    /// Reports whether the value ends with the supplied suffix.
    ///
    /// @param suffix `suffix` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool UString::ends_with(u8string_view suffix) const {
        return ends_with(as_char_view(suffix));
    }

    /// Finds first of in the available state.
    ///
    /// @param scalars `scalars` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @throws `out_of_range` if `scalar_position > scalar_size_`.
    usize UString::find_first_of(const UString &scalars, usize scalar_position) const {
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

    /// Finds first of in the available state.
    ///
    /// @param scalars `scalars` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize UString::find_first_of(string_view scalars, usize scalar_position) const {
        return find_first_of(UString{scalars}, scalar_position);
    }

    /// Finds first not of in the available state.
    ///
    /// @param scalars `scalars` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @throws `out_of_range` if `scalar_position > scalar_size_`.
    usize UString::find_first_not_of(const UString &scalars, usize scalar_position) const {
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

    /// Finds first not of in the available state.
    ///
    /// @param scalars `scalars` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize UString::find_first_not_of(string_view scalars, usize scalar_position) const {
        return find_first_not_of(UString{scalars}, scalar_position);
    }

    /// Finds last of in the available state.
    ///
    /// @param scalars `scalars` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize UString::find_last_of(const UString &scalars, usize scalar_position) const {
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

    /// Finds last of in the available state.
    ///
    /// @param scalars `scalars` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize UString::find_last_of(string_view scalars, usize scalar_position) const {
        return find_last_of(UString{scalars}, scalar_position);
    }

    /// Finds last not of in the available state.
    ///
    /// @param scalars `scalars` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize UString::find_last_not_of(const UString &scalars, usize scalar_position) const {
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

    /// Finds last not of in the available state.
    ///
    /// @param scalars `scalars` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize UString::find_last_not_of(string_view scalars, usize scalar_position) const {
        return find_last_not_of(UString{scalars}, scalar_position);
    }

    /// Reports whether codepoint boundary holds for this `Foundation`.
    ///
    /// @param byte_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool UString::is_codepoint_boundary(usize byte_index) const noexcept {
        if (byte_index > byte_size_) {
            return false;
        }
        if (byte_index == 0 || byte_index == byte_size_) {
            return true;
        }
        return !is_continuation_byte(static_cast<unsigned char>(storage_data()[byte_index]));
    }

    /// Performs the byte index of operation for `Foundation` using the supplied arguments.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @throws `out_of_range` if `scalar_index > scalar_size_`.
    usize UString::byte_index_of(usize scalar_index) const {
        if (scalar_index > scalar_size_) {
            throw out_of_range{format("UString scalar index {} is out of range for size {}.", scalar_index, scalar_size_)};
        }
        return byte_index_of_unchecked(scalar_index);
    }

    /// Performs the scalar index of byte operation for `Foundation` using the supplied arguments.
    ///
    /// @param byte_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @throws `out_of_range` if `!is_codepoint_boundary(byte_index)`.
    usize UString::scalar_index_of_byte(usize byte_index) const {
        if (!is_codepoint_boundary(byte_index)) {
            throw out_of_range{format("UString byte index {} is not a UTF-8 scalar boundary.", byte_index)};
        }
        return scalar_index_of_byte_unchecked(byte_index);
    }

    /// Compares the supplied values and returns their relative ordering or match result.
    ///
    /// @param other Other object used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    int UString::compare(const UString &other) const noexcept {
        return cpp_string_view().compare(other.cpp_string_view());
    }

    /// Compares the supplied values and returns their relative ordering or match result.
    ///
    /// @param other Other object used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    int UString::compare(string_view other) const {
        validate_or_throw(other);
        return cpp_string_view().compare(other);
    }

    /// Exchanges this object state with the supplied object.
    ///
    /// @param other Other object used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void UString::swap(UString &other) noexcept {
        using std::swap;
        swap(byte_size_, other.byte_size_);
        swap(scalar_size_, other.scalar_size_);
        swap(capacity_, other.capacity_);
        swap(heap_, other.heap_);
        swap(small_, other.small_);
    }

    /// Compares the operands for equality.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    bool operator==(const UString &lhs, const UString &rhs) noexcept {
        return lhs.cpp_string_view() == rhs.cpp_string_view();
    }

    /// Compares the operands and produces their ordering.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns the comparison category describing the ordering of the operands.
    /// @note This function does not throw exceptions.
    strong_ordering operator<=>(const UString &lhs, const UString &rhs) noexcept {
        const int result = lhs.compare(rhs);
        if (result < 0) {
            return strong_ordering::less;
        }
        if (result > 0) {
            return strong_ordering::greater;
        }
        return strong_ordering::equal;
    }

    /// Adds the operands and returns the result.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString operator+(UString lhs, const UString &rhs) {
        lhs += rhs;
        return lhs;
    }

    /// Adds the operands and returns the result.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString operator+(UString lhs, string_view rhs) {
        lhs += rhs;
        return lhs;
    }

    /// Adds the operands and returns the result.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString operator+(string_view lhs, const UString &rhs) {
        UString result{lhs};
        result += rhs;
        return result;
    }

    /// Adds the operands and returns the result.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString operator+(UString lhs, char32_t rhs) {
        lhs += rhs;
        return lhs;
    }

    /// Performs the as char view operation for `Foundation` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    string_view UString::as_char_view(u8string_view text) noexcept {
        return string_view{reinterpret_cast<const char *>(text.data()), text.size()};
    }

    /// Validates or throw.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @throws `invalid_argument` if `!validation`.
    UStringValidation UString::validate_or_throw(string_view text) {
        const UStringValidation validation = validate_utf8(text);
        if (!validation) {
            throw invalid_argument{format(
                "UString requires strict UTF-8 without embedded NUL bytes: {} at byte {}.",
                to_string(validation.error),
                validation.byte_index)};
        }
        return validation;
    }

    /// Validates scalar or throw.
    ///
    /// @param scalar `scalar` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @throws `invalid_argument` if `codepoint == 0`.
    /// @throws `invalid_argument` if `is_surrogate(codepoint)`.
    /// @throws `invalid_argument` if `codepoint > max_unicode_scalar`.
    void UString::validate_scalar_or_throw(char32_t scalar) {
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

    /// Encodes scalar or throw into the target representation.
    ///
    /// @param scalar `scalar` value used by the operation.
    /// @param buffer Buffer used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize UString::encode_scalar_or_throw(char32_t scalar, char *buffer) {
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

    /// Encodes the supplied value into the target representation.
    ///
    /// @param lead `lead` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    usize UString::encoded_length_from_lead(unsigned char lead) noexcept {
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

    /// Encodes the supplied value into the target representation.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    usize UString::encoded_length_unchecked(const char *text) noexcept {
        return encoded_length_from_lead(static_cast<unsigned char>(*text));
    }

    /// Decodes unchecked.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    char32_t UString::decode_unchecked(const char *text) noexcept {
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

    /// Performs the previous codepoint operation for `Foundation` using the supplied arguments.
    ///
    /// @param begin First position or element included in the operation.
    /// @param cursor `cursor` value used by the operation.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    const char *UString::previous_codepoint(const char *begin, const char *cursor) noexcept {
        do {
            --cursor;
        } while (cursor > begin && is_continuation_byte(static_cast<unsigned char>(*cursor)));
        return cursor;
    }

    /// Allocates buffer.
    ///
    /// @param capacity `capacity` value used by the operation.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @throws `length_error` if `capacity > max_size() || capacity == std::numeric_limits<usize>::max()`.
    char *UString::allocate_buffer(usize capacity) {
        if (capacity > max_size() || capacity == std::numeric_limits<usize>::max()) {
            throw length_error{"UString capacity exceeds max_size()."};
        }
        char *buffer = new char[capacity + 1];
        buffer[0] = '\0';
        return buffer;
    }

    /// Returns the storage data associated with this `Foundation`.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    const char *UString::storage_data() const noexcept {
        return heap_ == nullptr ? small_.data() : heap_;
    }

    /// Returns the mutable data associated with this `Foundation`.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    char *UString::mutable_data() noexcept {
        return heap_ == nullptr ? small_.data() : heap_;
    }

    /// Performs the overlaps storage operation for `Foundation` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool UString::overlaps_storage(string_view text) const noexcept {
        if (text.empty()) {
            return false;
        }
        const char *begin = storage_data();
        const char *end = begin + byte_size_;
        const char *text_begin = text.data();
        const char *text_end = text_begin + text.size();
        return text_begin < end && text_end > begin;
    }

    /// Moves from using the supplied arguments and current state.
    ///
    /// @param other Other object used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void UString::move_from(UString &&other) noexcept {
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

    /// Assigns validated unaliased using the supplied arguments and current state.
    ///
    /// @param text Text consumed by the operation.
    /// @param scalar_count Number of elements or operations to process.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void UString::assign_validated_unaliased(string_view text, usize scalar_count) {
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

    /// Finds or creates the capacity required by the operation.
    ///
    /// @param requested_capacity `requested_capacity` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @throws `length_error` if `requested_capacity > max_size()`.
    void UString::ensure_capacity(usize requested_capacity) {
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

    /// Returns the checked total byte size for this `Foundation`.
    ///
    /// @param additional_bytes `additional_bytes` value used by the operation.
    ///
    /// @return Returns the requested count or size.
    /// @throws `length_error` if `additional_bytes > max_size() - byte_size_`.
    usize UString::checked_total_byte_size(usize additional_bytes) const {
        if (additional_bytes > max_size() - byte_size_) {
            throw length_error{"UString operation would exceed max_size()."};
        }
        return byte_size_ + additional_bytes;
    }

    /// Appends the supplied value or range to the current contents.
    ///
    /// @param text Text consumed by the operation.
    /// @param scalar_count Number of elements or operations to process.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::append_validated(string_view text, usize scalar_count) {
        if (text.empty()) {
            return *this;
        }
        if (overlaps_storage(text)) {
            UString copy{text, scalar_count, ValidatedInput{}};
            return append_validated(copy.cpp_string_view(), copy.scalar_size_);
        }

        append_validated_unaliased(text, scalar_count);
        return *this;
    }

    /// Appends the supplied value or range to the current contents.
    ///
    /// @param text Text consumed by the operation.
    /// @param scalar_count Number of elements or operations to process.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void UString::append_validated_unaliased(string_view text, usize scalar_count) {
        const usize old_byte_size = byte_size_;
        const usize new_byte_size = checked_total_byte_size(text.size());
        ensure_capacity(new_byte_size);

        std::memcpy(mutable_data() + old_byte_size, text.data(), text.size());
        byte_size_ = new_byte_size;
        scalar_size_ += scalar_count;
        mutable_data()[byte_size_] = '\0';
    }

    /// Appends the supplied value or range to the current contents.
    ///
    /// @param encoded `encoded` value used by the operation.
    /// @param encoded_size Requested or available size for the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void UString::append_encoded_scalar(const char *encoded, usize encoded_size) {
        const usize old_byte_size = byte_size_;
        const usize new_byte_size = checked_total_byte_size(encoded_size);
        ensure_capacity(new_byte_size);

        std::memcpy(mutable_data() + old_byte_size, encoded, encoded_size);
        byte_size_ = new_byte_size;
        ++scalar_size_;
        mutable_data()[byte_size_] = '\0';
    }

    /// Inserts the supplied value or range at the requested position.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    /// @param text Text consumed by the operation.
    /// @param scalar_count Number of elements or operations to process.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @throws `out_of_range` if `scalar_index > scalar_size_`.
    UString &UString::insert_validated(usize scalar_index, string_view text, usize scalar_count) {
        if (scalar_index > scalar_size_) {
            throw out_of_range{format("UString insert index {} is out of range for size {}.", scalar_index, scalar_size_)};
        }
        if (text.empty()) {
            return *this;
        }
        if (overlaps_storage(text)) {
            UString copy{text, scalar_count, ValidatedInput{}};
            return insert_validated(scalar_index, copy.cpp_string_view(), copy.scalar_size_);
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
    UString &UString::replace_validated(usize scalar_index, usize scalar_count, string_view replacement, usize replacement_scalar_count) {
        if (scalar_index > scalar_size_) {
            throw out_of_range{format("UString replace index {} is out of range for size {}.", scalar_index, scalar_size_)};
        }
        if (overlaps_storage(replacement)) {
            UString copy{replacement, replacement_scalar_count, ValidatedInput{}};
            return replace_validated(scalar_index, scalar_count, copy.cpp_string_view(), copy.scalar_size_);
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

    /// Performs the byte index of unchecked operation for `Foundation` using the supplied arguments.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    usize UString::byte_index_of_unchecked(usize scalar_index) const noexcept {
        const char *cursor = storage_data();
        for (usize index = 0; index < scalar_index; ++index) {
            cursor += encoded_length_unchecked(cursor);
        }
        return static_cast<usize>(cursor - storage_data());
    }

    /// Performs the scalar index of byte unchecked operation for `Foundation` using the supplied arguments.
    ///
    /// @param byte_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    usize UString::scalar_index_of_byte_unchecked(usize byte_index) const noexcept {
        const char *cursor = storage_data();
        const char *target = storage_data() + byte_index;
        usize scalar_index = 0;
        while (cursor < target) {
            cursor += encoded_length_unchecked(cursor);
            ++scalar_index;
        }
        return scalar_index;
    }

    /// Performs the iterator at unchecked operation for `Foundation` using the supplied arguments.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    UString::CodepointIterator UString::iterator_at_unchecked(usize scalar_index) const noexcept {
        const char *cursor = storage_data() + byte_index_of_unchecked(scalar_index);
        return CodepointIterator{storage_data(), cursor};
    }

    /// Reports whether scalar unchecked holds for this `Foundation`.
    ///
    /// @param scalar `scalar` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool UString::contains_scalar_unchecked(char32_t scalar) const noexcept {
        for (char32_t current : codepoints()) {
            if (current == scalar) {
                return true;
            }
        }
        return false;
    }

    /// Finds validated in the available state.
    ///
    /// @param needle `needle` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @throws `out_of_range` if `scalar_position > scalar_size_`.
    usize UString::find_validated(string_view needle, usize scalar_position) const {
        if (scalar_position > scalar_size_) {
            throw out_of_range{"UString::find() start position is out of range."};
        }
        if (needle.empty()) {
            return scalar_position;
        }

        const usize start_byte = byte_index_of_unchecked(scalar_position);
        const size_t found = cpp_string_view().find(needle, start_byte);
        if (found == string_view::npos) {
            return npos;
        }
        return scalar_index_of_byte_unchecked(static_cast<usize>(found));
    }

    /// Finds the last matching occurrence in the available range.
    ///
    /// @param needle `needle` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize UString::rfind_validated(string_view needle, usize scalar_position) const {
        if (needle.empty()) {
            return scalar_position == npos ? scalar_size_ : std::min(scalar_position, scalar_size_);
        }
        if (byte_size_ < needle.size()) {
            return npos;
        }

        const usize clamped_scalar_position = scalar_position == npos ? scalar_size_ : std::min(scalar_position, scalar_size_);
        const usize byte_position = byte_index_of_unchecked(clamped_scalar_position);
        const size_t found = cpp_string_view().rfind(needle, byte_position);
        if (found == string_view::npos) {
            return npos;
        }
        return scalar_index_of_byte_unchecked(static_cast<usize>(found));
    }

    /// Exchanges this object state with the supplied object.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @note This function does not throw exceptions.
    void swap(UString &lhs, UString &rhs) noexcept {
        lhs.swap(rhs);
    }

    /// Validates UTF-8.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    UStringValidation ustr::validate_utf8(string_view text) noexcept {
        return UString::validate_utf8(text);
    }

    /// Reports whether valid UTF-8 holds for this `Foundation`.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool ustr::is_valid_utf8(string_view text) noexcept {
        return UString::is_valid_utf8(text);
    }

    /// Creates or converts a value from c str representation.
    ///
    /// @param buffer Buffer used or affected by the operation.
    /// @param max_bytes `max_bytes` value used by the operation.
    ///
    /// @return Returns the newly constructed or converted value.
    /// @throws `invalid_argument` if `buffer == nullptr && max_bytes != 0`.
    ustr ustr::from_c_str(const char *buffer, usize max_bytes) {
        if (buffer == nullptr && max_bytes != 0) {
            throw invalid_argument{"ustr::from_c_str() received a null pointer with a non-zero size."};
        }
        const usize length = Detail::bounded_c_length(buffer, max_bytes);
        return ustr{string_view{buffer == nullptr ? "" : buffer, length}};
    }

    /// Returns the data associated with this `Foundation`.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    const char *ustr::data() const noexcept {
        return bytes_.data();
    }

    /// Returns the current or globally available C++ string view value.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    string_view ustr::cpp_string_view() const noexcept {
        return bytes_;
    }

    /// Computes the C++ bytes required by the supplied values.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    string_view ustr::cpp_bytes() const noexcept {
        return bytes_;
    }

    /// Returns the current or globally available C++ string value.
    ///
    /// @return Returns the current C++ string value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    string ustr::cpp_string() const {
        return string{bytes_};
    }

    /// Converts the value to std string representation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `TextConversionError::NonAscii`.
    expected<string, TextConversionError> ustr::to_std_string() const {
        if (!is_ascii()) {
            return unexpected(TextConversionError::NonAscii);
        }
        return string{bytes_};
    }

    /// Converts the value to std string unchecked representation.
    ///
    /// @return Returns the current to std string unchecked value.
    /// @note Terminates the process if an invariant required by this unchecked operation is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    string ustr::to_std_string_unchecked() const {
        if (!is_ascii()) {
            std::terminate();
        }
        return string{bytes_};
    }

    /// Converts the value to owned representation.
    ///
    /// @return Returns the current to owned value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString ustr::to_owned() const {
        return UString{*this};
    }

    /// Returns the current or globally available C++ u8string view value.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    u8string_view ustr::cpp_u8string_view() const noexcept {
        return u8string_view{reinterpret_cast<const char8_t *>(bytes_.data()), bytes_.size()};
    }

    /// Returns the current or globally available C++ u8string value.
    ///
    /// @return Returns the current C++ u8string value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    u8string ustr::cpp_u8string() const {
        return u8string{cpp_u8string_view()};
    }

    /// Returns the current or globally available C++ wstring value.
    ///
    /// @return Returns the current C++ wstring value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    wstring ustr::cpp_wstring() const {
        if constexpr (sizeof(wchar_t) == 2) {
            const u16string units = cpp_u16string();
            return wstring{units.begin(), units.end()};
        } else {
            const u32string units = cpp_u32string();
            return wstring{units.begin(), units.end()};
        }
    }

    /// Assigns validated using the supplied arguments and current state.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void ustr::assign_validated(string_view text) {
        const UStringValidation validation = validate_or_throw(text);
        bytes_ = text;
        scalar_size_ = validation.scalar_count;
    }

    /// Performs the as char view operation for `Foundation` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    string_view ustr::as_char_view(u8string_view text) noexcept {
        return string_view{reinterpret_cast<const char *>(text.data()), text.size()};
    }

    /// Validates or throw.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @throws `invalid_argument` if `!validation`.
    UStringValidation ustr::validate_or_throw(string_view text) {
        const UStringValidation validation = UString::validate_utf8(text);
        if (!validation) {
            throw invalid_argument{format(
                "ustr requires strict UTF-8 without embedded NUL bytes: {} at byte {}.",
                to_string(validation.error),
                validation.byte_index)};
        }
        return validation;
    }

    /// Validates scalar or throw.
    ///
    /// @param scalar `scalar` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @throws `invalid_argument` if `codepoint == 0`.
    /// @throws `invalid_argument` if `codepoint >= 0xD800 && codepoint <= 0xDFFF`.
    /// @throws `invalid_argument` if `codepoint > 0x10FFFF`.
    void ustr::validate_scalar_or_throw(char32_t scalar) {
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

    /// Encodes the supplied value into the target representation.
    ///
    /// @param lead `lead` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    usize ustr::encoded_length_from_lead(unsigned char lead) noexcept {
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

    /// Encodes the supplied value into the target representation.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    usize ustr::encoded_length_unchecked(const char *text) noexcept {
        return encoded_length_from_lead(static_cast<unsigned char>(*text));
    }

    /// Decodes unchecked.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    char32_t ustr::decode_unchecked(const char *text) noexcept {
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

    /// Performs the previous codepoint operation for `Foundation` using the supplied arguments.
    ///
    /// @param begin First position or element included in the operation.
    /// @param cursor `cursor` value used by the operation.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    const char *ustr::previous_codepoint(const char *begin, const char *cursor) noexcept {
        do {
            --cursor;
        } while (cursor > begin && is_continuation_byte(static_cast<unsigned char>(*cursor)));
        return cursor;
    }

    /// Performs the byte index of unchecked operation for `Foundation` using the supplied arguments.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    usize ustr::byte_index_of_unchecked(usize scalar_index) const noexcept {
        const char *cursor = bytes_.data();
        for (usize index = 0; index < scalar_index; ++index) {
            cursor += encoded_length_unchecked(cursor);
        }
        return static_cast<usize>(cursor - bytes_.data());
    }

    /// Performs the scalar index of byte unchecked operation for `Foundation` using the supplied arguments.
    ///
    /// @param byte_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    usize ustr::scalar_index_of_byte_unchecked(usize byte_index) const noexcept {
        const char *cursor = bytes_.data();
        const char *target = bytes_.data() + byte_index;
        usize scalar_index = 0;
        while (cursor < target) {
            cursor += encoded_length_unchecked(cursor);
            ++scalar_index;
        }
        return scalar_index;
    }

    /// Reports whether scalar unchecked holds for this `Foundation`.
    ///
    /// @param scalar `scalar` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool ustr::contains_scalar_unchecked(char32_t scalar) const noexcept {
        for (char32_t current : codepoints()) {
            if (current == scalar) {
                return true;
            }
        }
        return false;
    }

    /// Assigns a new value to this `Foundation`.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::operator=(const ustr &text) {
        return assign(text);
    }

    /// Returns the current or globally available as ustr value.
    ///
    /// @return Returns the current as ustr value.
    /// @note This function does not throw exceptions.
    ustr UString::as_ustr() const & noexcept {
        return ustr{cpp_string_view(), scalar_size_, ustr::ValidatedInput{}};
    }

    /// Returns the current or globally available slice value.
    ///
    /// @return Returns the current slice value.
    /// @note This function does not throw exceptions.
    ustr UString::slice() const & noexcept {
        return as_ustr();
    }

    /// Performs the slice operation for `Foundation` using the supplied arguments.
    ///
    /// @param scalar_start `scalar_start` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    ustr UString::slice(usize scalar_start) const & {
        return slice(USlice{scalar_start});
    }

    /// Performs the slice operation for `Foundation` using the supplied arguments.
    ///
    /// @param scalar_start `scalar_start` value used by the operation.
    /// @param scalar_end `scalar_end` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    ustr UString::slice(usize scalar_start, usize scalar_end) const & {
        return slice(USlice{scalar_start, scalar_end});
    }

    /// Performs the slice operation for `Foundation` using the supplied arguments.
    ///
    /// @param range Range of values to process.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    ustr UString::slice(USlice range) const & {
        const Detail::ResolvedSlice resolved = Detail::resolve_slice(range, scalar_size_, "UString");
        const usize first_byte = byte_index_of_unchecked(resolved.start);
        const usize last_byte = byte_index_of_unchecked(resolved.end);
        return ustr{string_view{storage_data() + first_byte, last_byte - first_byte}, resolved.scalar_count(), ustr::ValidatedInput{}};
    }

    /// Performs the slice operation for `Foundation` using the supplied arguments.
    ///
    /// @param pattern `pattern` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString UString::slice(USlicePattern pattern) const {
        return as_ustr().slice(pattern);
    }

    /// Returns the current or globally available C++ u16string value.
    ///
    /// @return Returns the current C++ u16string value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    u16string UString::cpp_u16string() const {
        return Detail::to_utf16(codepoints());
    }

    /// Returns the current or globally available C++ u32string value.
    ///
    /// @return Returns the current C++ u32string value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    u32string UString::cpp_u32string() const {
        return Detail::to_utf32(codepoints());
    }

    /// Returns the current or globally available C++ u16string value.
    ///
    /// @return Returns the current C++ u16string value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    u16string ustr::cpp_u16string() const {
        return Detail::to_utf16(codepoints());
    }

    /// Returns the current or globally available C++ u32string value.
    ///
    /// @return Returns the current C++ u32string value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    u32string ustr::cpp_u32string() const {
        return Detail::to_utf32(codepoints());
    }

    /// Accesses an element of the `Foundation` by index or key.
    ///
    /// @param range Range of values to process.
    ///
    /// @return Returns the selected element or a reference/proxy referring to it.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    ustr UString::operator[](USlice range) const & {
        return slice(range);
    }

    /// Accesses an element of the `Foundation` by index or key.
    ///
    /// @param pattern `pattern` value used by the operation.
    ///
    /// @return Returns the selected element or a reference/proxy referring to it.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString UString::operator[](USlicePattern pattern) const {
        return slice(pattern);
    }

    /// Assigns the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::assign(const ustr &text) {
        assign_validated_unaliased(text.cpp_string_view(), text.scalar_size());
        return *this;
    }

    /// Appends the supplied value or range to the current contents.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::append(const ustr &text) {
        return append_validated(text.cpp_string_view(), text.scalar_size());
    }

    /// Adds the right-hand value to this object in place.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::operator+=(const ustr &text) {
        return append(text);
    }

    /// Inserts the supplied value or range at the requested position.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::insert(usize scalar_index, const ustr &text) {
        return insert_validated(scalar_index, text.cpp_string_view(), text.scalar_size());
    }

    /// Replaces the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    /// @param scalar_count Number of elements or operations to process.
    /// @param replacement `replacement` value used by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::replace(usize scalar_index, usize scalar_count, const ustr &replacement) {
        return replace_validated(scalar_index, scalar_count, replacement.cpp_string_view(), replacement.scalar_size());
    }

    /// Replaces all using the supplied arguments and current state.
    ///
    /// @param needle `needle` value used by the operation.
    /// @param replacement `replacement` value used by the operation.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @throws `invalid_argument` if `needle.empty()`.
    UString &UString::replace_all(const ustr &needle, const ustr &replacement) {
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

    /// Finds the requested entry in the available state.
    ///
    /// @param needle `needle` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize UString::find(const ustr &needle, usize scalar_position) const {
        return find_validated(needle.cpp_string_view(), scalar_position);
    }

    /// Finds the last matching occurrence in the available range.
    ///
    /// @param needle `needle` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    usize UString::rfind(const ustr &needle, usize scalar_position) const {
        return rfind_validated(needle.cpp_string_view(), scalar_position);
    }

    /// Reports whether contains holds for this `Foundation`.
    ///
    /// @param needle `needle` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool UString::contains(const ustr &needle) const noexcept {
        return cpp_string_view().find(needle.cpp_string_view()) != string_view::npos;
    }

    /// Reports whether the value begins with the supplied prefix.
    ///
    /// @param prefix `prefix` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool UString::starts_with(const ustr &prefix) const noexcept {
        return cpp_string_view().starts_with(prefix.cpp_string_view());
    }

    /// Reports whether the value ends with the supplied suffix.
    ///
    /// @param suffix `suffix` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool UString::ends_with(const ustr &suffix) const noexcept {
        return cpp_string_view().ends_with(suffix.cpp_string_view());
    }

    /// Compares the supplied values and returns their relative ordering or match result.
    ///
    /// @param other Other object used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    int UString::compare(const ustr &other) const noexcept {
        return cpp_string_view().compare(other.cpp_string_view());
    }

    /// Compares the operands for equality.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    bool operator==(const UString &lhs, const ustr &rhs) noexcept {
        return lhs.cpp_string_view() == rhs.cpp_string_view();
    }

    /// Compares the operands for equality.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    bool operator==(const ustr &lhs, const UString &rhs) noexcept {
        return lhs.cpp_string_view() == rhs.cpp_string_view();
    }

    /// Compares the operands and produces their ordering.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns the comparison category describing the ordering of the operands.
    /// @note This function does not throw exceptions.
    strong_ordering operator<=>(const UString &lhs, const ustr &rhs) noexcept {
        const int result = lhs.compare(rhs);
        if (result < 0) {
            return strong_ordering::less;
        }
        if (result > 0) {
            return strong_ordering::greater;
        }
        return strong_ordering::equal;
    }

    /// Compares the operands and produces their ordering.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns the comparison category describing the ordering of the operands.
    /// @note This function does not throw exceptions.
    strong_ordering operator<=>(const ustr &lhs, const UString &rhs) noexcept {
        const int result = lhs.compare(rhs.as_ustr());
        if (result < 0) {
            return strong_ordering::less;
        }
        if (result > 0) {
            return strong_ordering::greater;
        }
        return strong_ordering::equal;
    }

    /// Adds the operands and returns the result.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString operator+(UString lhs, const ustr &rhs) {
        lhs += rhs;
        return lhs;
    }

    /// Adds the operands and returns the result.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString operator+(const ustr &lhs, const UString &rhs) {
        UString result{lhs};
        result += rhs;
        return result;
    }

    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::ostream &operator<<(std::ostream &os, const UString &value) {
        return os << value.cpp_string_view();
    }

    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::ostream &operator<<(std::ostream &os, const ustr &value) {
        return os << value.cpp_string_view();
    }

    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::ostream &operator<<(std::ostream &os, USlice value) {
        return os << Detail::display_string(value);
    }

    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::ostream &operator<<(std::ostream &os, USlicePattern value) {
        return os << Detail::display_string(value);
    }

    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::ostream &operator<<(std::ostream &os, UStringValidationError value) {
        return os << to_string(value);
    }

    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::ostream &operator<<(std::ostream &os, TextConversionError value) {
        return os << to_string(value);
    }

    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::ostream &operator<<(std::ostream &os, const UStringValidation &value) {
        return os << Detail::display_string(value);
    }

} // namespace SFT::Foundation

/// Converts the `` to `_ustr`.
///
/// @param text Text consumed by the operation.
/// @param byte_count Number of elements or operations to process.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
SFT::Foundation::ustr operator""_ustr(const char *text, size_t byte_count) {
    return SFT::Foundation::ustr{string_view{text, byte_count}};
}

/// Converts the `` to `_ustr`.
///
/// @param text Text consumed by the operation.
/// @param byte_count Number of elements or operations to process.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
SFT::Foundation::ustr operator""_ustr(const char8_t *text, size_t byte_count) {
    return SFT::Foundation::ustr{u8string_view{text, byte_count}};
}


namespace SFT::Foundation {

    /// Performs the ustring operation for `Foundation` using the supplied arguments.
    ///
    /// @note This function does not throw exceptions.
    UString::UString() noexcept {
        small_[0] = '\0';
    }

    /// Performs the ustring operation for `Foundation` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @throws `invalid_argument` if `text == nullptr`.
    UString::UString(const char *text) {
        if (text == nullptr) {
            throw invalid_argument{"UString cannot be constructed from a null pointer."};
        }
        assign(string_view{text});
    }

    /// Performs the ustring operation for `Foundation` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    /// @param byte_count Number of elements or operations to process.
    ///
    /// @throws `invalid_argument` if `text == nullptr && byte_count != 0`.
    UString::UString(const char *text, usize byte_count) {
        if (text == nullptr && byte_count != 0) {
            throw invalid_argument{"UString cannot be constructed from a null pointer and non-zero size."};
        }
        assign(string_view{text == nullptr ? "" : text, byte_count});
    }

    /// Performs the ustring operation for `Foundation` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString::UString(string_view text) {
        assign(text);
    }

    /// Performs the ustring operation for `Foundation` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @note This function does not throw exceptions.
    UString::UString(const string &text) noexcept {
        assign(string_view{text});
    }

    /// Performs the ustring operation for `Foundation` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString::UString(u8string_view text) {
        assign(as_char_view(text));
    }

    /// Performs the ustring operation for `Foundation` using the supplied arguments.
    ///
    /// @param other Other object used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString::UString(const UString &other) {
        assign_validated_unaliased(other.cpp_string_view(), other.scalar_size_);
    }

    /// Performs the ustring operation for `Foundation` using the supplied arguments.
    ///
    /// @param other Other object used by the operation.
    ///
    /// @note This function does not throw exceptions.
    UString::UString(UString &&other) noexcept {
        move_from(std::move(other));
    }

    /// Destroys the `Foundation` and releases resources owned by it.
    ///
    /// @note This function does not throw exceptions.
    UString::~UString() noexcept {
        delete[] heap_;
    }

    /// Performs the ustring operation for `Foundation` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    /// @param scalar_count Number of elements or operations to process.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString::UString(string_view text, usize scalar_count, ValidatedInput) {
        assign_validated_unaliased(text, scalar_count);
    }

    /// Performs the ustr operation for `Foundation` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @throws `invalid_argument` if `text == nullptr`.
    ustr::ustr(const char *text) {
        if (text == nullptr) {
            throw invalid_argument{"ustr cannot be constructed from a null pointer."};
        }
        assign_validated(string_view{text});
    }

    /// Performs the ustr operation for `Foundation` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    /// @param byte_count Number of elements or operations to process.
    ///
    /// @throws `invalid_argument` if `text == nullptr && byte_count != 0`.
    ustr::ustr(const char *text, usize byte_count) {
        if (text == nullptr && byte_count != 0) {
            throw invalid_argument{"ustr cannot be constructed from a null pointer and non-zero size."};
        }
        assign_validated(string_view{text == nullptr ? "" : text, byte_count});
    }

    /// Performs the ustr operation for `Foundation` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    ustr::ustr(string_view text) {
        assign_validated(text);
    }

    /// Performs the ustr operation for `Foundation` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    ustr::ustr(const string &text) {
        assign_validated(string_view{text});
    }

    /// Performs the ustr operation for `Foundation` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    ustr::ustr(u8string_view text) {
        assign_validated(as_char_view(text));
    }

    /// Performs the ustr operation for `Foundation` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @note This function does not throw exceptions.
    ustr::ustr(const UString &text) noexcept
        : bytes_(text.cpp_string_view()), scalar_size_(text.scalar_size()) {
    }

    /// Performs the ustring operation for `Foundation` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString::UString(const ustr &text) {
        assign_validated_unaliased(text.cpp_string_view(), text.scalar_size());
    }

    /// Converts the `Foundation` to `ustr`.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    UString::operator ustr() const & noexcept {
        return as_ustr();
    }

} // namespace SFT::Foundation


namespace SFT::Foundation {

    /// Creates or converts a value from c str representation.
    ///
    /// @param buffer Buffer used or affected by the operation.
    ///
    /// @return Returns the newly constructed or converted value.
    /// @throws `invalid_argument` if `buffer == nullptr`.
    [[nodiscard]] UString UString::from_c_str(const char *buffer) {
        if (buffer == nullptr) {
            throw invalid_argument{"UString::from_c_str() received a null pointer."};
        }
        return UString{string_view{buffer}};
    }

    /// Creates or converts a value from c str representation.
    ///
    /// @param buffer Buffer used or affected by the operation.
    /// @param max_bytes `max_bytes` value used by the operation.
    ///
    /// @return Returns the newly constructed or converted value.
    /// @throws `invalid_argument` if `buffer == nullptr && max_bytes != 0`.
    [[nodiscard]] UString UString::from_c_str(const char *buffer, usize max_bytes) {
        if (buffer == nullptr && max_bytes != 0) {
            throw invalid_argument{"UString::from_c_str() received a null pointer with a non-zero size."};
        }
        const usize length = Detail::bounded_c_length(buffer, max_bytes);
        return UString{string_view{buffer == nullptr ? "" : buffer, length}};
    }

    /// Attempts to from c str without requiring normal failure to be exceptional.
    ///
    /// @param buffer Buffer used or affected by the operation.
    /// @param max_bytes `max_bytes` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    [[nodiscard]] optional<UString> UString::try_from_c_str(const char *buffer, usize max_bytes) {
        if (buffer == nullptr) {
            return max_bytes == 0 ? optional<UString>{UString{}} : nullopt;
        }
        const usize length = Detail::bounded_c_length(buffer, max_bytes);
        return try_from_utf8(string_view{buffer, length});
    }

    /// Creates or converts a value from UTF-16 representation.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the newly constructed or converted value.
    /// @throws `invalid_argument` if `index + 1 >= text.size()`.
    /// @throws `invalid_argument` if `low < 0xDC00 || low > 0xDFFF`.
    /// @throws `invalid_argument` if `unit >= 0xDC00 && unit <= 0xDFFF`.
    [[nodiscard]] UString UString::from_utf16(u16string_view text) {
        UString value;
        for (usize index = 0; index < text.size(); ++index) {
            const auto unit = static_cast<u32>(static_cast<char16_t>(text[index]));
            char32_t scalar = 0;
            if (unit >= 0xD800 && unit <= 0xDBFF) {
                if (index + 1 >= text.size()) {
                    throw invalid_argument{"UString::from_utf16() ends with a truncated surrogate pair."};
                }
                const auto low = static_cast<u32>(static_cast<char16_t>(text[index + 1]));
                if (low < 0xDC00 || low > 0xDFFF) {
                    throw invalid_argument{"UString::from_utf16() has an unpaired high surrogate."};
                }
                scalar = static_cast<char32_t>(0x10000u + ((unit - 0xD800u) << 10u) + (low - 0xDC00u));
                ++index;
            } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
                throw invalid_argument{"UString::from_utf16() has an unpaired low surrogate."};
            } else {
                scalar = static_cast<char32_t>(unit);
            }
            value.append(scalar);
        }
        return value;
    }

    /// Creates or converts a value from UTF-32 representation.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the newly constructed or converted value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] UString UString::from_utf32(u32string_view text) {
        return from_codepoints(text);
    }

    /// Creates or converts a value from wstring representation.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the newly constructed or converted value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] UString UString::from_wstring(wstring_view text) {
        if constexpr (sizeof(wchar_t) == 2) {
            u16string units;
            units.reserve(text.size());
            for (wchar_t unit : text) {
                units.push_back(static_cast<char16_t>(unit));
            }
            return from_utf16(units);
        } else {
            u32string units;
            units.reserve(text.size());
            for (wchar_t unit : text) {
                units.push_back(static_cast<char32_t>(unit));
            }
            return from_utf32(units);
        }
    }

    /// Returns the data associated with this `Foundation`.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const char *UString::data() const noexcept {
        return storage_data();
    }

    /// Returns the c str associated with this `Foundation`.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const char *UString::c_str() const noexcept {
        return storage_data();
    }

    /// Returns the current or globally available C++ string view value.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    [[nodiscard]] string_view UString::cpp_string_view() const noexcept {
        return string_view{storage_data(), byte_size_};
    }

    /// Computes the C++ bytes required by the supplied values.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    [[nodiscard]] string_view UString::cpp_bytes() const noexcept {
        return cpp_string_view();
    }

    /// Converts the `Foundation` to `string_view`.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    [[nodiscard]] UString::operator string_view() const noexcept {
        return cpp_string_view();
    }

    /// Reports whether this `Foundation` contains no elements or payload.
    ///
    /// @return Returns the current empty value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool UString::empty() const noexcept {
        return byte_size_ == 0;
    }

    /// Reports whether ascii holds for this `Foundation`.
    ///
    /// @return Returns the current is ascii value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool UString::is_ascii() const noexcept {
        return std::ranges::all_of(cpp_string_view(), [](char byte) {
            return static_cast<unsigned char>(byte) <= 0x7Fu;
        });
    }

    /// Returns the size for this `Foundation`.
    ///
    /// @return Returns the current size value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize UString::size() const noexcept {
        return scalar_size_;
    }

    /// Returns the length for this `Foundation`.
    ///
    /// @return Returns the current length value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize UString::length() const noexcept {
        return scalar_size_;
    }

    /// Returns the scalar size for this `Foundation`.
    ///
    /// @return Returns the current scalar size value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize UString::scalar_size() const noexcept {
        return scalar_size_;
    }

    /// Returns the codepoint size for this `Foundation`.
    ///
    /// @return Returns the current codepoint size value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize UString::codepoint_size() const noexcept {
        return scalar_size_;
    }

    /// Returns the byte size for this `Foundation`.
    ///
    /// @return Returns the current byte size value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize UString::byte_size() const noexcept {
        return byte_size_;
    }

    /// Computes the size bytes required by the supplied values.
    ///
    /// @return Returns the current size bytes value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize UString::size_bytes() const noexcept {
        return byte_size_;
    }

    /// Returns the current or globally available capacity value.
    ///
    /// @return Returns the current capacity value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize UString::capacity() const noexcept {
        return capacity_;
    }

    /// Reports whether small holds for this `Foundation`.
    ///
    /// @return Returns the current is small value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool UString::is_small() const noexcept {
        return heap_ == nullptr;
    }

    /// Returns an iterator to the first element in the range.
    ///
    /// @return Returns an iterator referring to the first element.
    /// @note This function does not throw exceptions.
    [[nodiscard]] UString::CodepointIterator UString::begin() const noexcept {
        return CodepointIterator{storage_data(), storage_data()};
    }

    /// Returns the one-past-the-end iterator for the range.
    ///
    /// @return Returns the one-past-the-end iterator.
    /// @note This function does not throw exceptions.
    [[nodiscard]] UString::CodepointIterator UString::end() const noexcept {
        return CodepointIterator{storage_data(), storage_data() + byte_size_};
    }

    /// Returns the current or globally available codepoints value.
    ///
    /// @return Returns the current codepoints value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] UString::CodepointView UString::codepoints() const noexcept {
        return CodepointView{storage_data(), byte_size_, scalar_size_};
    }

    /// Returns the current or globally available byte begin value.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const char *UString::byte_begin() const noexcept {
        return storage_data();
    }

    /// Returns the current or globally available byte end value.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const char *UString::byte_end() const noexcept {
        return storage_data() + byte_size_;
    }

    /// Returns the current or globally available front value.
    ///
    /// @return Returns the current front value.
    /// @throws `out_of_range` if `empty()`.
    [[nodiscard]] char32_t UString::front() const {
        if (empty()) {
            throw out_of_range{"UString::front() called on an empty string."};
        }
        return decode_unchecked(storage_data());
    }

    /// Returns the current or globally available back value.
    ///
    /// @return Returns the current back value.
    /// @throws `out_of_range` if `empty()`.
    [[nodiscard]] char32_t UString::back() const {
        if (empty()) {
            throw out_of_range{"UString::back() called on an empty string."};
        }
        return decode_unchecked(previous_codepoint(storage_data(), storage_data() + byte_size_));
    }

    /// Performs the at operation for `Foundation` using the supplied arguments.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @throws `out_of_range` if `scalar_index >= scalar_size_`.
    [[nodiscard]] char32_t UString::at(usize scalar_index) const {
        if (scalar_index >= scalar_size_) {
            throw out_of_range{format("UString scalar index {} is out of range for size {}.", scalar_index, scalar_size_)};
        }
        return decode_unchecked(storage_data() + byte_index_of(scalar_index));
    }

    /// Accesses an element of the `Foundation` by index or key.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the selected element or a reference/proxy referring to it.
    /// @pre `scalar_index < scalar_size_`; debug builds assert if this precondition is violated.
    /// @note This function does not throw exceptions.
    [[nodiscard]] char32_t UString::operator[](usize scalar_index) const noexcept {
        assert(scalar_index < scalar_size_);
        return decode_unchecked(storage_data() + byte_index_of_unchecked(scalar_index));
    }

    /// Clears the stored state or contents.
    ///
    /// @return Returns the current clear value.
    /// @note This function does not throw exceptions.
    void UString::clear() noexcept {
        byte_size_ = 0;
        scalar_size_ = 0;
        mutable_data()[0] = '\0';
    }

    /// Reserves storage for at least the requested capacity without changing the logical contents.
    ///
    /// @param requested_capacity `requested_capacity` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @throws `length_error` if `requested_capacity > max_size()`.
    void UString::reserve(usize requested_capacity) {
        if (requested_capacity > max_size()) {
            throw length_error{"UString capacity request exceeds max_size()."};
        }
        ensure_capacity(requested_capacity);
    }

    /// Returns the current or globally available shrink to fit value.
    ///
    /// @return Returns the current shrink to fit value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void UString::shrink_to_fit() {
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

    /// Appends the supplied value or range to the current contents.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::append(const UString &text) {
        return append_validated(text.cpp_string_view(), text.scalar_size_);
    }

    /// Adds the supplied value to the end or work queue.
    ///
    /// @param scalar `scalar` value used by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::push_back(char32_t scalar) {
        return append(scalar);
    }

    /// Removes and returns or discards the next value from the container or queue.
    ///
    /// @return Returns the current pop back value.
    /// @throws `out_of_range` if `empty()`.
    void UString::pop_back() {
        if (empty()) {
            throw out_of_range{"UString::pop_back() called on an empty string."};
        }

        const char *last = previous_codepoint(storage_data(), storage_data() + byte_size_);
        byte_size_ = static_cast<usize>(last - storage_data());
        --scalar_size_;
        mutable_data()[byte_size_] = '\0';
    }

    /// Adds the right-hand value to this object in place.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString &UString::operator+=(const UString &text) {
        return append(text);
    }

    /// Converts the `Foundation` to `string_view`.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    [[nodiscard]] ustr::operator string_view() const noexcept {
        return bytes_;
    }

    /// Reports whether this `Foundation` contains no elements or payload.
    ///
    /// @return Returns the current empty value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool ustr::empty() const noexcept {
        return bytes_.empty();
    }

    /// Reports whether ascii holds for this `Foundation`.
    ///
    /// @return Returns the current is ascii value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool ustr::is_ascii() const noexcept {
        return std::ranges::all_of(bytes_, [](char byte) {
            return static_cast<unsigned char>(byte) <= 0x7Fu;
        });
    }

    /// Returns the size for this `Foundation`.
    ///
    /// @return Returns the current size value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize ustr::size() const noexcept {
        return scalar_size_;
    }

    /// Returns the length for this `Foundation`.
    ///
    /// @return Returns the current length value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize ustr::length() const noexcept {
        return scalar_size_;
    }

    /// Returns the scalar size for this `Foundation`.
    ///
    /// @return Returns the current scalar size value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize ustr::scalar_size() const noexcept {
        return scalar_size_;
    }

    /// Returns the codepoint size for this `Foundation`.
    ///
    /// @return Returns the current codepoint size value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize ustr::codepoint_size() const noexcept {
        return scalar_size_;
    }

    /// Returns the byte size for this `Foundation`.
    ///
    /// @return Returns the current byte size value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize ustr::byte_size() const noexcept {
        return bytes_.size();
    }

    /// Computes the size bytes required by the supplied values.
    ///
    /// @return Returns the current size bytes value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize ustr::size_bytes() const noexcept {
        return bytes_.size();
    }

    /// Returns an iterator to the first element in the range.
    ///
    /// @return Returns an iterator referring to the first element.
    /// @note This function does not throw exceptions.
    [[nodiscard]] ustr::CodepointIterator ustr::begin() const noexcept {
        return CodepointIterator{bytes_.data(), bytes_.data()};
    }

    /// Returns the one-past-the-end iterator for the range.
    ///
    /// @return Returns the one-past-the-end iterator.
    /// @note This function does not throw exceptions.
    [[nodiscard]] ustr::CodepointIterator ustr::end() const noexcept {
        return CodepointIterator{bytes_.data(), bytes_.data() + bytes_.size()};
    }

    /// Returns the current or globally available codepoints value.
    ///
    /// @return Returns the current codepoints value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] ustr::CodepointView ustr::codepoints() const noexcept {
        return CodepointView{bytes_.data(), bytes_.size(), scalar_size_};
    }

    /// Returns the current or globally available byte begin value.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const char *ustr::byte_begin() const noexcept {
        return bytes_.data();
    }

    /// Returns the current or globally available byte end value.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const char *ustr::byte_end() const noexcept {
        return bytes_.data() + bytes_.size();
    }

    /// Returns the current or globally available front value.
    ///
    /// @return Returns the current front value.
    /// @throws `out_of_range` if `empty()`.
    [[nodiscard]] char32_t ustr::front() const {
        if (empty()) {
            throw out_of_range{"ustr::front() called on an empty string slice."};
        }
        return decode_unchecked(bytes_.data());
    }

    /// Returns the current or globally available back value.
    ///
    /// @return Returns the current back value.
    /// @throws `out_of_range` if `empty()`.
    [[nodiscard]] char32_t ustr::back() const {
        if (empty()) {
            throw out_of_range{"ustr::back() called on an empty string slice."};
        }
        return decode_unchecked(previous_codepoint(bytes_.data(), bytes_.data() + bytes_.size()));
    }

    /// Performs the at operation for `Foundation` using the supplied arguments.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @throws `out_of_range` if `scalar_index >= scalar_size_`.
    [[nodiscard]] char32_t ustr::at(usize scalar_index) const {
        if (scalar_index >= scalar_size_) {
            throw out_of_range{format("ustr scalar index {} is out of range for size {}.", scalar_index, scalar_size_)};
        }
        return decode_unchecked(bytes_.data() + byte_index_of_unchecked(scalar_index));
    }

    /// Accesses an element of the `Foundation` by index or key.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the selected element or a reference/proxy referring to it.
    /// @pre `scalar_index < scalar_size_`; debug builds assert if this precondition is violated.
    /// @note This function does not throw exceptions.
    [[nodiscard]] char32_t ustr::operator[](usize scalar_index) const noexcept {
        assert(scalar_index < scalar_size_);
        return decode_unchecked(bytes_.data() + byte_index_of_unchecked(scalar_index));
    }

    /// Accesses an element of the `Foundation` by index or key.
    ///
    /// @param range Range of values to process.
    ///
    /// @return Returns the selected element or a reference/proxy referring to it.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] ustr ustr::operator[](USlice range) const {
        return slice(range);
    }

    /// Accesses an element of the `Foundation` by index or key.
    ///
    /// @param pattern `pattern` value used by the operation.
    ///
    /// @return Returns the selected element or a reference/proxy referring to it.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] UString ustr::operator[](USlicePattern pattern) const {
        return slice(pattern);
    }

    /// Performs the slice operation for `Foundation` using the supplied arguments.
    ///
    /// @param scalar_start `scalar_start` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] ustr ustr::slice(usize scalar_start) const {
        return slice(USlice{scalar_start});
    }

    /// Performs the slice operation for `Foundation` using the supplied arguments.
    ///
    /// @param scalar_start `scalar_start` value used by the operation.
    /// @param scalar_end `scalar_end` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] ustr ustr::slice(usize scalar_start, usize scalar_end) const {
        return slice(USlice{scalar_start, scalar_end});
    }

    /// Performs the slice operation for `Foundation` using the supplied arguments.
    ///
    /// @param range Range of values to process.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] ustr ustr::slice(USlice range) const {
        const Detail::ResolvedSlice resolved = Detail::resolve_slice(range, scalar_size_, "ustr");
        const usize first_byte = byte_index_of_unchecked(resolved.start);
        const usize last_byte = byte_index_of_unchecked(resolved.end);
        return ustr{string_view{bytes_.data() + first_byte, last_byte - first_byte}, resolved.scalar_count(), ValidatedInput{}};
    }

    /// Performs the slice operation for `Foundation` using the supplied arguments.
    ///
    /// @param pattern `pattern` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] UString ustr::slice(USlicePattern pattern) const {
        const Detail::ResolvedSlicePattern resolved = Detail::resolve_slice(pattern, scalar_size_, "ustr");
        UString result;

        for (usize group_start = resolved.range.start; group_start < resolved.range.end;) {
            const usize remaining = resolved.range.end - group_start;
            const usize group_count = std::min(resolved.grouping, remaining);
            result.append(slice(group_start, group_start + group_count));


            group_start += group_count + resolved.spread;
        }

        return result;
    }

    /// Performs the substr operation for `Foundation` using the supplied arguments.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    /// @param scalar_count Number of elements or operations to process.
    ///
    /// @return Returns the value produced by the operation.
    /// @throws `out_of_range` if `scalar_index > scalar_size_`.
    [[nodiscard]] ustr ustr::substr(usize scalar_index, usize scalar_count) const {
        if (scalar_index > scalar_size_) {
            throw out_of_range{format("ustr substr index {} is out of range for size {}.", scalar_index, scalar_size_)};
        }

        const usize actual_scalar_count = scalar_count == npos ? scalar_size_ - scalar_index : std::min(scalar_count, scalar_size_ - scalar_index);
        const usize first_byte = byte_index_of_unchecked(scalar_index);
        const usize last_byte = byte_index_of_unchecked(scalar_index + actual_scalar_count);
        return ustr{string_view{bytes_.data() + first_byte, last_byte - first_byte}, actual_scalar_count, ValidatedInput{}};
    }

    /// Finds the requested entry in the available state.
    ///
    /// @param needle `needle` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @throws `out_of_range` if `scalar_position > scalar_size_`.
    [[nodiscard]] usize ustr::find(const ustr &needle, usize scalar_position) const {
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

    /// Finds the requested entry in the available state.
    ///
    /// @param needle `needle` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] usize ustr::find(string_view needle, usize scalar_position) const {
        const ustr checked{needle};
        return find(checked, scalar_position);
    }

    /// Finds the requested entry in the available state.
    ///
    /// @param needle `needle` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] usize ustr::find(u8string_view needle, usize scalar_position) const {
        return find(as_char_view(needle), scalar_position);
    }

    /// Finds the last matching occurrence in the available range.
    ///
    /// @param needle `needle` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] usize ustr::rfind(const ustr &needle, usize scalar_position) const {
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

    /// Finds the last matching occurrence in the available range.
    ///
    /// @param needle `needle` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] usize ustr::rfind(string_view needle, usize scalar_position) const {
        const ustr checked{needle};
        return rfind(checked, scalar_position);
    }

    /// Finds the last matching occurrence in the available range.
    ///
    /// @param needle `needle` value used by the operation.
    /// @param scalar_position `scalar_position` value used by the operation.
    ///
    /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] usize ustr::rfind(u8string_view needle, usize scalar_position) const {
        return rfind(as_char_view(needle), scalar_position);
    }

    /// Reports whether contains holds for this `Foundation`.
    ///
    /// @param needle `needle` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool ustr::contains(const ustr &needle) const noexcept {
        return bytes_.find(needle.bytes_) != string_view::npos;
    }

    /// Reports whether contains holds for this `Foundation`.
    ///
    /// @param needle `needle` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] bool ustr::contains(string_view needle) const {
        return find(needle) != npos;
    }

    /// Reports whether contains holds for this `Foundation`.
    ///
    /// @param needle `needle` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] bool ustr::contains(u8string_view needle) const {
        return contains(as_char_view(needle));
    }

    /// Reports whether contains holds for this `Foundation`.
    ///
    /// @param scalar `scalar` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] bool ustr::contains(char32_t scalar) const {
        validate_scalar_or_throw(scalar);
        return contains_scalar_unchecked(scalar);
    }

    /// Reports whether the value begins with the supplied prefix.
    ///
    /// @param prefix `prefix` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool ustr::starts_with(const ustr &prefix) const noexcept {
        return bytes_.starts_with(prefix.bytes_);
    }

    /// Reports whether the value begins with the supplied prefix.
    ///
    /// @param prefix `prefix` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] bool ustr::starts_with(string_view prefix) const {
        const ustr checked{prefix};
        return starts_with(checked);
    }

    /// Reports whether the value begins with the supplied prefix.
    ///
    /// @param prefix `prefix` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] bool ustr::starts_with(u8string_view prefix) const {
        return starts_with(as_char_view(prefix));
    }

    /// Reports whether the value ends with the supplied suffix.
    ///
    /// @param suffix `suffix` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool ustr::ends_with(const ustr &suffix) const noexcept {
        return bytes_.ends_with(suffix.bytes_);
    }

    /// Reports whether the value ends with the supplied suffix.
    ///
    /// @param suffix `suffix` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] bool ustr::ends_with(string_view suffix) const {
        const ustr checked{suffix};
        return ends_with(checked);
    }

    /// Reports whether the value ends with the supplied suffix.
    ///
    /// @param suffix `suffix` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] bool ustr::ends_with(u8string_view suffix) const {
        return ends_with(as_char_view(suffix));
    }

    /// Reports whether codepoint boundary holds for this `Foundation`.
    ///
    /// @param byte_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool ustr::is_codepoint_boundary(usize byte_index) const noexcept {
        if (byte_index > bytes_.size()) {
            return false;
        }
        if (byte_index == 0 || byte_index == bytes_.size()) {
            return true;
        }
        return !is_continuation_byte(static_cast<unsigned char>(bytes_[byte_index]));
    }

    /// Performs the byte index of operation for `Foundation` using the supplied arguments.
    ///
    /// @param scalar_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @throws `out_of_range` if `scalar_index > scalar_size_`.
    [[nodiscard]] usize ustr::byte_index_of(usize scalar_index) const {
        if (scalar_index > scalar_size_) {
            throw out_of_range{format("ustr scalar index {} is out of range for size {}.", scalar_index, scalar_size_)};
        }
        return byte_index_of_unchecked(scalar_index);
    }

    /// Performs the scalar index of byte operation for `Foundation` using the supplied arguments.
    ///
    /// @param byte_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @throws `out_of_range` if `!is_codepoint_boundary(byte_index)`.
    [[nodiscard]] usize ustr::scalar_index_of_byte(usize byte_index) const {
        if (!is_codepoint_boundary(byte_index)) {
            throw out_of_range{format("ustr byte index {} is not a UTF-8 scalar boundary.", byte_index)};
        }
        return scalar_index_of_byte_unchecked(byte_index);
    }

    /// Compares the supplied values and returns their relative ordering or match result.
    ///
    /// @param other Other object used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] int ustr::compare(const ustr &other) const noexcept {
        return bytes_.compare(other.bytes_);
    }

    /// Compares the supplied values and returns their relative ordering or match result.
    ///
    /// @param other Other object used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] int ustr::compare(string_view other) const {
        const ustr checked{other};
        return compare(checked);
    }

    /// Compares the operands for equality.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool operator==(const ustr &lhs, const ustr &rhs) noexcept {
        return lhs.bytes_ == rhs.bytes_;
    }

    /// Compares the operands and produces their ordering.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns the comparison category describing the ordering of the operands.
    /// @note This function does not throw exceptions.
    [[nodiscard]] strong_ordering operator<=>(const ustr &lhs, const ustr &rhs) noexcept {
        const int result = lhs.compare(rhs);
        if (result < 0) {
            return strong_ordering::less;
        }
        if (result > 0) {
            return strong_ordering::greater;
        }
        return strong_ordering::equal;
    }

} // namespace SFT::Foundation


namespace std {

    /// Invokes the callable behavior provided by `std`.
    ///
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    std::size_t hash<SFT::Foundation::UString>::operator()(const SFT::Foundation::UString &value) const noexcept {
        return std::hash<std::string_view>{}(value.cpp_string_view());
    }

    /// Invokes the callable behavior provided by `std`.
    ///
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    std::size_t hash<SFT::Foundation::ustr>::operator()(const SFT::Foundation::ustr &value) const noexcept {
        return std::hash<std::string_view>{}(value.cpp_string_view());
    }

} // namespace std

/// Formats the supplied value into the provided formatting context.
///
/// @param value Value consumed by the operation.
/// @param ctx `ctx` value used by the operation.
///
/// @return Returns the formatting context iterator/result produced by the underlying formatter.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
fmt::format_context::iterator fmt::formatter<SFT::Foundation::UString>::format(const SFT::Foundation::UString &value, fmt::format_context &ctx) const {
    return fmt::formatter<std::string_view>::format(value.cpp_string_view(), ctx);
}

/// Formats the supplied value into the provided formatting context.
///
/// @param value Value consumed by the operation.
/// @param ctx `ctx` value used by the operation.
///
/// @return Returns the formatting context iterator/result produced by the underlying formatter.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
fmt::format_context::iterator fmt::formatter<SFT::Foundation::ustr>::format(const SFT::Foundation::ustr &value, fmt::format_context &ctx) const {
    return fmt::formatter<std::string_view>::format(value.cpp_string_view(), ctx);
}

/// Formats the supplied value into the provided formatting context.
///
/// @param value Value consumed by the operation.
/// @param ctx `ctx` value used by the operation.
///
/// @return Returns the formatting context iterator/result produced by the underlying formatter.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
fmt::format_context::iterator fmt::formatter<SFT::Foundation::USlice>::format(SFT::Foundation::USlice value, fmt::format_context &ctx) const {
    return fmt::formatter<std::string_view>::format(SFT::Foundation::Detail::display_string(value), ctx);
}

/// Formats the supplied value into the provided formatting context.
///
/// @param value Value consumed by the operation.
/// @param ctx `ctx` value used by the operation.
///
/// @return Returns the formatting context iterator/result produced by the underlying formatter.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
fmt::format_context::iterator fmt::formatter<SFT::Foundation::USlicePattern>::format(SFT::Foundation::USlicePattern value, fmt::format_context &ctx) const {
    return fmt::formatter<std::string_view>::format(SFT::Foundation::Detail::display_string(value), ctx);
}

/// Formats the supplied value into the provided formatting context.
///
/// @param value Value consumed by the operation.
/// @param ctx `ctx` value used by the operation.
///
/// @return Returns the formatting context iterator/result produced by the underlying formatter.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
fmt::format_context::iterator fmt::formatter<SFT::Foundation::UStringValidationError>::format(SFT::Foundation::UStringValidationError value, fmt::format_context &ctx) const {
    return fmt::formatter<std::string_view>::format(SFT::Foundation::to_string(value), ctx);
}

/// Formats the supplied value into the provided formatting context.
///
/// @param value Value consumed by the operation.
/// @param ctx `ctx` value used by the operation.
///
/// @return Returns the formatting context iterator/result produced by the underlying formatter.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
fmt::format_context::iterator fmt::formatter<SFT::Foundation::TextConversionError>::format(SFT::Foundation::TextConversionError value, fmt::format_context &ctx) const {
    return fmt::formatter<std::string_view>::format(SFT::Foundation::to_string(value), ctx);
}

/// Formats the supplied value into the provided formatting context.
///
/// @param value Value consumed by the operation.
/// @param ctx `ctx` value used by the operation.
///
/// @return Returns the formatting context iterator/result produced by the underlying formatter.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
fmt::format_context::iterator fmt::formatter<SFT::Foundation::UStringValidation>::format(const SFT::Foundation::UStringValidation &value, fmt::format_context &ctx) const {
    return fmt::formatter<std::string_view>::format(SFT::Foundation::Detail::display_string(value), ctx);
}

