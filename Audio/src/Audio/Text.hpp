#pragma once

#include <Foundation/Foundation.hpp>

#include <ranges>

#include <charconv>
#include <format>
#include <optional>
#include <string_view>
#include <vector>

/// Text helpers for the places audio code meets bytes it does not control: tags in files and network descriptions, device
/// names from the OS, extensions typed by users.
namespace SFT::Audio {

    /// `bytes` as text. Valid UTF-8 is kept; anything else (an ID3v1 or WAV INFO tag in Latin-1, a device name in the local
    /// code page) is read byte-per-character as Latin-1 rather than rejected, so odd metadata never fails a load.
    [[nodiscard]] inline UString text_from_bytes(std::string_view bytes) {
        if (auto valid = UString::try_from_utf8(bytes)) {
            return std::move(*valid);
        }
        UString out;
        for (char c : bytes) {
            out.push_back(static_cast<char32_t>(static_cast<unsigned char>(c)));
        }
        return out;
    }

    /// A file-system failure reported by `Foundation::Io` (plain `std::string`) as an audio error message.
    [[nodiscard]] inline UString io_error(std::string_view message) {
        return UString{std::format("audio: {}", text_from_bytes(message))};
    }

    /// ASCII letters lower-cased; every other character is kept.
    [[nodiscard]] inline UString ascii_lower(const UString &text) {
        UString out;
        for (char32_t c : text) {
            out.push_back(c >= U'A' && c <= U'Z' ? static_cast<char32_t>(c + (U'a' - U'A')) : c);
        }
        return out;
    }

    /// ASCII letters upper-cased; every other character is kept.
    [[nodiscard]] inline UString ascii_upper(const UString &text) {
        UString out;
        for (char32_t c : text) {
            out.push_back(c >= U'a' && c <= U'z' ? static_cast<char32_t>(c - (U'a' - U'A')) : c);
        }
        return out;
    }

    /// ASCII letters lower-cased in a view; every other character is kept.
    [[nodiscard]] inline UString ascii_lower(const ustr &text) {
        UString out;
        for (char32_t c : text.cpp_string_view() | std::views::transform([](char ch) { return static_cast<char32_t>(static_cast<unsigned char>(ch)); })) {
            out.push_back(c >= U'A' && c <= U'Z' ? static_cast<char32_t>(c + (U'a' - U'A')) : c);
        }
        return out;
    }

    /// `bytes` split at every `separator` (an ASCII character); an empty input gives one empty piece, like the protocols that
    /// use it expect. Pieces are owned because `ustr` views cannot be stored.
    [[nodiscard]] inline std::vector<UString> split(std::string_view bytes, char separator) {
        std::vector<UString> parts;
        usize begin = 0;
        for (;;) {
            const usize end = bytes.find(separator, begin);
            parts.push_back(text_from_bytes(bytes.substr(begin, end == std::string_view::npos ? end : end - begin)));
            if (end == std::string_view::npos) {
                return parts;
            }
            begin = end + 1;
        }
    }

    /// `bytes` without trailing spaces, tabs, CR and LF.
    [[nodiscard]] inline UString trim_end(std::string_view bytes) {
        while (!bytes.empty() && (bytes.back() == ' ' || bytes.back() == '\t' || bytes.back() == '\r' || bytes.back() == '\n')) {
            bytes.remove_suffix(1);
        }
        return text_from_bytes(bytes);
    }

    /// The number a text starts with (leading blanks allowed, trailing characters ignored), or nothing when there is none.
    template <class T>
    [[nodiscard]] std::optional<T> parse_number(std::string_view bytes) {
        while (!bytes.empty() && (bytes.front() == ' ' || bytes.front() == '\t')) {
            bytes.remove_prefix(1);
        }
        T value{};
        const auto [end, error] = std::from_chars(bytes.data(), bytes.data() + bytes.size(), value);
        if (error != std::errc{}) {
            return std::nullopt;
        }
        (void)end;
        return value;
    }

    /// `parse_number` with a fallback for fields that are optional or malformed.
    template <class T>
    [[nodiscard]] T number_or(std::string_view text, T fallback) {
        return parse_number<T>(text).value_or(fallback);
    }


} // namespace SFT::Audio
