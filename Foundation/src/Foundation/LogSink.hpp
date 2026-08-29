#pragma once

/// Lets a caller — same-language C++ or a foreign FFI consumer — receive every log message the
/// engine produces, alongside whatever console/file sinks are already configured. This is what a
/// host application uses to build its own log viewer/console rather than only ever seeing what
/// this process's stdout shows.

#include <Foundation/Types.hpp>

#include <functional>
#include <string_view>

namespace SFT::Foundation {

    /// Severity of a log message, ordinal-matched to `spdlog::level::level_enum` and to the FFI's
    /// `SturdyLogLevel` so both sides can `static_cast` between them without a translation table.
    enum class LogLevel : u8 {
        Trace = 0,
        Debug = 1,
        Info = 2,
        Warn = 3,
        Error = 4,
        Critical = 5,
    };

    /// Handle to a registered log sink, returned by `add_log_sink`.
    struct LogSinkId {
        u64 value = 0;

        /// Reports whether this handle refers to a real, still-registered sink.
        ///
        /// @return Returns `true` when nonzero; otherwise `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
    };

    /// A caller-supplied function invoked once per log message. `message` is the formatted text
    /// alone (no timestamp/level prefix — the callback receives `level` separately to format
    /// however it likes); it is only valid for the duration of the call.
    using LogSinkCallback = std::function<void(LogLevel level, std::string_view message)>;

    /// Registers `callback` to run for every subsequent log message, in addition to the engine's
    /// own sinks. Safe to call from any thread, at any point — including before any other engine
    /// subsystem has started, since logging itself has no other dependencies.
    ///
    /// @param callback Function to invoke per message. A null/empty `std::function` is rejected
    ///        by returning an invalid `LogSinkId`.
    ///
    /// @return A handle to unregister the sink later with `remove_log_sink`, or an invalid
    ///         (`is_valid() == false`) handle if `callback` was empty.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] LogSinkId add_log_sink(LogSinkCallback callback);

    /// Unregisters a sink added by `add_log_sink`. An invalid handle, or one already removed, is a
    /// no-op.
    ///
    /// @param id Handle returned by `add_log_sink`.
    ///
    /// @note This function does not throw exceptions.
    void remove_log_sink(LogSinkId id) noexcept;

} // namespace SFT::Foundation
