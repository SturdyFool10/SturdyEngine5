#include <WindowManager/WindowError.hpp>

namespace SFT::WindowManager {

/// Performs the window error operation for `Windowing` using the supplied arguments.
///
/// @param error_code `error_code` value used by the operation.
/// @param error_message `error_message` value used by the operation.
///
/// @note This function does not throw exceptions.
WindowError::WindowError(WindowErrorCode error_code, const char *error_message) noexcept
            : code(error_code) {
            if (error_message) {
                try {
                    message = error_message;
                } catch (...) {
                }
            }
        }

/// Performs the window error operation for `Windowing` using the supplied arguments.
///
/// @param error_code `error_code` value used by the operation.
/// @param error_message `error_message` value used by the operation.
///
/// @note This function does not throw exceptions.
WindowError::WindowError(WindowErrorCode error_code, string_view error_message) noexcept
            : code(error_code) {
            try {
                message = error_message;
            } catch (...) {
            }
        }

} // namespace SFT::WindowManager
