module;

#include <concepts>
#include <string>
#include <string_view>

export module Sturdy.Core:ShaderSource;

import Sturdy.Foundation;

using std::convertible_to;
using std::string;
using std::string_view;

export namespace SFT::Core::Slang {

    enum class ShaderSourceKind {
        SourceString,
        File,
    };

    struct ShaderSource {
        ShaderSourceKind kind = ShaderSourceKind::SourceString;
        string module_name;
        string path;
        string source;

        [[nodiscard]] static ShaderSource from_source(string module_name, string source, string path = {});
        [[nodiscard]] static ShaderSource from_file(string path, string module_name = {});
    };

    // Satisfied by any type that provides static constexpr module_name and source string views.
    // Allows embedding shaders as types and compiling them without runtime file I/O.
    template <typename StaticShader>
    concept StaticShaderSource = requires {
        { StaticShader::module_name } -> convertible_to<string_view>;
        { StaticShader::source } -> convertible_to<string_view>;
    };

    template <StaticShaderSource StaticShader>
    [[nodiscard]] ShaderSource shader_source_from_type() {
        ShaderSource source{};
        source.kind = ShaderSourceKind::SourceString;
        source.module_name = string{string_view{StaticShader::module_name}};
        source.source = string{string_view{StaticShader::source}};

        if constexpr (requires { { StaticShader::path } -> convertible_to<string_view>; }) {
            source.path = string{string_view{StaticShader::path}};
        }

        return source;
    }

} // namespace SFT::Core::Slang
