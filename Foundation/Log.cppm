module;

#include <spdlog/spdlog.h>

#include <string_view>
#include <utility>

export module Sturdy.Foundation:Log;

using std::string_view;

export namespace SFT::Foundation {

    template <typename... Args>
    void log(spdlog::level::level_enum level, spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        try {
            ::spdlog::log(level, format, std::forward<Args>(args)...);
        } catch (...) {
        }
    }

    inline void log(spdlog::level::level_enum level, string_view message) noexcept {
        try {
            ::spdlog::log(level, "{}", message);
        } catch (...) {
        }
    }

    template <typename... Args>
    void log_trace(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::trace, format, std::forward<Args>(args)...);
    }

    inline void log_trace(string_view message) noexcept { log(spdlog::level::trace, message); }

    template <typename... Args>
    void log_debug(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::debug, format, std::forward<Args>(args)...);
    }

    inline void log_debug(string_view message) noexcept { log(spdlog::level::debug, message); }

    template <typename... Args>
    void log_info(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::info, format, std::forward<Args>(args)...);
    }

    inline void log_info(string_view message) noexcept { log(spdlog::level::info, message); }

    template <typename... Args>
    void log_warn(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::warn, format, std::forward<Args>(args)...);
    }

    inline void log_warn(string_view message) noexcept { log(spdlog::level::warn, message); }

    template <typename... Args>
    void log_error(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::err, format, std::forward<Args>(args)...);
    }

    inline void log_error(string_view message) noexcept { log(spdlog::level::err, message); }

} // namespace SFT::Foundation
