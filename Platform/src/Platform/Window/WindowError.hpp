#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <string>
#include <string_view>
#pragma endregion

using std::string;
using std::string_view;

namespace SFT::Platform::Windowing {

    enum class WindowErrorCode {
        Unsupported,
        InvalidArgument,
        BackendUnavailable,
        CreationFailed,
        OperationFailed,
        OutOfMemory,
    };

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

        WindowError(WindowErrorCode error_code, const char *error_message) noexcept;

        WindowError(WindowErrorCode error_code, string_view error_message) noexcept;
    };

} // namespace SFT::Platform::Windowing
