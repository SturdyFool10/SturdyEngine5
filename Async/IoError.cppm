module;

#pragma region Imports
#include <string>
#include <string_view>
#pragma endregion

export module Sturdy.Async:IoError;

using std::string;
using std::string_view;

export namespace SFT::Async {

    // Shared error shape for :File and :Net — deliberately small and platform-agnostic (no errno/
    // WinSock codes leak out), matching the WindowError/GraphicsBackendError shape used elsewhere.
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
        string message;

        IoError(IoErrorCode error_code, const char *error_message) noexcept
            : code(error_code) {
            if (error_message) {
                try {
                    message = error_message;
                } catch (...) {
                }
            }
        }

        IoError(IoErrorCode error_code, string_view error_message) noexcept
            : code(error_code) {
            try {
                message = error_message;
            } catch (...) {
            }
        }
    };

} // namespace SFT::Async
