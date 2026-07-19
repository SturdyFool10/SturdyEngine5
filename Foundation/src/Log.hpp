#pragma once

#include <spdlog/spdlog.h>

#include <string_view>
#include <utility>

using std::string_view;

namespace SFT::Foundation {

    // The engine's logging front-end — a thin, **exception-proof** wrapper over spdlog. Every function
    // swallows any exception the logger might throw (bad format, I/O), so logging can never bring the
    // program down or escape a `noexcept` caller. Format strings use `std::format` / `{fmt}` syntax and
    // are checked at compile time.
    //
    // Prefer the leveled helpers (`log_info`, `log_error`, ...) over calling `log()` with an explicit
    // level.
    //
    // ```cpp
    // log_info("selected GPU: {} ({} MB)", name, vram_mb);
    // log_error("shader '{}' failed: {}", path, err.message);
    // ```

    // Log `args` formatted by `format` at an explicit `level`. Compile-time-checked format string.
    template <typename... Args>
    void log(spdlog::level::level_enum level, spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        try {
            ::spdlog::log(level, format, std::forward<Args>(args)...);
        } catch (...) {
        }
    }

    // Log a plain, already-formatted `message` at an explicit `level` (no format parsing).
    inline void log(spdlog::level::level_enum level, string_view message) noexcept {
        try {
            ::spdlog::log(level, "{}", message);
        } catch (...) {
        }
    }

    // `trace`: extremely verbose, per-operation tracing — usually compiled out in release.
    template <typename... Args>
    void log_trace(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::trace, format, std::forward<Args>(args)...);
    }

    inline void log_trace(string_view message) noexcept { log(spdlog::level::trace, message); }

    // `debug`: developer diagnostics useful while debugging, not wanted in normal runs.
    template <typename... Args>
    void log_debug(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::debug, format, std::forward<Args>(args)...);
    }

    inline void log_debug(string_view message) noexcept { log(spdlog::level::debug, message); }

    // `info`: normal operational milestones (startup, device selection, ...).
    template <typename... Args>
    void log_info(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::info, format, std::forward<Args>(args)...);
    }

    inline void log_info(string_view message) noexcept { log(spdlog::level::info, message); }

    // `warn`: recoverable problems or degraded paths worth flagging.
    template <typename... Args>
    void log_warn(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::warn, format, std::forward<Args>(args)...);
    }

    inline void log_warn(string_view message) noexcept { log(spdlog::level::warn, message); }

    // `error`: failures the caller could not handle transparently.
    template <typename... Args>
    void log_error(spdlog::format_string_t<Args...> format, Args &&...args) noexcept {
        log(spdlog::level::err, format, std::forward<Args>(args)...);
    }

    inline void log_error(string_view message) noexcept { log(spdlog::level::err, message); }

    // Force pending messages to their sinks. Fatal contract paths call this immediately before
    // termination so their final diagnostic reaches the console even if a future logger becomes
    // buffered or asynchronous.
    inline void flush_logs() noexcept {
        try {
            if (const auto logger = ::spdlog::default_logger()) {
                logger->flush();
            }
        } catch (...) {
        }
    }

} // namespace SFT::Foundation
