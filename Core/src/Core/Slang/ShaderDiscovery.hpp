#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#pragma endregion

#include <Core/Slang/Shader.hpp>
#include <Core/Slang/ShaderError.hpp>
#include <Core/Slang/ShaderSource.hpp>
#include <Core/Slang/ShaderTypes.hpp>
#include <Core/Slang/ShaderReflection.hpp>

using std::error_code;
using std::string;
using std::string_view;
using std::vector;

namespace fs = std::filesystem;

namespace SFT::Core::Slang {


    inline constexpr string_view shader_file_extension = ".slang";


    struct UnCompiledShader {
        ShaderSource source;
        ShaderReflection reflection;


        /// Returns a human-readable name for the supplied module value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] string_view module_name() const noexcept;
    };


    [[nodiscard]] vector<UnCompiledShader> discover_shaders(const fs::path &directory,
                                                                   ShaderCompiler &compiler,
                                                                   const ShaderCompileOptions &options = {},
                                                                   bool enable_disk_cache = false);

} // namespace SFT::Core::Slang
