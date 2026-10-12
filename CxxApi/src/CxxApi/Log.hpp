#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <CxxApi/Types/Log.hpp>

namespace SFT::CxxApi {

    /// Receives every message the engine logs, so a game can host its own console or log viewer. The binding implements this
    /// once, forwarding to its own closure.
    class LogSinkCallback {
      public:
        virtual ~LogSinkCallback() = default;

        /// `message` is `length` bytes of UTF-8, not null-terminated, valid only for this call.
        ///
        /// Called from whichever thread produced the message, concurrently with other invocations: it must be thread-safe, must
        /// not block, and must not call back into the engine. Must not unwind.
        virtual void on_message(LogLevel level, const char *message, std::size_t length) noexcept = 0;
    };

    /// Registers a sink alongside the engine's own console and file sinks and returns its handle. Safe to call before the runtime
    /// starts, from any thread. The engine keeps `callback` alive until the sink is removed and no call is still running.
    [[nodiscard]] std::uint64_t log_add_sink(std::unique_ptr<LogSinkCallback> callback);
    /// Unregisters a sink. An unknown or already removed handle is not an error.
    void log_remove_sink(std::uint64_t sink) noexcept;

} // namespace SFT::CxxApi
