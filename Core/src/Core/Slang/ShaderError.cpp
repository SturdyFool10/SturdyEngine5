#include <Core/Slang/ShaderError.hpp>

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
        // Record where the failure was found: by the time the error value reaches whoever reports it, this stack is long gone.
        Foundation::note_trace_origin();
        return unexpected(ShaderError{code, std::move(message), std::move(diagnostics)});
    }

} // namespace SFT::Core::Slang
