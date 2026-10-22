#pragma once

#include <Foundation/Trace.hpp>

#include <stdexcept>
#include <string>

namespace SFT::CxxApi {

    /// Thrown by every fallible CxxApi function. `cxx` turns it into the `Err` of a function bound
    /// as `-> Result<T>`, carrying `what()` as the message.
    ///
    /// Constructing one records an engine stack trace for the calling thread (the trace recorded where the underlying
    /// failure was found if there is one, otherwise the stack at this throw, plus the engine scopes an in-flight
    /// exception has already unwound through). The foreign caller reads it with `last_error_trace` straight after the
    /// `Err` arrives, on the same thread. Capturing never throws and costs nothing when tracing is off.
    class Error : public std::runtime_error {
      public:
        explicit Error(const std::string &message) : std::runtime_error(message) { record_trace(); }
        explicit Error(const char *message) : std::runtime_error(message) { record_trace(); }

      private:
        static void record_trace() noexcept {
            Foundation::TraceCapture trace;
            if (auto origin = Foundation::take_trace_origin()) {
                trace = std::move(*origin);
            } else {
                trace = Foundation::TraceCapture::capture(2);
            }
            trace.adopt_unwound_scopes();
            Foundation::set_last_error_trace(std::move(trace));
        }
    };

    /// Throws `Error` with `message`.
    ///
    /// @param message UTF-8 description of the failure.
    [[noreturn]] inline void fail(const std::string &message) { throw Error{message}; }

} // namespace SFT::CxxApi
