module;

#include <expected>
#include <string>
#include <utility>

export module Sturdy.Core:ShaderError;

import Sturdy.Foundation;

using std::expected;
using std::string;
using std::unexpected;

export namespace SFT::Core::Slang {

    enum class ShaderErrorCode {
        InitializationFailed,
        InvalidArgument,
        FileReadFailed,
        CompilationFailed,
        ReflectionFailed,
        EntryPointNotFound,
        CodeGenerationFailed,
        OutOfMemory,
        OperationFailed,
    };

    struct ShaderError {
        ShaderErrorCode code = ShaderErrorCode::OperationFailed;
        string message;
        string diagnostics;
    };

    using ShaderResult = expected<void, ShaderError>;

    template <typename Value>
    using ShaderExpected = expected<Value, ShaderError>;

    [[nodiscard]] inline unexpected<ShaderError> shader_error(ShaderErrorCode code, string message, string diagnostics = {}) {
        return unexpected(ShaderError{code, std::move(message), std::move(diagnostics)});
    }

} // namespace SFT::Core::Slang
