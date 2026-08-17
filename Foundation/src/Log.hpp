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














    /// Log `args` formatted by `format` at an explicit `level`. Compile-time-checked format string.
    template <typename... Args>
    void log(spdlog::level::level_enum level, spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        try {
            ::spdlog::log(level, format, std::forward<Args>(args)...);
        } catch (...) {
        }
    }

    /// Log a plain, already-formatted `message` at an explicit `level` (no format parsing).
    void log(spdlog::level::level_enum level, string_view message) noexcept;

    /// `trace`: extremely verbose, per-operation tracing — usually compiled out in release.
    template <typename... Args>
    void log_trace(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::trace, format, std::forward<Args>(args)...);
    }

    void log_trace(string_view message) noexcept;

    /// `debug`: developer diagnostics useful while debugging, not wanted in normal runs.
    template <typename... Args>
    void log_debug(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::debug, format, std::forward<Args>(args)...);
    }

    void log_debug(string_view message) noexcept;

    /// `info`: normal operational milestones (startup, device selection, ...).
    template <typename... Args>
    void log_info(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::info, format, std::forward<Args>(args)...);
    }

    void log_info(string_view message) noexcept;

    /// `warn`: recoverable problems or degraded paths worth flagging.
    template <typename... Args>
    void log_warn(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::warn, format, std::forward<Args>(args)...);
    }

    void log_warn(string_view message) noexcept;

    /// `error`: failures the caller could not handle transparently.
    template <typename... Args>
    void log_error(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::err, format, std::forward<Args>(args)...);
    }

    void log_error(string_view message) noexcept;

    /// Render one compiler-style diagnostic as a single log record, so the logger contributes only
    /// one timestamp/level prefix and the diagnostic's indented continuation lines remain readable.
    void log_diagnostic(const ConsoleDiagnostic &diagnostic) noexcept;

    /// Force pending messages to their sinks. Fatal contract paths call this immediately before
    /// termination so their final diagnostic reaches the console even if a future logger becomes
    /// buffered or asynchronous.
    void flush_logs() noexcept;

    /// Adds a rotating file sink to spdlog's default logger, on top of whatever sinks it already has
    /// (normally just the default console sink), so log history survives a closed console or a crash.
    /// Also flushes on every `warn`-or-worse record, since a crash can otherwise take buffered lines
    /// down with it. Existing sinks are left untouched; returns false if the file could not be opened.
    bool init_file_logging(
        const std::filesystem::path &log_file_path,
        std::size_t max_file_size_bytes = 5 * 1024 * 1024,
        std::size_t max_rotated_files = 3) noexcept;

} // namespace SFT::Foundation
