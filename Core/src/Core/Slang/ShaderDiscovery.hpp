#pragma once

#include <Foundation/Foundation.hpp>

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


    /// Which shader modules start-up discovery looks at. Discovery only pre-reflects shaders (it catches a broken one early and fills the
    /// reflection cache); every shader is still compiled on first use whether or not it was discovered, so leaving one out costs nothing
    /// but that early check, and saves reflecting it on a cold cache.
    struct ShaderSelection {
        /// False skips discovery altogether.
        bool enabled = true;
        /// Module names (file stems, e.g. `ui_quad`) to discover. Empty means every shader.
        vector<string> allow;
        /// Module names never to discover, whatever `allow` says.
        vector<string> deny;

        /// Whether the module called `module_name` is to be discovered.
        [[nodiscard]] bool allows(string_view module_name) const noexcept;
    };

    /// What discovery learned about where shaders come from.
    struct ShaderDiscoveryReport {
        /// The shader directory exists. When it does not, the engine runs on its embedded shaders alone and has nothing to watch for hot reload.
        bool directory_found = false;
    };

    [[nodiscard]] vector<UnCompiledShader> discover_shaders(const fs::path &directory,
                                                                   ShaderCompiler &compiler,
                                                                   const ShaderCompileOptions &options = {},
                                                                   bool enable_disk_cache = false,
                                                                   const ShaderSelection &selection = {},
                                                                   ShaderDiscoveryReport *report = nullptr);

} // namespace SFT::Core::Slang
