#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <string>
#include <string_view>
#pragma endregion

using std::string;
using std::string_view;

namespace SFT::WindowManager {

    enum class WindowErrorCode {
        Unsupported,
        InvalidArgument,
        BackendUnavailable,
        CreationFailed,
        OperationFailed,
        OutOfMemory,
    };

    /// Returns a human-readable name for the supplied window error code value.
    ///
    /// @param code `code` value used by the operation.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr string_view window_error_code_name(WindowErrorCode code) noexcept {
        switch (code) {
            case WindowErrorCode::Unsupported: return "platform.window.unsupported";
            case WindowErrorCode::InvalidArgument: return "platform.window.invalid_argument";
            case WindowErrorCode::BackendUnavailable: return "platform.window.backend_unavailable";
            case WindowErrorCode::CreationFailed: return "platform.window.creation_failed";
            case WindowErrorCode::OperationFailed: return "platform.window.operation_failed";
            case WindowErrorCode::OutOfMemory: return "platform.window.out_of_memory";
        }
        return "platform.window.unknown";
    }

    struct WindowError {
        WindowErrorCode code;
        string message;

        /// Constructs a `WindowError` from the supplied initialization values.
        ///
        /// @param error_code `error_code` value used by the operation.
        /// @param error_message `error_message` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        WindowError(WindowErrorCode error_code, const char *error_message) noexcept;

        /// Constructs a `WindowError` from the supplied initialization values.
        ///
        /// @param error_code `error_code` value used by the operation.
        /// @param error_message `error_message` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        WindowError(WindowErrorCode error_code, string_view error_message) noexcept;
    };

} // namespace SFT::WindowManager
