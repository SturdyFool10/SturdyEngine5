#pragma once

// Plain-data log types. bindgen-safe (see plans/cxx-api.md).

#include <cstddef>
#include <cstdint>

namespace SFT::CxxApi {

    /// Severity of a log message. Mirrors `Foundation::LogLevel`; values never change.
    enum class LogLevel : std::uint32_t {
        Trace = 0,
        Debug = 1,
        Info = 2,
        Warn = 3,
        Error = 4,
        Critical = 5,
    };

} // namespace SFT::CxxApi
