#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <concepts>
#include <string>
#include <string_view>
#pragma endregion

using std::convertible_to;
using std::string;
using std::string_view;

namespace SFT::Core::Slang {


    enum class ShaderSourceKind {
        SourceString,
        File,
    };


    struct ShaderSource {
        ShaderSourceKind kind = ShaderSourceKind::SourceString;
        string module_name;
        string path;
        string source;


        /// Creates or converts a value from source representation.
        ///
        /// @param module_name Name used to identify or label the target.
        /// @param source Source value or resource.
        /// @param path Filesystem path identifying the target resource.
        ///
        /// @return Returns the newly constructed or converted value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static ShaderSource from_source(string module_name, string source, string path = {});


        /// Creates or converts a value from file representation.
        ///
        /// @param path Filesystem path identifying the target resource.
        /// @param module_name Name used to identify or label the target.
        ///
        /// @return Returns the newly constructed or converted value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static ShaderSource from_file(string path, string module_name = {});
    };


    template <typename StaticShader>
    concept StaticShaderSource = requires {
        { StaticShader::module_name } -> convertible_to<string_view>;
        { StaticShader::source } -> convertible_to<string_view>;
    };


    /// Returns the current or globally available shader source from type value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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
