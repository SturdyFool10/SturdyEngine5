#include "ShaderDiscovery.hpp"

#include <Core/Slang/ShaderCache.hpp>

namespace SFT::Core::Slang {

[[nodiscard]] string_view UnCompiledShader::module_name() const noexcept {
            return source.module_name;
        }

vector<UnCompiledShader> discover_shaders(const fs::path &directory,
                                                                   ShaderCompiler &compiler,
                                                                   const ShaderCompileOptions &options,
                                                                   bool enable_disk_cache) {
        vector<UnCompiledShader> shaders;
        Foundation::log_info("Slang: discovering shaders under '{}'...", directory.string());
        const Foundation::Stopwatch stopwatch;
        const std::filesystem::path cache_directory{string{default_shader_cache_directory}};
        usize considered = 0;
        usize failed = 0;
        usize cache_hits = 0;

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
            const string module_name = entry.path().stem().string();
            const u64 cache_key = enable_disk_cache
                ? compute_shader_cache_key(module_name, *text, /*variant_canonical=*/"", options)
                : 0;

            optional<ShaderReflection> cached_reflection;
            if (enable_disk_cache) {
                cached_reflection = load_shader_reflection_cache_entry(cache_directory, cache_key);
            }

            ShaderSource source = ShaderSource::from_source(module_name, std::move(*text), path_string);
            if (cached_reflection) {
                ++cache_hits;
                shaders.push_back(UnCompiledShader{std::move(source), std::move(*cached_reflection)});
                continue;
            }

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

            if (enable_disk_cache) {
                (void)store_shader_reflection_cache_entry(cache_directory, cache_key, *reflected);
            }
            shaders.push_back(UnCompiledShader{std::move(source), std::move(*reflected)});
        }

        Foundation::log_info("Slang: discovered {} shader(s) ({} of {} .slang file(s) failed, {} reflection cache hit(s)) in {}",
                             shaders.size(),
                             failed,
                             considered,
                             cache_hits,
                             stopwatch.elapsed_human());
        return shaders;
    }

} // namespace SFT::Core::Slang
