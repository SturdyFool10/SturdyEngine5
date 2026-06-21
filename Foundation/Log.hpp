#pragma once

// Thin wrapper over spdlog (already a Foundation dependency) so subsystems can report
// diagnostics without each one re-deciding a logging convention.

#include <spdlog/spdlog.h>

#include <string_view>
#include <utility>

namespace SFT::Foundation {

template <typename... Args>
void log(spdlog::level::level_enum level, spdlog::format_string_t<Args...> format, Args&&... args) noexcept
{
    try {
        ::spdlog::log(level, format, std::forward<Args>(args)...);
    } catch (...) {
    }
}

inline void log(spdlog::level::level_enum level, std::string_view message) noexcept
{
    try {
        ::spdlog::log(level, "{}", message);
    } catch (...) {
    }
}

template <typename... Args>
void log_trace(spdlog::format_string_t<Args...> format, Args&&... args) noexcept
{
    log(spdlog::level::trace, format, std::forward<Args>(args)...);
}

inline void log_trace(std::string_view message) noexcept { log(spdlog::level::trace, message); }

template <typename... Args>
void log_debug(spdlog::format_string_t<Args...> format, Args&&... args) noexcept
{
    log(spdlog::level::debug, format, std::forward<Args>(args)...);
}

inline void log_debug(std::string_view message) noexcept { log(spdlog::level::debug, message); }

template <typename... Args>
void log_info(spdlog::format_string_t<Args...> format, Args&&... args) noexcept
{
    log(spdlog::level::info, format, std::forward<Args>(args)...);
}

inline void log_info(std::string_view message) noexcept { log(spdlog::level::info, message); }

template <typename... Args>
void log_warn(spdlog::format_string_t<Args...> format, Args&&... args) noexcept
{
    log(spdlog::level::warn, format, std::forward<Args>(args)...);
}

inline void log_warn(std::string_view message) noexcept { log(spdlog::level::warn, message); }

template <typename... Args>
void log_error(spdlog::format_string_t<Args...> format, Args&&... args) noexcept
{
    log(spdlog::level::err, format, std::forward<Args>(args)...);
}

inline void log_error(std::string_view message) noexcept { log(spdlog::level::err, message); }

} // namespace SFT::Foundation
