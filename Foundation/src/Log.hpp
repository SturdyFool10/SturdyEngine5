#pragma once

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <Foundation/src/ConsoleDiagnostic.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

using std::string_view;

namespace SFT::Foundation {


    /// Logs the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param level `level` value used by the operation.
    /// @param format Format used for the resource, render target, or conversion.
    /// @param args `args` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    template <typename... Args>
    void log(spdlog::level::level_enum level, spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        try {
            ::spdlog::log(level, format, std::forward<Args>(args)...);
        } catch (...) {
        }
    }


    /// Logs the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param level `level` value used by the operation.
    /// @param message Text consumed by the operation.
    ///
    /// @note This function does not throw exceptions.
    void log(spdlog::level::level_enum level, string_view message) noexcept;


    /// Logs trace using the supplied arguments and current state.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    /// @param args `args` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    template <typename... Args>
    void log_trace(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::trace, format, std::forward<Args>(args)...);
    }

    /// Logs trace using the supplied arguments and current state.
    ///
    /// @param message Text consumed by the operation.
    ///
    /// @note This function does not throw exceptions.
    void log_trace(string_view message) noexcept;


    /// Logs debug using the supplied arguments and current state.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    /// @param args `args` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    template <typename... Args>
    void log_debug(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::debug, format, std::forward<Args>(args)...);
    }

    /// Logs debug using the supplied arguments and current state.
    ///
    /// @param message Text consumed by the operation.
    ///
    /// @note This function does not throw exceptions.
    void log_debug(string_view message) noexcept;


    /// Logs info using the supplied arguments and current state.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    /// @param args `args` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    template <typename... Args>
    void log_info(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::info, format, std::forward<Args>(args)...);
    }

    /// Logs info using the supplied arguments and current state.
    ///
    /// @param message Text consumed by the operation.
    ///
    /// @note This function does not throw exceptions.
    void log_info(string_view message) noexcept;


    /// Logs warn using the supplied arguments and current state.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    /// @param args `args` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    template <typename... Args>
    void log_warn(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::warn, format, std::forward<Args>(args)...);
    }

    /// Logs warn using the supplied arguments and current state.
    ///
    /// @param message Text consumed by the operation.
    ///
    /// @note This function does not throw exceptions.
    void log_warn(string_view message) noexcept;


    /// Logs error using the supplied arguments and current state.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    /// @param args `args` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    template <typename... Args>
    void log_error(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::err, format, std::forward<Args>(args)...);
    }

    /// Logs error using the supplied arguments and current state.
    ///
    /// @param message Text consumed by the operation.
    ///
    /// @note This function does not throw exceptions.
    void log_error(string_view message) noexcept;


    /// Logs diagnostic using the supplied arguments and current state.
    ///
    /// @param diagnostic `diagnostic` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void log_diagnostic(const ConsoleDiagnostic &diagnostic) noexcept;


    /// Flushes logs.
    ///
    /// @note This function does not throw exceptions.
    void flush_logs() noexcept;


    /// Initializes file logging for use.
    ///
    /// @param log_file_path Filesystem path identifying the target resource.
    /// @param max_file_size_bytes Size of the relevant data in bytes.
    /// @param max_rotated_files `max_rotated_files` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool init_file_logging(
        const std::filesystem::path &log_file_path,
        std::size_t max_file_size_bytes = 5 * 1024 * 1024,
        std::size_t max_rotated_files = 3) noexcept;

} // namespace SFT::Foundation
