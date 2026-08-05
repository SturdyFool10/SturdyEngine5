#include "ShaderDiscovery.hpp"

namespace SFT::Core::Slang {

[[nodiscard]] string_view UnCompiledShader::module_name() const noexcept {
            return source.module_name;
        }

vector<UnCompiledShader> discover_shaders(const fs::path &directory,
                                                                   ShaderCompiler &compiler,
                                                                   const ShaderCompileOptions &options) {
        vector<UnCompiledShader> shaders;
        Foundation::log_info("Slang: discovering shaders under '{}'...", directory.string());
        const Foundation::Stopwatch stopwatch;
        usize considered = 0;
        usize failed = 0;

        error_code ec;
        if (!fs::is_directory(directory, ec) || ec) {
            Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                .severity = Foundation::DiagnosticSeverity::Warning,
                .code = "shader.discovery.directory_missing",
                .summary = "shader discovery directory is unavailable",
                .context = directory.string(),
                .cause_code = ec ? "filesystem.error" : "filesystem.not_found",
                .cause = ec ? ec.message() : "the configured path is not a directory",
                .details = {},
                .help = "set EngineConfig::shaders_directory to a readable shader directory",
            });
            return shaders;
        }

        fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            const fs::directory_entry &entry = *it;
            if (!entry.is_regular_file(ec) || ec || entry.path().extension() != shader_file_extension) {
                continue;
            }

            ++considered;
            const string path_string = entry.path().string();
            auto text = Foundation::read_file_to_string(entry.path());
            if (!text) {
                Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                    .code = "shader.discovery.read",
                    .summary = "could not read a shader source file",
                    .context = path_string,
                    .cause_code = "shader.file_read_failed",
                    .cause = "the file could not be opened or read completely",
                    .details = {},
                    .help = "check the file path and read permissions",
                });
                ++failed;
                continue;
            }

            // Use the file stem as the module name — the same value the backend's later compile()
            // derives from the path, so a shader keeps one stable module name end to end.
            ShaderSource source = ShaderSource::from_source(entry.path().stem().string(), std::move(*text), path_string);
            auto reflected = compiler.reflect(source, options);
            if (!reflected) {
                const ShaderError &error = reflected.error();
                Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                    .code = "shader.discovery.reflect",
                    .summary = "shader reflection failed",
                    .context = path_string,
                    .cause_code = string{shader_error_code_name(error.code)},
                    .cause = error.message,
                    .details = error.diagnostics,
                    .help = "fix the reported Slang diagnostics before starting the renderer",
                });
                ++failed;
                continue;
            }

            shaders.push_back(UnCompiledShader{std::move(source), std::move(*reflected)});
        }

        Foundation::log_info("Slang: discovered {} shader(s) ({} of {} .slang file(s) failed) in {}",
                             shaders.size(),
                             failed,
                             considered,
                             stopwatch.elapsed_human());
        return shaders;
    }

} // namespace SFT::Core::Slang
