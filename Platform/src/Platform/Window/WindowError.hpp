#pragma once

#include <Foundation/Foundation.hpp>

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

    struct WindowError {
        WindowErrorCode code;
        string message;

        WindowError(WindowErrorCode error_code, const char *error_message) noexcept
            : code(error_code) {
            if (error_message) {
                try {
                    message = error_message;
                } catch (...) {
                }
            }
        }

        WindowError(WindowErrorCode error_code, string_view error_message) noexcept
            : code(error_code) {
            try {
                message = error_message;
            } catch (...) {
            }
        }
    };

} // namespace SFT::Platform::Windowing
