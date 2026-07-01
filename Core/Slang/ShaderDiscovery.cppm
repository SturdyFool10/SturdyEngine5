module;

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module Sturdy.Core:ShaderDiscovery;

import Sturdy.Foundation;
import :Shader;
import :ShaderError;
import :ShaderSource;
import :ShaderTypes;
import :ShaderReflection;

using std::error_code;
using std::string;
using std::string_view;
using std::vector;

namespace fs = std::filesystem;

export namespace SFT::Core::Slang {

    inline constexpr string_view shader_file_extension = ".slang";

    // A shader that Slang has parsed and reflected but not yet compiled to any target's bytecode.
    // It holds only source code and reflection info -- no session, module, or linked program -- so
    // discovering and reflecting every shader on disk at startup, before a graphics backend even
    // exists, costs no GPU or codegen work and leaves nothing Slang-specific to keep alive.
    // Actually generating bytecode for an entry point means recompiling `source` later via
    // ShaderCompiler.
    struct UnCompiledShader {
        ShaderSource source;
        ShaderReflection reflection;

        [[nodiscard]] string_view module_name() const noexcept {
            return source.module_name;
        }
    };

    // Recursively walks `directory` for *.slang files and reflects each one. A file that fails to
    // parse is logged and skipped rather than aborting the scan, since one broken shader shouldn't
    // stop the rest of the engine from starting up.
    [[nodiscard]] inline vector<UnCompiledShader> discover_shaders(const fs::path &directory,
                                                                   ShaderCompiler &compiler,
                                                                   const ShaderCompileOptions &options = {}) {
        vector<UnCompiledShader> shaders;

        error_code ec;
        if (!fs::is_directory(directory, ec) || ec) {
            Foundation::log_warn("Shader directory does not exist, skipping discovery: {}", directory.string());
            return shaders;
        }

        fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            const fs::directory_entry &entry = *it;
            if (!entry.is_regular_file(ec) || ec || entry.path().extension() != shader_file_extension) {
                continue;
            }

            const string path_string = entry.path().string();
            auto text = Foundation::read_file_to_string(entry.path());
            if (!text) {
                Foundation::log_error("Failed to read Slang shader file: {}", path_string);
                continue;
            }

            // Use the file stem as the module name — the same value the backend's later compile()
            // derives from the path, so a shader keeps one stable module name end to end.
            ShaderSource source = ShaderSource::from_source(entry.path().stem().string(), std::move(*text), path_string);
            auto reflected = compiler.reflect(source, options);
            if (!reflected) {
                Foundation::log_error("Failed to reflect Slang shader {}: {}", path_string, reflected.error().message);
                continue;
            }

            shaders.push_back(UnCompiledShader{std::move(source), std::move(*reflected)});
        }

        return shaders;
    }

} // namespace SFT::Core::Slang
