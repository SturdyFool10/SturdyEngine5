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

        IoError(IoErrorCode error_code, const char *error_message) noexcept;

        IoError(IoErrorCode error_code, std::string_view error_message) noexcept;
    };

} // namespace SFT::Async
