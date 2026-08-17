#include "ShaderError.hpp"

namespace SFT::Core::Slang {

/// Creates an error result describing the supplied shader failure.
///
/// @param code `code` value used by the operation.
/// @param message Text consumed by the operation.
/// @param diagnostics `diagnostics` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
unexpected<ShaderError> shader_error(ShaderErrorCode code, string message, string diagnostics) {
        return unexpected(ShaderError{code, std::move(message), std::move(diagnostics)});
    }

} // namespace SFT::Core::Slang
