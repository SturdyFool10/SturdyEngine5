#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace SFT::Core::Slang {

    /// Shader paths cross the API (and the Rust FFI) as UTF-8 `std::string`s. `std::filesystem::path{string}` reads a narrow string in the
    /// Windows ANSI code page, which silently mangles - or fails to open - a path with a character outside that page (an install folder under a
    /// user name such as "Zoë" on a machine set to a different code page). These two go through UTF-8 explicitly, and are identity-like
    /// wrappers everywhere else.
    [[nodiscard]] inline std::filesystem::path shader_path_from_utf8(std::string_view text) {
#if defined(_WIN32)
        return std::filesystem::path{std::u8string{reinterpret_cast<const char8_t *>(text.data()), text.size()}};
#else
        return std::filesystem::path{std::string{text}};
#endif
    }

    [[nodiscard]] inline std::string shader_path_to_utf8(const std::filesystem::path &path) {
#if defined(_WIN32)
        const std::u8string text = path.u8string();
        return std::string{reinterpret_cast<const char *>(text.data()), text.size()};
#else
        return path.string();
#endif
    }

} // namespace SFT::Core::Slang
