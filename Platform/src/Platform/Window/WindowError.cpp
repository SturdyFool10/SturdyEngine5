#include "WindowError.hpp"

namespace SFT::Platform::Windowing {

WindowError::WindowError(WindowErrorCode error_code, const char *error_message) noexcept
            : code(error_code) {
            if (error_message) {
                try {
                    message = error_message;
                } catch (...) {
                }
            }
        }

WindowError::WindowError(WindowErrorCode error_code, string_view error_message) noexcept
            : code(error_code) {
            try {
                message = error_message;
            } catch (...) {
            }
        }

} // namespace SFT::Platform::Windowing
