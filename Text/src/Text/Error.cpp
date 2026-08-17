#include "Error.hpp"

namespace SFT::Text {

/// Creates an error result describing the supplied text failure.
///
/// @param code `code` value used by the operation.
/// @param message Text consumed by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
unexpected<TextError> text_error(TextErrorCode code, UString message) {
        return unexpected(TextError{code, std::move(message)});
    }

} // namespace SFT::Text
