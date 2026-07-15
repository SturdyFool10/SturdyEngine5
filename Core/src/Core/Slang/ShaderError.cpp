#include "ShaderError.hpp"

namespace SFT::Core::Slang {

unexpected<ShaderError> shader_error(ShaderErrorCode code, string message, string diagnostics) {
        return unexpected(ShaderError{code, std::move(message), std::move(diagnostics)});
    }

} // namespace SFT::Core::Slang
