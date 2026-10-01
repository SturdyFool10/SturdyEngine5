#pragma once

#include <stdexcept>
#include <string>

namespace SFT::CxxApi {

    /// Thrown by every fallible CxxApi function. `cxx` turns it into the `Err` of a function bound
    /// as `-> Result<T>`, carrying `what()` as the message.
    class Error : public std::runtime_error {
      public:
        using std::runtime_error::runtime_error;
    };

    /// Throws `Error` with `message`.
    ///
    /// @param message UTF-8 description of the failure.
    [[noreturn]] inline void fail(const std::string &message) { throw Error{message}; }

} // namespace SFT::CxxApi
