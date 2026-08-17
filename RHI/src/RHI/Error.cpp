#include "Error.hpp"

namespace SFT::RHI {

/// Creates an error result describing the supplied RHI failure.
///
/// @param code `code` value used by the operation.
/// @param message Text consumed by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
unexpected<RhiError> rhi_error(RhiErrorCode code, string message) {
        return unexpected(RhiError{code, std::move(message)});
    }

} // namespace SFT::RHI
