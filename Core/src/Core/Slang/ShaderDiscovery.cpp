#include "ShaderDiscovery.hpp"

namespace SFT::Core::Slang {

[[nodiscard]] string_view UnCompiledShader::module_name() const noexcept {
            return source.module_name;
        }

vector<UnCompiledShader> discover_shaders(const fs::path &directory,
                                                                   ShaderCompiler &compiler,
                                                                   const ShaderCompileOptions &options) {
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
