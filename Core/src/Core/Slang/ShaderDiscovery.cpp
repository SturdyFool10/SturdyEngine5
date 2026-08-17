#include "ShaderDiscovery.hpp"

#include <Core/Slang/EmbeddedShaders.hpp>
#include <Core/Slang/ShaderCache.hpp>

#include <unordered_set>

using std::unordered_set;

namespace SFT::Core::Slang {

[[nodiscard]] string_view UnCompiledShader::module_name() const noexcept {
            return source.module_name;
        }

namespace {





    void reflect_and_append(vector<UnCompiledShader> &shaders, ShaderSource source, ShaderCompiler &compiler,
                             const ShaderCompileOptions &options, bool enable_disk_cache,
                             const std::filesystem::path &cache_directory, const string &context,
                             usize &failed, usize &cache_hits) {
        const u64 cache_key = enable_disk_cache
            ? compute_shader_cache_key(source.module_name, source.source,                       "", options)
            : 0;

        if (enable_disk_cache) {
            if (optional<ShaderReflection> cached = load_shader_reflection_cache_entry(cache_directory, cache_key)) {
                ++cache_hits;
                shaders.push_back(UnCompiledShader{std::move(source), std::move(*cached)});
                return;
            }
        }

        auto reflected = compiler.reflect(source, options);
        if (!reflected) {
            const ShaderError &error = reflected.error();
            Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                .code = "shader.discovery.reflect",
                .summary = "shader reflection failed",
                .context = context,
                .cause_code = string{shader_error_code_name(error.code)},
                .cause = error.message,
                .details = error.diagnostics,
                .help = "fix the reported Slang diagnostics before starting the renderer",
            });
            ++failed;
            return;
        }

        if (enable_disk_cache) {
            (void)store_shader_reflection_cache_entry(cache_directory, cache_key, *reflected);
        }
        shaders.push_back(UnCompiledShader{std::move(source), std::move(*reflected)});
    }

} // namespace

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
        usize embedded_fallbacks = 0;



        unordered_set<string> discovered_module_names;

        error_code ec;
        const bool directory_ok = fs::is_directory(directory, ec) && !ec;
        if (!directory_ok) {
            Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                .severity = Foundation::DiagnosticSeverity::Warning,
                .code = "shader.discovery.directory_missing",
                .summary = "shader discovery directory is unavailable; falling back to embedded shaders",
                .context = directory.string(),
                .cause_code = ec ? "filesystem.error" : "filesystem.not_found",
                .cause = ec ? ec.message() : "the configured path is not a directory",
                .details = {},
                .help = "set EngineConfig::shaders_directory to a readable shader directory to enable hot-reload",
            });
        } else {
            fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec);
            const fs::recursive_directory_iterator end;
            for (; !ec && it != end; it.increment(ec)) {
                const fs::directory_entry &entry = *it;
                if (!entry.is_regular_file(ec) || ec || entry.path().extension() != shader_file_extension) {
                    continue;
                }

                ++considered;
                const string path_string = entry.path().string();



                const string module_name = entry.path().stem().string();

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

                discovered_module_names.insert(module_name);
                ShaderSource source = ShaderSource::from_source(module_name, std::move(*text), path_string);
                reflect_and_append(shaders, std::move(source), compiler, options, enable_disk_cache,
                                    cache_directory, path_string, failed, cache_hits);
            }
        }





        for (const EmbeddedShaderSource &embedded : embedded_shaders()) {
            const string module_name{embedded.module_name};
            if (discovered_module_names.contains(module_name)) {
                continue;
            }
            ++embedded_fallbacks;
            const string context = "<embedded>/" + string{embedded.relative_path};
            ShaderSource source = ShaderSource::from_source(module_name, string{embedded.source}, context);
            reflect_and_append(shaders, std::move(source), compiler, options, enable_disk_cache,
                                cache_directory, context, failed, cache_hits);
        }

        Foundation::log_info(
            "Slang: discovered {} shader(s) ({} of {} on-disk .slang file(s) failed, {} reflection cache hit(s), "
            "{} used from the embedded fallback) in {}",
            shaders.size(), failed, considered, cache_hits, embedded_fallbacks, stopwatch.elapsed_human());
        return shaders;
    }

} // namespace SFT::Core::Slang
