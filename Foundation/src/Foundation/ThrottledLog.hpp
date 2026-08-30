#pragma once

#include <Foundation/Log.hpp>
#include <Foundation/Types.hpp>

#pragma region Imports
#include <chrono>
#include <source_location>
#include <string>
#pragma endregion

namespace SFT::Foundation {

    /// Decides whether the call site identified by `location` should actually emit right now, given
    /// `min_interval` — the engine-wide rate limit for *this exact call site* (file+line), not for the
    /// message content, so a hot loop hammering the same `log_error()`-shaped call collapses into one
    /// line every `min_interval` instead of one line per frame. Every call still counts, so the emitted
    /// line can report how many were suppressed since the last one.
    ///
    /// @param location Call site identity — pass `std::source_location::current()` from the wrapping
    ///        macro/function, not from further up the call stack, or every caller collapses into one
    ///        shared budget.
    /// @param min_interval Minimum real time between two emissions from the same call site.
    ///
    /// @return `0` the first time a call site is seen (always emits), or on any call at/after
    ///         `min_interval` has elapsed since the last emission (also emits) — the return value is
    ///         the number of calls suppressed since the last emission. A negative value (`-1`) means
    ///         "don't emit yet", i.e. still within the throttle window.
    /// @note This function does not throw exceptions.
    [[nodiscard]] i64 throttled_log_gate(const std::source_location &location,
                                        std::chrono::milliseconds min_interval) noexcept;

    /// Clears all throttle-gate state, forgetting every call site's last-emitted time and suppressed
    /// count. Tests and any long-lived tool that wants a clean slate between runs are the only expected
    /// callers — normal logging code never needs this.
    ///
    /// @note This function does not throw exceptions.
    void reset_throttled_log_gates() noexcept;

    /// Logs `message` (already fully formatted by the caller) through the normal `Foundation::log()`
    /// path, prefixed with the call site's file:line/function and, if any calls were suppressed since
    /// the last emission from that exact call site, a "(+N suppressed since last)" suffix — this
    /// is the formatting half of the throttled-logging contract; `throttled_log_gate` is the rate-limit
    /// half. Prefer the `SFT_LOG_*_THROTTLED` macros (below) over calling this directly, since they
    /// capture `std::source_location::current()` and the gate check for you.
    ///
    /// @param level Severity to log at.
    /// @param location Call site identity, forwarded into the formatted prefix.
    /// @param suppressed Value `throttled_log_gate` returned for this call — pass it straight through.
    /// @param message Fully-formatted message text (already `fmt::format`-ed by the caller).
    ///
    /// @note This function does not throw exceptions.
    void emit_throttled_log(spdlog::level::level_enum level, const std::source_location &location,
                            i64 suppressed, string_view message) noexcept;

    /// Formats `args` per `format` and routes the result through `emit_throttled_log`, but only if
    /// `throttled_log_gate` says this call site's throttle window has elapsed. This is the one function
    /// every `SFT_LOG_*_THROTTLED` macro expands to — call it directly instead of the macros only if a
    /// non-default `std::source_location` is genuinely needed (e.g. forwarding a caller's location
    /// through a logging helper of your own).
    ///
    /// @param level Severity to log at.
    /// @param min_interval Minimum real time between two emissions from `location`.
    /// @param location Call site identity.
    /// @param format Format string, checked at compile time like `Foundation::log_error`'s own.
    /// @param args Format arguments.
    ///
    /// @note This function does not throw exceptions.
    template <typename... Args>
    void log_throttled(spdlog::level::level_enum level, std::chrono::milliseconds min_interval,
                       const std::source_location &location, spdlog::format_string_t<Args...> format,
                       Args &&...args) noexcept {
        const i64 suppressed = throttled_log_gate(location, min_interval);
        if (suppressed < 0) {
            return;
        }
        try {
            emit_throttled_log(level, location, suppressed, ::fmt::format(format, std::forward<Args>(args)...));
        } catch (...) {
        }
    }

} // namespace SFT::Foundation

/// Logs at `level` no more than once per `interval_ms` milliseconds *per call site* — use these instead
/// of `Foundation::log_error`/`log_warn`/etc. for anything reachable from a per-frame or per-element
/// code path (layout validation, resource-lookup misses, shader compile retries...), where a real,
/// persistent problem would otherwise print one line per frame forever and bury everything else in the
/// log. The emitted line always carries the call site's file:line/function and, once suppression has
/// happened, how many calls were folded into it — both `Foundation::log_error(...)` itself doesn't give
/// you, since a plain format string has no way to say where it was called from.
#define SFT_LOG_ERROR_THROTTLED(interval_ms, ...)                                                                    \
    ::SFT::Foundation::log_throttled(::spdlog::level::err, std::chrono::milliseconds(interval_ms),                  \
                                     std::source_location::current(), __VA_ARGS__)
#define SFT_LOG_WARN_THROTTLED(interval_ms, ...)                                                                     \
    ::SFT::Foundation::log_throttled(::spdlog::level::warn, std::chrono::milliseconds(interval_ms),                  \
                                     std::source_location::current(), __VA_ARGS__)
#define SFT_LOG_INFO_THROTTLED(interval_ms, ...)                                                                     \
    ::SFT::Foundation::log_throttled(::spdlog::level::info, std::chrono::milliseconds(interval_ms),                  \
                                     std::source_location::current(), __VA_ARGS__)
