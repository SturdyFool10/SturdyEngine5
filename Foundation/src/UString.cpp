#include <Foundation/src/UString.hpp>


namespace SFT::Foundation::Detail {

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

    ResolvedSlicePattern resolve_slice(USlicePattern slice, usize scalar_size, string_view owner) {
        const ResolvedSlice range = resolve_slice(USlice{slice.start(), slice.end_or(USlice::npos)}, scalar_size, owner);
        const usize grouping = slice.grouping();
        if (grouping == 0) {
            throw invalid_argument{format("{} slice grouping must be greater than zero.", owner)};
        }

        return ResolvedSlicePattern{.range = range, .spread = slice.spread(), .grouping = grouping};
    }

    usize bounded_c_length(const char *text, usize max_bytes) noexcept {
        if (text == nullptr || max_bytes == 0) {
            return 0;
        }
        const void *terminator = std::memchr(text, '\0', max_bytes);
        return terminator == nullptr ? max_bytes : static_cast<usize>(static_cast<const char *>(terminator) - text);
    }

    string display_string(USlice slice) {
        if (slice.has_end()) {
            return format("USlice({}..{})", slice.start(), slice.end_or(0));
        }
        return format("USlice({}..)", slice.start());
    }

    string display_string(USlicePattern pattern) {
        if (pattern.has_end()) {
            return format("USlicePattern({}..{}, group={}, spread={})", pattern.start(), pattern.end_or(0), pattern.grouping(), pattern.spread());
        }
        return format("USlicePattern({}.., group={}, spread={})", pattern.start(), pattern.grouping(), pattern.spread());
    }

    string display_string(const UStringValidation &validation) {
        if (validation.valid) {
            return format("UStringValidation(valid, {} scalars)", validation.scalar_count);
        }
        return format("UStringValidation(invalid: {} at byte {}, {} scalars)", to_string(validation.error), validation.byte_index, validation.scalar_count);
    }

    u32string to_utf32(UString::CodepointView codepoints) {
        u32string result;
        result.reserve(codepoints.size());
        for (char32_t scalar : codepoints) {
            result.push_back(scalar);
        }
        return result;
    }

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

    char32_t UString::CodepointIterator::operator*() const noexcept {
        assert(current_ != nullptr);
        return UString::decode_unchecked(current_);
    }

    UString::CodepointIterator &UString::CodepointIterator::operator++() noexcept {
        assert(current_ != nullptr);
        current_ += UString::encoded_length_unchecked(current_);
        return *this;
    }

    UString::CodepointIterator UString::CodepointIterator::operator++(int) noexcept {
        CodepointIterator copy = *this;
        ++(*this);
        return copy;
    }

    UString::CodepointIterator &UString::CodepointIterator::operator--() noexcept {
        assert(begin_ != nullptr);
        assert(current_ != nullptr);
        assert(current_ > begin_);
        do {
            --current_;
        } while (current_ > begin_ && UString::is_continuation_byte(static_cast<unsigned char>(*current_)));
        return *this;
    }

    UString::CodepointIterator UString::CodepointIterator::operator--(int) noexcept {
        CodepointIterator copy = *this;
        --(*this);
        return copy;
    }

    bool operator==(UString::CodepointIterator lhs, UString::CodepointIterator rhs) noexcept {
        return lhs.begin_ == rhs.begin_ && lhs.current_ == rhs.current_;
    }

    UString &UString::operator=(UString other) noexcept {
        swap(other);
        return *this;
    }

    UString &UString::operator=(string_view text) {
        return assign(text);
    }

    UString &UString::operator=(const char *text) {
        if (text == nullptr) {
            throw invalid_argument{"UString cannot be assigned from a null pointer."};
        }
        return assign(string_view{text});
    }

    UString &UString::operator=(u8string_view text) {
        return assign(as_char_view(text));
    }

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

    bool UString::is_valid_utf8(string_view text) noexcept {
        return static_cast<bool>(validate_utf8(text));
    }

    optional<UString> UString::try_from_utf8(string_view text) {
        const UStringValidation validation = validate_utf8(text);
        if (!validation) {
            return nullopt;
        }
        return UString{text, validation.scalar_count, ValidatedInput{}};
    }

    optional<UString> UString::try_from_utf8(u8string_view text) {
        return try_from_utf8(as_char_view(text));
    }

    string UString::cpp_string() const {
        return string{cpp_string_view()};
    }

    expected<string, TextConversionError> UString::to_std_string() const {
        if (!is_ascii()) {
            return unexpected(TextConversionError::NonAscii);
        }
        return string{cpp_string_view()};
    }

    string UString::to_std_string_unchecked() const {
        if (!is_ascii()) {
            std::terminate();
        }
        return string{cpp_string_view()};
    }

    u8string_view UString::cpp_u8string_view() const noexcept {
        return u8string_view{reinterpret_cast<const char8_t *>(storage_data()), byte_size_};
    }

    u8string UString::cpp_u8string() const {
        return u8string{cpp_u8string_view()};
    }

    wstring UString::cpp_wstring() const {
        if constexpr (sizeof(wchar_t) == 2) {
            const u16string units = cpp_u16string();
            return wstring{units.begin(), units.end()};
        } else {
            const u32string units = cpp_u32string();
            return wstring{units.begin(), units.end()};
        }
    }

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

    UString &UString::assign(u8string_view text) {
        return assign(as_char_view(text));
    }

    UString &UString::assign(const char *text) {
        if (text == nullptr) {
            throw invalid_argument{"UString cannot be assigned from a null pointer."};
        }
        return assign(string_view{text});
    }

    UString &UString::append(string_view text) {
        const UStringValidation validation = validate_or_throw(text);
        return append_validated(text, validation.scalar_count);
    }

    UString &UString::append(u8string_view text) {
        return append(as_char_view(text));
    }

    UString &UString::append(const char *text) {
        if (text == nullptr) {
            throw invalid_argument{"UString cannot append a null pointer."};
        }
        return append(string_view{text});
    }

    UString &UString::append(char32_t scalar) {
        char buffer[4]{};
        const usize encoded_size = encode_scalar_or_throw(scalar, buffer);
        append_encoded_scalar(buffer, encoded_size);
        return *this;
    }

    UString &UString::operator+=(string_view text) {
        return append(text);
    }

    UString &UString::operator+=(const char *text) {
        return append(text);
    }

    UString &UString::operator+=(char32_t scalar) {
        return append(scalar);
    }

    UString &UString::insert(usize scalar_index, const UString &text) {
        return insert_validated(scalar_index, text.cpp_string_view(), text.scalar_size_);
    }

    UString &UString::insert(usize scalar_index, string_view text) {
        const UStringValidation validation = validate_or_throw(text);
        return insert_validated(scalar_index, text, validation.scalar_count);
    }

    UString &UString::insert(usize scalar_index, u8string_view text) {
        return insert(scalar_index, as_char_view(text));
    }

    UString &UString::insert(usize scalar_index, char32_t scalar) {
        char buffer[4]{};
        const usize encoded_size = encode_scalar_or_throw(scalar, buffer);
        return insert_validated(scalar_index, string_view{buffer, encoded_size}, 1);
    }

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

    UString &UString::replace(usize scalar_index, usize scalar_count, const UString &replacement) {
        return replace_validated(scalar_index, scalar_count, replacement.cpp_string_view(), replacement.scalar_size_);
    }

    UString &UString::replace(usize scalar_index, usize scalar_count, string_view replacement) {
        const UStringValidation validation = validate_or_throw(replacement);
        return replace_validated(scalar_index, scalar_count, replacement, validation.scalar_count);
    }

    UString &UString::replace(usize scalar_index, usize scalar_count, u8string_view replacement) {
        return replace(scalar_index, scalar_count, as_char_view(replacement));
    }

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

    UString UString::substr(usize scalar_index, usize scalar_count) const {
        if (scalar_index > scalar_size_) {
            throw out_of_range{format("UString substr index {} is out of range for size {}.", scalar_index, scalar_size_)};
        }

        const usize actual_scalar_count = scalar_count == npos ? scalar_size_ - scalar_index : std::min(scalar_count, scalar_size_ - scalar_index);
        const usize first_byte = byte_index_of_unchecked(scalar_index);
        const usize last_byte = byte_index_of_unchecked(scalar_index + actual_scalar_count);
        return UString{string_view{storage_data() + first_byte, last_byte - first_byte}, actual_scalar_count, ValidatedInput{}};
    }

    usize UString::find(const UString &needle, usize scalar_position) const {
        return find_validated(needle.cpp_string_view(), scalar_position);
    }

    usize UString::find(string_view needle, usize scalar_position) const {
        validate_or_throw(needle);
        return find_validated(needle, scalar_position);
    }

    usize UString::find(u8string_view needle, usize scalar_position) const {
        return find(as_char_view(needle), scalar_position);
    }

    usize UString::rfind(const UString &needle, usize scalar_position) const {
        return rfind_validated(needle.cpp_string_view(), scalar_position);
    }

    usize UString::rfind(string_view needle, usize scalar_position) const {
        validate_or_throw(needle);
        return rfind_validated(needle, scalar_position);
    }

    usize UString::rfind(u8string_view needle, usize scalar_position) const {
        return rfind(as_char_view(needle), scalar_position);
    }

    bool UString::contains(const UString &needle) const noexcept {
        return cpp_string_view().find(needle.cpp_string_view()) != string_view::npos;
    }

    bool UString::contains(string_view needle) const {
        return find(needle) != npos;
    }

    bool UString::contains(u8string_view needle) const {
        return contains(as_char_view(needle));
    }

    bool UString::contains(char32_t scalar) const {
        validate_scalar_or_throw(scalar);
        return contains_scalar_unchecked(scalar);
    }

    bool UString::starts_with(const UString &prefix) const noexcept {
        return cpp_string_view().starts_with(prefix.cpp_string_view());
    }

    bool UString::starts_with(string_view prefix) const {
        validate_or_throw(prefix);
        return cpp_string_view().starts_with(prefix);
    }

    bool UString::starts_with(u8string_view prefix) const {
        return starts_with(as_char_view(prefix));
    }

    bool UString::ends_with(const UString &suffix) const noexcept {
        return cpp_string_view().ends_with(suffix.cpp_string_view());
    }

    bool UString::ends_with(string_view suffix) const {
        validate_or_throw(suffix);
        return cpp_string_view().ends_with(suffix);
    }

    bool UString::ends_with(u8string_view suffix) const {
        return ends_with(as_char_view(suffix));
    }

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

    usize UString::find_first_of(string_view scalars, usize scalar_position) const {
        return find_first_of(UString{scalars}, scalar_position);
    }

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

    usize UString::find_first_not_of(string_view scalars, usize scalar_position) const {
        return find_first_not_of(UString{scalars}, scalar_position);
    }

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

    usize UString::find_last_of(string_view scalars, usize scalar_position) const {
        return find_last_of(UString{scalars}, scalar_position);
    }

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

    usize UString::find_last_not_of(string_view scalars, usize scalar_position) const {
        return find_last_not_of(UString{scalars}, scalar_position);
    }

    bool UString::is_codepoint_boundary(usize byte_index) const noexcept {
        if (byte_index > byte_size_) {
            return false;
        }
        if (byte_index == 0 || byte_index == byte_size_) {
            return true;
        }
        return !is_continuation_byte(static_cast<unsigned char>(storage_data()[byte_index]));
    }

    usize UString::byte_index_of(usize scalar_index) const {
        if (scalar_index > scalar_size_) {
            throw out_of_range{format("UString scalar index {} is out of range for size {}.", scalar_index, scalar_size_)};
        }
        return byte_index_of_unchecked(scalar_index);
    }

    usize UString::scalar_index_of_byte(usize byte_index) const {
        if (!is_codepoint_boundary(byte_index)) {
            throw out_of_range{format("UString byte index {} is not a UTF-8 scalar boundary.", byte_index)};
        }
        return scalar_index_of_byte_unchecked(byte_index);
    }

    int UString::compare(const UString &other) const noexcept {
        return cpp_string_view().compare(other.cpp_string_view());
    }

    int UString::compare(string_view other) const {
        validate_or_throw(other);
        return cpp_string_view().compare(other);
    }

    void UString::swap(UString &other) noexcept {
        using std::swap;
        swap(byte_size_, other.byte_size_);
        swap(scalar_size_, other.scalar_size_);
        swap(capacity_, other.capacity_);
        swap(heap_, other.heap_);
        swap(small_, other.small_);
    }

    bool operator==(const UString &lhs, const UString &rhs) noexcept {
        return lhs.cpp_string_view() == rhs.cpp_string_view();
    }

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

    UString operator+(UString lhs, const UString &rhs) {
        lhs += rhs;
        return lhs;
    }

    UString operator+(UString lhs, string_view rhs) {
        lhs += rhs;
        return lhs;
    }

    UString operator+(string_view lhs, const UString &rhs) {
        UString result{lhs};
        result += rhs;
        return result;
    }

    UString operator+(UString lhs, char32_t rhs) {
        lhs += rhs;
        return lhs;
    }

    string_view UString::as_char_view(u8string_view text) noexcept {
        return string_view{reinterpret_cast<const char *>(text.data()), text.size()};
    }

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

    usize UString::encoded_length_unchecked(const char *text) noexcept {
        return encoded_length_from_lead(static_cast<unsigned char>(*text));
    }

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

    const char *UString::previous_codepoint(const char *begin, const char *cursor) noexcept {
        do {
            --cursor;
        } while (cursor > begin && is_continuation_byte(static_cast<unsigned char>(*cursor)));
        return cursor;
    }

    char *UString::allocate_buffer(usize capacity) {
        if (capacity > max_size() || capacity == std::numeric_limits<usize>::max()) {
            throw length_error{"UString capacity exceeds max_size()."};
        }
        char *buffer = new char[capacity + 1];
        buffer[0] = '\0';
        return buffer;
    }

    const char *UString::storage_data() const noexcept {
        return heap_ == nullptr ? small_.data() : heap_;
    }

    char *UString::mutable_data() noexcept {
        return heap_ == nullptr ? small_.data() : heap_;
    }

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

    usize UString::checked_total_byte_size(usize additional_bytes) const {
        if (additional_bytes > max_size() - byte_size_) {
            throw length_error{"UString operation would exceed max_size()."};
        }
        return byte_size_ + additional_bytes;
    }

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

    void UString::append_validated_unaliased(string_view text, usize scalar_count) {
        const usize old_byte_size = byte_size_;
        const usize new_byte_size = checked_total_byte_size(text.size());
        ensure_capacity(new_byte_size);

        std::memcpy(mutable_data() + old_byte_size, text.data(), text.size());
        byte_size_ = new_byte_size;
        scalar_size_ += scalar_count;
        mutable_data()[byte_size_] = '\0';
    }

    void UString::append_encoded_scalar(const char *encoded, usize encoded_size) {
        const usize old_byte_size = byte_size_;
        const usize new_byte_size = checked_total_byte_size(encoded_size);
        ensure_capacity(new_byte_size);

        std::memcpy(mutable_data() + old_byte_size, encoded, encoded_size);
        byte_size_ = new_byte_size;
        ++scalar_size_;
        mutable_data()[byte_size_] = '\0';
    }

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

    usize UString::byte_index_of_unchecked(usize scalar_index) const noexcept {
        const char *cursor = storage_data();
        for (usize index = 0; index < scalar_index; ++index) {
            cursor += encoded_length_unchecked(cursor);
        }
        return static_cast<usize>(cursor - storage_data());
    }

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

    UString::CodepointIterator UString::iterator_at_unchecked(usize scalar_index) const noexcept {
        const char *cursor = storage_data() + byte_index_of_unchecked(scalar_index);
        return CodepointIterator{storage_data(), cursor};
    }

    bool UString::contains_scalar_unchecked(char32_t scalar) const noexcept {
        for (char32_t current : codepoints()) {
            if (current == scalar) {
                return true;
            }
        }
        return false;
    }

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

    void swap(UString &lhs, UString &rhs) noexcept {
        lhs.swap(rhs);
    }

    UStringValidation ustr::validate_utf8(string_view text) noexcept {
        return UString::validate_utf8(text);
    }

    bool ustr::is_valid_utf8(string_view text) noexcept {
        return UString::is_valid_utf8(text);
    }

    ustr ustr::from_c_str(const char *buffer, usize max_bytes) {
        if (buffer == nullptr && max_bytes != 0) {
            throw invalid_argument{"ustr::from_c_str() received a null pointer with a non-zero size."};
        }
        const usize length = Detail::bounded_c_length(buffer, max_bytes);
        return ustr{string_view{buffer == nullptr ? "" : buffer, length}};
    }

    const char *ustr::data() const noexcept {
        return bytes_.data();
    }

    string_view ustr::cpp_string_view() const noexcept {
        return bytes_;
    }

    string_view ustr::cpp_bytes() const noexcept {
        return bytes_;
    }

    string ustr::cpp_string() const {
        return string{bytes_};
    }

    expected<string, TextConversionError> ustr::to_std_string() const {
        if (!is_ascii()) {
            return unexpected(TextConversionError::NonAscii);
        }
        return string{bytes_};
    }

    string ustr::to_std_string_unchecked() const {
        if (!is_ascii()) {
            std::terminate();
        }
        return string{bytes_};
    }

    UString ustr::to_owned() const {
        return UString{*this};
    }

    u8string_view ustr::cpp_u8string_view() const noexcept {
        return u8string_view{reinterpret_cast<const char8_t *>(bytes_.data()), bytes_.size()};
    }

    u8string ustr::cpp_u8string() const {
        return u8string{cpp_u8string_view()};
    }

    wstring ustr::cpp_wstring() const {
        if constexpr (sizeof(wchar_t) == 2) {
            const u16string units = cpp_u16string();
            return wstring{units.begin(), units.end()};
        } else {
            const u32string units = cpp_u32string();
            return wstring{units.begin(), units.end()};
        }
    }

    void ustr::assign_validated(string_view text) {
        const UStringValidation validation = validate_or_throw(text);
        bytes_ = text;
        scalar_size_ = validation.scalar_count;
    }

    string_view ustr::as_char_view(u8string_view text) noexcept {
        return string_view{reinterpret_cast<const char *>(text.data()), text.size()};
    }

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

    usize ustr::encoded_length_unchecked(const char *text) noexcept {
        return encoded_length_from_lead(static_cast<unsigned char>(*text));
    }

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

    const char *ustr::previous_codepoint(const char *begin, const char *cursor) noexcept {
        do {
            --cursor;
        } while (cursor > begin && is_continuation_byte(static_cast<unsigned char>(*cursor)));
        return cursor;
    }

    usize ustr::byte_index_of_unchecked(usize scalar_index) const noexcept {
        const char *cursor = bytes_.data();
        for (usize index = 0; index < scalar_index; ++index) {
            cursor += encoded_length_unchecked(cursor);
        }
        return static_cast<usize>(cursor - bytes_.data());
    }

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

    bool ustr::contains_scalar_unchecked(char32_t scalar) const noexcept {
        for (char32_t current : codepoints()) {
            if (current == scalar) {
                return true;
            }
        }
        return false;
    }

    UString &UString::operator=(const ustr &text) {
        return assign(text);
    }

    ustr UString::as_ustr() const & noexcept {
        return ustr{cpp_string_view(), scalar_size_, ustr::ValidatedInput{}};
    }

    ustr UString::slice() const & noexcept {
        return as_ustr();
    }

    ustr UString::slice(usize scalar_start) const & {
        return slice(USlice{scalar_start});
    }

    ustr UString::slice(usize scalar_start, usize scalar_end) const & {
        return slice(USlice{scalar_start, scalar_end});
    }

    ustr UString::slice(USlice range) const & {
        const Detail::ResolvedSlice resolved = Detail::resolve_slice(range, scalar_size_, "UString");
        const usize first_byte = byte_index_of_unchecked(resolved.start);
        const usize last_byte = byte_index_of_unchecked(resolved.end);
        return ustr{string_view{storage_data() + first_byte, last_byte - first_byte}, resolved.scalar_count(), ustr::ValidatedInput{}};
    }

    UString UString::slice(USlicePattern pattern) const {
        return as_ustr().slice(pattern);
    }

    u16string UString::cpp_u16string() const {
        return Detail::to_utf16(codepoints());
    }

    u32string UString::cpp_u32string() const {
        return Detail::to_utf32(codepoints());
    }

    u16string ustr::cpp_u16string() const {
        return Detail::to_utf16(codepoints());
    }

    u32string ustr::cpp_u32string() const {
        return Detail::to_utf32(codepoints());
    }

    ustr UString::operator[](USlice range) const & {
        return slice(range);
    }

    UString UString::operator[](USlicePattern pattern) const {
        return slice(pattern);
    }

    UString &UString::assign(const ustr &text) {
        assign_validated_unaliased(text.cpp_string_view(), text.scalar_size());
        return *this;
    }

    UString &UString::append(const ustr &text) {
        return append_validated(text.cpp_string_view(), text.scalar_size());
    }

    UString &UString::operator+=(const ustr &text) {
        return append(text);
    }

    UString &UString::insert(usize scalar_index, const ustr &text) {
        return insert_validated(scalar_index, text.cpp_string_view(), text.scalar_size());
    }

    UString &UString::replace(usize scalar_index, usize scalar_count, const ustr &replacement) {
        return replace_validated(scalar_index, scalar_count, replacement.cpp_string_view(), replacement.scalar_size());
    }

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

    usize UString::find(const ustr &needle, usize scalar_position) const {
        return find_validated(needle.cpp_string_view(), scalar_position);
    }

    usize UString::rfind(const ustr &needle, usize scalar_position) const {
        return rfind_validated(needle.cpp_string_view(), scalar_position);
    }

    bool UString::contains(const ustr &needle) const noexcept {
        return cpp_string_view().find(needle.cpp_string_view()) != string_view::npos;
    }

    bool UString::starts_with(const ustr &prefix) const noexcept {
        return cpp_string_view().starts_with(prefix.cpp_string_view());
    }

    bool UString::ends_with(const ustr &suffix) const noexcept {
        return cpp_string_view().ends_with(suffix.cpp_string_view());
    }

    int UString::compare(const ustr &other) const noexcept {
        return cpp_string_view().compare(other.cpp_string_view());
    }

    bool operator==(const UString &lhs, const ustr &rhs) noexcept {
        return lhs.cpp_string_view() == rhs.cpp_string_view();
    }

    bool operator==(const ustr &lhs, const UString &rhs) noexcept {
        return lhs.cpp_string_view() == rhs.cpp_string_view();
    }

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

    UString operator+(UString lhs, const ustr &rhs) {
        lhs += rhs;
        return lhs;
    }

    UString operator+(const ustr &lhs, const UString &rhs) {
        UString result{lhs};
        result += rhs;
        return result;
    }

    std::ostream &operator<<(std::ostream &os, const UString &value) {
        return os << value.cpp_string_view();
    }

    std::ostream &operator<<(std::ostream &os, const ustr &value) {
        return os << value.cpp_string_view();
    }

    std::ostream &operator<<(std::ostream &os, USlice value) {
        return os << Detail::display_string(value);
    }

    std::ostream &operator<<(std::ostream &os, USlicePattern value) {
        return os << Detail::display_string(value);
    }

    std::ostream &operator<<(std::ostream &os, UStringValidationError value) {
        return os << to_string(value);
    }

    std::ostream &operator<<(std::ostream &os, TextConversionError value) {
        return os << to_string(value);
    }

    std::ostream &operator<<(std::ostream &os, const UStringValidation &value) {
        return os << Detail::display_string(value);
    }

} // namespace SFT::Foundation

SFT::Foundation::ustr operator""_ustr(const char *text, size_t byte_count) {
    return SFT::Foundation::ustr{string_view{text, byte_count}};
}

SFT::Foundation::ustr operator""_ustr(const char8_t *text, size_t byte_count) {
    return SFT::Foundation::ustr{u8string_view{text, byte_count}};
}


namespace SFT::Foundation {

    UString::UString() noexcept {
        small_[0] = '\0';
    }

    UString::UString(const char *text) {
        if (text == nullptr) {
            throw invalid_argument{"UString cannot be constructed from a null pointer."};
        }
        assign(string_view{text});
    }

    UString::UString(const char *text, usize byte_count) {
        if (text == nullptr && byte_count != 0) {
            throw invalid_argument{"UString cannot be constructed from a null pointer and non-zero size."};
        }
        assign(string_view{text == nullptr ? "" : text, byte_count});
    }

    UString::UString(string_view text) {
        assign(text);
    }

    UString::UString(const string &text) noexcept {
        assign(string_view{text});
    }

    UString::UString(u8string_view text) {
        assign(as_char_view(text));
    }

    UString::UString(const UString &other) {
        assign_validated_unaliased(other.cpp_string_view(), other.scalar_size_);
    }

    UString::UString(UString &&other) noexcept {
        move_from(std::move(other));
    }

    UString::~UString() noexcept {
        delete[] heap_;
    }

    UString::UString(string_view text, usize scalar_count, ValidatedInput) {
        assign_validated_unaliased(text, scalar_count);
    }

    ustr::ustr(const char *text) {
        if (text == nullptr) {
            throw invalid_argument{"ustr cannot be constructed from a null pointer."};
        }
        assign_validated(string_view{text});
    }

    ustr::ustr(const char *text, usize byte_count) {
        if (text == nullptr && byte_count != 0) {
            throw invalid_argument{"ustr cannot be constructed from a null pointer and non-zero size."};
        }
        assign_validated(string_view{text == nullptr ? "" : text, byte_count});
    }

    ustr::ustr(string_view text) {
        assign_validated(text);
    }

    ustr::ustr(const string &text) {
        assign_validated(string_view{text});
    }

    ustr::ustr(u8string_view text) {
        assign_validated(as_char_view(text));
    }

    ustr::ustr(const UString &text) noexcept
        : bytes_(text.cpp_string_view()), scalar_size_(text.scalar_size()) {
    }

    UString::UString(const ustr &text) {
        assign_validated_unaliased(text.cpp_string_view(), text.scalar_size());
    }

    UString::operator ustr() const & noexcept {
        return as_ustr();
    }

} // namespace SFT::Foundation


namespace SFT::Foundation {

    [[nodiscard]] UString UString::from_c_str(const char *buffer) {
        if (buffer == nullptr) {
            throw invalid_argument{"UString::from_c_str() received a null pointer."};
        }
        return UString{string_view{buffer}};
    }

    [[nodiscard]] UString UString::from_c_str(const char *buffer, usize max_bytes) {
        if (buffer == nullptr && max_bytes != 0) {
            throw invalid_argument{"UString::from_c_str() received a null pointer with a non-zero size."};
        }
        const usize length = Detail::bounded_c_length(buffer, max_bytes);
        return UString{string_view{buffer == nullptr ? "" : buffer, length}};
    }

    [[nodiscard]] optional<UString> UString::try_from_c_str(const char *buffer, usize max_bytes) {
        if (buffer == nullptr) {
            return max_bytes == 0 ? optional<UString>{UString{}} : nullopt;
        }
        const usize length = Detail::bounded_c_length(buffer, max_bytes);
        return try_from_utf8(string_view{buffer, length});
    }

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

    [[nodiscard]] UString UString::from_utf32(u32string_view text) {
        return from_codepoints(text);
    }

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

    [[nodiscard]] const char *UString::data() const noexcept {
        return storage_data();
    }

    [[nodiscard]] const char *UString::c_str() const noexcept {
        return storage_data();
    }

    [[nodiscard]] string_view UString::cpp_string_view() const noexcept {
        return string_view{storage_data(), byte_size_};
    }

    [[nodiscard]] string_view UString::cpp_bytes() const noexcept {
        return cpp_string_view();
    }

    [[nodiscard]] UString::operator string_view() const noexcept {
        return cpp_string_view();
    }

    [[nodiscard]] bool UString::empty() const noexcept {
        return byte_size_ == 0;
    }

    [[nodiscard]] bool UString::is_ascii() const noexcept {
        return std::ranges::all_of(cpp_string_view(), [](char byte) {
            return static_cast<unsigned char>(byte) <= 0x7Fu;
        });
    }

    [[nodiscard]] usize UString::size() const noexcept {
        return scalar_size_;
    }

    [[nodiscard]] usize UString::length() const noexcept {
        return scalar_size_;
    }

    [[nodiscard]] usize UString::scalar_size() const noexcept {
        return scalar_size_;
    }

    [[nodiscard]] usize UString::codepoint_size() const noexcept {
        return scalar_size_;
    }

    [[nodiscard]] usize UString::byte_size() const noexcept {
        return byte_size_;
    }

    [[nodiscard]] usize UString::size_bytes() const noexcept {
        return byte_size_;
    }

    [[nodiscard]] usize UString::capacity() const noexcept {
        return capacity_;
    }

    [[nodiscard]] bool UString::is_small() const noexcept {
        return heap_ == nullptr;
    }

    [[nodiscard]] UString::CodepointIterator UString::begin() const noexcept {
        return CodepointIterator{storage_data(), storage_data()};
    }

    [[nodiscard]] UString::CodepointIterator UString::end() const noexcept {
        return CodepointIterator{storage_data(), storage_data() + byte_size_};
    }

    [[nodiscard]] UString::CodepointView UString::codepoints() const noexcept {
        return CodepointView{storage_data(), byte_size_, scalar_size_};
    }

    [[nodiscard]] const char *UString::byte_begin() const noexcept {
        return storage_data();
    }

    [[nodiscard]] const char *UString::byte_end() const noexcept {
        return storage_data() + byte_size_;
    }

    [[nodiscard]] char32_t UString::front() const {
        if (empty()) {
            throw out_of_range{"UString::front() called on an empty string."};
        }
        return decode_unchecked(storage_data());
    }

    [[nodiscard]] char32_t UString::back() const {
        if (empty()) {
            throw out_of_range{"UString::back() called on an empty string."};
        }
        return decode_unchecked(previous_codepoint(storage_data(), storage_data() + byte_size_));
    }

    [[nodiscard]] char32_t UString::at(usize scalar_index) const {
        if (scalar_index >= scalar_size_) {
            throw out_of_range{format("UString scalar index {} is out of range for size {}.", scalar_index, scalar_size_)};
        }
        return decode_unchecked(storage_data() + byte_index_of(scalar_index));
    }

    [[nodiscard]] char32_t UString::operator[](usize scalar_index) const noexcept {
        assert(scalar_index < scalar_size_);
        return decode_unchecked(storage_data() + byte_index_of_unchecked(scalar_index));
    }

    void UString::clear() noexcept {
        byte_size_ = 0;
        scalar_size_ = 0;
        mutable_data()[0] = '\0';
    }

    void UString::reserve(usize requested_capacity) {
        if (requested_capacity > max_size()) {
            throw length_error{"UString capacity request exceeds max_size()."};
        }
        ensure_capacity(requested_capacity);
    }

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

    UString &UString::append(const UString &text) {
        return append_validated(text.cpp_string_view(), text.scalar_size_);
    }

    UString &UString::push_back(char32_t scalar) {
        return append(scalar);
    }

    void UString::pop_back() {
        if (empty()) {
            throw out_of_range{"UString::pop_back() called on an empty string."};
        }

        const char *last = previous_codepoint(storage_data(), storage_data() + byte_size_);
        byte_size_ = static_cast<usize>(last - storage_data());
        --scalar_size_;
        mutable_data()[byte_size_] = '\0';
    }

    UString &UString::operator+=(const UString &text) {
        return append(text);
    }

    [[nodiscard]] ustr::operator string_view() const noexcept {
        return bytes_;
    }

    [[nodiscard]] bool ustr::empty() const noexcept {
        return bytes_.empty();
    }

    [[nodiscard]] bool ustr::is_ascii() const noexcept {
        return std::ranges::all_of(bytes_, [](char byte) {
            return static_cast<unsigned char>(byte) <= 0x7Fu;
        });
    }

    [[nodiscard]] usize ustr::size() const noexcept {
        return scalar_size_;
    }

    [[nodiscard]] usize ustr::length() const noexcept {
        return scalar_size_;
    }

    [[nodiscard]] usize ustr::scalar_size() const noexcept {
        return scalar_size_;
    }

    [[nodiscard]] usize ustr::codepoint_size() const noexcept {
        return scalar_size_;
    }

    [[nodiscard]] usize ustr::byte_size() const noexcept {
        return bytes_.size();
    }

    [[nodiscard]] usize ustr::size_bytes() const noexcept {
        return bytes_.size();
    }

    [[nodiscard]] ustr::CodepointIterator ustr::begin() const noexcept {
        return CodepointIterator{bytes_.data(), bytes_.data()};
    }

    [[nodiscard]] ustr::CodepointIterator ustr::end() const noexcept {
        return CodepointIterator{bytes_.data(), bytes_.data() + bytes_.size()};
    }

    [[nodiscard]] ustr::CodepointView ustr::codepoints() const noexcept {
        return CodepointView{bytes_.data(), bytes_.size(), scalar_size_};
    }

    [[nodiscard]] const char *ustr::byte_begin() const noexcept {
        return bytes_.data();
    }

    [[nodiscard]] const char *ustr::byte_end() const noexcept {
        return bytes_.data() + bytes_.size();
    }

    [[nodiscard]] char32_t ustr::front() const {
        if (empty()) {
            throw out_of_range{"ustr::front() called on an empty string slice."};
        }
        return decode_unchecked(bytes_.data());
    }

    [[nodiscard]] char32_t ustr::back() const {
        if (empty()) {
            throw out_of_range{"ustr::back() called on an empty string slice."};
        }
        return decode_unchecked(previous_codepoint(bytes_.data(), bytes_.data() + bytes_.size()));
    }

    [[nodiscard]] char32_t ustr::at(usize scalar_index) const {
        if (scalar_index >= scalar_size_) {
            throw out_of_range{format("ustr scalar index {} is out of range for size {}.", scalar_index, scalar_size_)};
        }
        return decode_unchecked(bytes_.data() + byte_index_of_unchecked(scalar_index));
    }

    [[nodiscard]] char32_t ustr::operator[](usize scalar_index) const noexcept {
        assert(scalar_index < scalar_size_);
        return decode_unchecked(bytes_.data() + byte_index_of_unchecked(scalar_index));
    }

    [[nodiscard]] ustr ustr::operator[](USlice range) const {
        return slice(range);
    }

    [[nodiscard]] UString ustr::operator[](USlicePattern pattern) const {
        return slice(pattern);
    }

    [[nodiscard]] ustr ustr::slice(usize scalar_start) const {
        return slice(USlice{scalar_start});
    }

    [[nodiscard]] ustr ustr::slice(usize scalar_start, usize scalar_end) const {
        return slice(USlice{scalar_start, scalar_end});
    }

    [[nodiscard]] ustr ustr::slice(USlice range) const {
        const Detail::ResolvedSlice resolved = Detail::resolve_slice(range, scalar_size_, "ustr");
        const usize first_byte = byte_index_of_unchecked(resolved.start);
        const usize last_byte = byte_index_of_unchecked(resolved.end);
        return ustr{string_view{bytes_.data() + first_byte, last_byte - first_byte}, resolved.scalar_count(), ValidatedInput{}};
    }

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

    [[nodiscard]] ustr ustr::substr(usize scalar_index, usize scalar_count) const {
        if (scalar_index > scalar_size_) {
            throw out_of_range{format("ustr substr index {} is out of range for size {}.", scalar_index, scalar_size_)};
        }

        const usize actual_scalar_count = scalar_count == npos ? scalar_size_ - scalar_index : std::min(scalar_count, scalar_size_ - scalar_index);
        const usize first_byte = byte_index_of_unchecked(scalar_index);
        const usize last_byte = byte_index_of_unchecked(scalar_index + actual_scalar_count);
        return ustr{string_view{bytes_.data() + first_byte, last_byte - first_byte}, actual_scalar_count, ValidatedInput{}};
    }

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

    [[nodiscard]] usize ustr::find(string_view needle, usize scalar_position) const {
        const ustr checked{needle};
        return find(checked, scalar_position);
    }

    [[nodiscard]] usize ustr::find(u8string_view needle, usize scalar_position) const {
        return find(as_char_view(needle), scalar_position);
    }

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

    [[nodiscard]] usize ustr::rfind(string_view needle, usize scalar_position) const {
        const ustr checked{needle};
        return rfind(checked, scalar_position);
    }

    [[nodiscard]] usize ustr::rfind(u8string_view needle, usize scalar_position) const {
        return rfind(as_char_view(needle), scalar_position);
    }

    [[nodiscard]] bool ustr::contains(const ustr &needle) const noexcept {
        return bytes_.find(needle.bytes_) != string_view::npos;
    }

    [[nodiscard]] bool ustr::contains(string_view needle) const {
        return find(needle) != npos;
    }

    [[nodiscard]] bool ustr::contains(u8string_view needle) const {
        return contains(as_char_view(needle));
    }

    [[nodiscard]] bool ustr::contains(char32_t scalar) const {
        validate_scalar_or_throw(scalar);
        return contains_scalar_unchecked(scalar);
    }

    [[nodiscard]] bool ustr::starts_with(const ustr &prefix) const noexcept {
        return bytes_.starts_with(prefix.bytes_);
    }

    [[nodiscard]] bool ustr::starts_with(string_view prefix) const {
        const ustr checked{prefix};
        return starts_with(checked);
    }

    [[nodiscard]] bool ustr::starts_with(u8string_view prefix) const {
        return starts_with(as_char_view(prefix));
    }

    [[nodiscard]] bool ustr::ends_with(const ustr &suffix) const noexcept {
        return bytes_.ends_with(suffix.bytes_);
    }

    [[nodiscard]] bool ustr::ends_with(string_view suffix) const {
        const ustr checked{suffix};
        return ends_with(checked);
    }

    [[nodiscard]] bool ustr::ends_with(u8string_view suffix) const {
        return ends_with(as_char_view(suffix));
    }

    [[nodiscard]] bool ustr::is_codepoint_boundary(usize byte_index) const noexcept {
        if (byte_index > bytes_.size()) {
            return false;
        }
        if (byte_index == 0 || byte_index == bytes_.size()) {
            return true;
        }
        return !is_continuation_byte(static_cast<unsigned char>(bytes_[byte_index]));
    }

    [[nodiscard]] usize ustr::byte_index_of(usize scalar_index) const {
        if (scalar_index > scalar_size_) {
            throw out_of_range{format("ustr scalar index {} is out of range for size {}.", scalar_index, scalar_size_)};
        }
        return byte_index_of_unchecked(scalar_index);
    }

    [[nodiscard]] usize ustr::scalar_index_of_byte(usize byte_index) const {
        if (!is_codepoint_boundary(byte_index)) {
            throw out_of_range{format("ustr byte index {} is not a UTF-8 scalar boundary.", byte_index)};
        }
        return scalar_index_of_byte_unchecked(byte_index);
    }

    [[nodiscard]] int ustr::compare(const ustr &other) const noexcept {
        return bytes_.compare(other.bytes_);
    }

    [[nodiscard]] int ustr::compare(string_view other) const {
        const ustr checked{other};
        return compare(checked);
    }

    [[nodiscard]] bool operator==(const ustr &lhs, const ustr &rhs) noexcept {
        return lhs.bytes_ == rhs.bytes_;
    }

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

    std::size_t hash<SFT::Foundation::UString>::operator()(const SFT::Foundation::UString &value) const noexcept {
        return std::hash<std::string_view>{}(value.cpp_string_view());
    }

    std::size_t hash<SFT::Foundation::ustr>::operator()(const SFT::Foundation::ustr &value) const noexcept {
        return std::hash<std::string_view>{}(value.cpp_string_view());
    }

} // namespace std

fmt::format_context::iterator fmt::formatter<SFT::Foundation::UString>::format(const SFT::Foundation::UString &value, fmt::format_context &ctx) const {
    return fmt::formatter<std::string_view>::format(value.cpp_string_view(), ctx);
}

fmt::format_context::iterator fmt::formatter<SFT::Foundation::ustr>::format(const SFT::Foundation::ustr &value, fmt::format_context &ctx) const {
    return fmt::formatter<std::string_view>::format(value.cpp_string_view(), ctx);
}

fmt::format_context::iterator fmt::formatter<SFT::Foundation::USlice>::format(SFT::Foundation::USlice value, fmt::format_context &ctx) const {
    return fmt::formatter<std::string_view>::format(SFT::Foundation::Detail::display_string(value), ctx);
}

fmt::format_context::iterator fmt::formatter<SFT::Foundation::USlicePattern>::format(SFT::Foundation::USlicePattern value, fmt::format_context &ctx) const {
    return fmt::formatter<std::string_view>::format(SFT::Foundation::Detail::display_string(value), ctx);
}

fmt::format_context::iterator fmt::formatter<SFT::Foundation::UStringValidationError>::format(SFT::Foundation::UStringValidationError value, fmt::format_context &ctx) const {
    return fmt::formatter<std::string_view>::format(SFT::Foundation::to_string(value), ctx);
}

fmt::format_context::iterator fmt::formatter<SFT::Foundation::TextConversionError>::format(SFT::Foundation::TextConversionError value, fmt::format_context &ctx) const {
    return fmt::formatter<std::string_view>::format(SFT::Foundation::to_string(value), ctx);
}

fmt::format_context::iterator fmt::formatter<SFT::Foundation::UStringValidation>::format(const SFT::Foundation::UStringValidation &value, fmt::format_context &ctx) const {
    return fmt::formatter<std::string_view>::format(SFT::Foundation::Detail::display_string(value), ctx);
}

