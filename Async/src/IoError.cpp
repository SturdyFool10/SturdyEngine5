#include <Async/src/IoError.hpp>


namespace SFT::Async {

    IoError::IoError(IoErrorCode error_code, const char *error_message) noexcept
        : code(error_code) {
        if (error_message) {
            try {
                message = error_message;
            } catch (...) {
            }
        }
    }

    IoError::IoError(IoErrorCode error_code, std::string_view error_message) noexcept
        : code(error_code) {
        try {
            message = error_message;
        } catch (...) {
        }
    }

} // namespace SFT::Async

