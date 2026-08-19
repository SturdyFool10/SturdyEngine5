#pragma once

#include <string>
#include <string_view>

namespace SFT::Async {

    enum class IoErrorCode {
        NotFound,
        PermissionDenied,
        AlreadyExists,
        InvalidArgument,
        ConnectionRefused,
        ConnectionReset,
        TimedOut,
        Unsupported,
        Unknown,
    };

    struct IoError {
        IoErrorCode code;
        std::string message;

        /// Constructs a `IoError` from the supplied initialization values.
        ///
        /// @param error_code `error_code` value used by the operation.
        /// @param error_message `error_message` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        IoError(IoErrorCode error_code, const char *error_message) noexcept;

        /// Constructs a `IoError` from the supplied initialization values.
        ///
        /// @param error_code `error_code` value used by the operation.
        /// @param error_message `error_message` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        IoError(IoErrorCode error_code, std::string_view error_message) noexcept;
    };

} // namespace SFT::Async
