#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#pragma endregion

using std::expected;
using std::string;
using std::string_view;
using std::unexpected;

namespace SFT::Core::Slang {


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

    /// Returns a human-readable name for the supplied shader error code value.
    ///
    /// @param code `code` value used by the operation.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr string_view shader_error_code_name(ShaderErrorCode code) noexcept {
        switch (code) {
            case ShaderErrorCode::InitializationFailed: return "shader.initialization_failed";
            case ShaderErrorCode::InvalidArgument: return "shader.invalid_argument";
            case ShaderErrorCode::FileReadFailed: return "shader.file_read_failed";
            case ShaderErrorCode::CompilationFailed: return "shader.compilation_failed";
            case ShaderErrorCode::ReflectionFailed: return "shader.reflection_failed";
            case ShaderErrorCode::EntryPointNotFound: return "shader.entry_point_not_found";
            case ShaderErrorCode::CodeGenerationFailed: return "shader.code_generation_failed";
            case ShaderErrorCode::OutOfMemory: return "shader.out_of_memory";
            case ShaderErrorCode::OperationFailed: return "shader.operation_failed";
        }
        return "shader.unknown";
    }


    struct ShaderError {
        ShaderErrorCode code = ShaderErrorCode::OperationFailed;
        string message;
        string diagnostics;
    };


    using ShaderResult = expected<void, ShaderError>;


    template <typename Value>
    using ShaderExpected = expected<Value, ShaderError>;


    [[nodiscard]] unexpected<ShaderError> shader_error(ShaderErrorCode code, string message, string diagnostics = {});

} // namespace SFT::Core::Slang
