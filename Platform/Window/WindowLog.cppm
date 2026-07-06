module;

#pragma region Imports
#include <spdlog/spdlog.h>

#include <utility>
#pragma endregion

export module Sturdy.Platform:WindowLog;

import Sturdy.Foundation;

export namespace SFT::Platform::Windowing::Detail {

    template <typename... Args>
    void window_log(spdlog::level::level_enum level, spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        Foundation::log(level, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void window_trace(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        window_log(spdlog::level::trace, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void window_debug(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        window_log(spdlog::level::debug, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void window_info(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        window_log(spdlog::level::info, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void window_warn(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        window_log(spdlog::level::warn, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void window_error(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        window_log(spdlog::level::err, format, std::forward<Args>(args)...);
    }

} // namespace SFT::Platform::Windowing::Detail
