module;

#pragma region Imports
#include <expected>
#include <string>
#include <utility>
#pragma endregion

export module Sturdy.Core:ShaderError;

import Sturdy.Foundation;

using std::expected;
using std::string;
using std::unexpected;

export namespace SFT::Core::Slang {

    // What went wrong in a shader operation. Roughly ordered along the pipeline: session/compiler
    // bring-up (`InitializationFailed`), bad inputs (`InvalidArgument`), I/O (`FileReadFailed`), the
    // Slang front-end (`CompilationFailed`, `ReflectionFailed`, `EntryPointNotFound`), and target
    // codegen (`CodeGenerationFailed`). `OutOfMemory` / `OperationFailed` are the catch-alls.
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

    // A shader failure: a machine-readable `code`, a short human `message`, and — for front-end
    // failures — the compiler's full `diagnostics` text (the multi-line Slang error dump), which is
    // where the actual line/column and cause live. Always surface `diagnostics` when a shader won't
    // compile; `message` alone rarely says why.
    struct ShaderError {
        ShaderErrorCode code = ShaderErrorCode::OperationFailed;
        string message;
        string diagnostics;
    };

    // Result of a shader operation that yields nothing on success — `expected<void, ShaderError>`.
    using ShaderResult = expected<void, ShaderError>;

    // Result of a shader operation that yields a `Value` on success, or a `ShaderError` on failure.
    // The whole Slang surface (`compile`, `reflect`, `entry_point_code`, ...) returns one of these, so
    // callers branch on `has_value()` / `operator bool` and read `.error()` on the failure path.
    template <typename Value>
    using ShaderExpected = expected<Value, ShaderError>;

    // Build the `unexpected<ShaderError>` failure branch of a `ShaderExpected` / `ShaderResult`. `code`
    // and `message` are required; pass the compiler's `diagnostics` for front-end failures.
    //
    // ```cpp
    // if (SLANG_FAILED(result))
    //     return shader_error(ShaderErrorCode::CompilationFailed, "compile failed", diagnostics_text);
    // ```
    [[nodiscard]] inline unexpected<ShaderError> shader_error(ShaderErrorCode code, string message, string diagnostics = {}) {
        return unexpected(ShaderError{code, std::move(message), std::move(diagnostics)});
    }

} // namespace SFT::Core::Slang
