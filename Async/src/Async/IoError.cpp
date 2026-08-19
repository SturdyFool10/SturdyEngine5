#include <Async/IoError.hpp>


namespace SFT::Async {

    /// Performs the I/O error operation for `Async` using the supplied arguments.
    ///
    /// @param error_code `error_code` value used by the operation.
    /// @param error_message `error_message` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    IoError::IoError(IoErrorCode error_code, const char *error_message) noexcept
        : code(error_code) {
        if (error_message) {
            try {
                message = error_message;
            } catch (...) {
            }
        }
    }

    /// Performs the I/O error operation for `Async` using the supplied arguments.
    ///
    /// @param error_code `error_code` value used by the operation.
    /// @param error_message `error_message` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    IoError::IoError(IoErrorCode error_code, std::string_view error_message) noexcept
        : code(error_code) {
        try {
            message = error_message;
        } catch (...) {
        }
    }

} // namespace SFT::Async

