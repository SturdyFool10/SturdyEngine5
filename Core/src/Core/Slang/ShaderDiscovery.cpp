#include <Core/Slang/ShaderPath.hpp>
#include <algorithm>
#include <Core/Slang/ShaderDiscovery.hpp>

#include <Core/Slang/EmbeddedShaders.hpp>
#include <Core/Slang/ShaderCache.hpp>

#include <atomic>
#include <optional>
#include <thread>
#include <unordered_set>

using std::unordered_set;

namespace SFT::Core::Slang {

/// Returns a human-readable name for the supplied module value.
///
/// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
/// @note This function does not throw exceptions.
[[nodiscard]] string_view UnCompiledShader::module_name() const noexcept {
            return source.module_name;
        }

namespace {


    /// One shader waiting to be reflected.
    struct DiscoveryJob {
        ShaderSource source;
        /// Where it came from, for messages: the file path, or `<embedded>/...`.
        string context;
    };

    /// What reflecting one job produced; empty when it failed (already reported).
    struct DiscoveryResult {
        std::optional<UnCompiledShader> shader;
        bool cache_hit = false;
        bool failed = false;
    };

    /// Reflects one shader, from the reflection cache when it is there. `compiler` is only used on a cache miss.
    DiscoveryResult reflect_one(DiscoveryJob &job, ShaderCompiler &compiler, const ShaderCompileOptions &options,
                                bool enable_disk_cache, const std::filesystem::path &cache_directory) {
        DiscoveryResult result{};
        ShaderSource source = std::move(job.source);
        const u64 cache_key = enable_disk_cache
            ? compute_shader_cache_key(source.module_name, source.source, "", options)
            : 0;

        if (enable_disk_cache) {
            auto cached = load_shader_reflection_cache_entry(cache_directory, cache_key);
            if (cached) {
                result.cache_hit = true;
                result.shader = UnCompiledShader{std::move(source), std::move(*cached)};
                return result;
            }
            report_shader_cache_issue(cached.error(), "loading the reflection cache entry of", job.context);
        }

        auto reflected = compiler.reflect(source, options);
        if (!reflected) {
            const ShaderError &error = reflected.error();
            Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                .code = "shader.discovery.reflect",
                .summary = "shader reflection failed",
                .context = job.context,
                .cause_code = string{shader_error_code_name(error.code)},
                .cause = error.message,
                .details = error.diagnostics,
                .help = "fix the reported Slang diagnostics before starting the renderer",
            });
            result.failed = true;
            return result;
        }

        if (enable_disk_cache) {
            if (auto stored = store_shader_reflection_cache_entry(cache_directory, cache_key, *reflected); !stored) {
                report_shader_cache_issue(stored.error(), "storing the reflection cache entry of", job.context);
            }
        }
        result.shader = UnCompiledShader{std::move(source), std::move(*reflected)};
        return result;
    }

    /// Reflects every job, several at a time, and returns the results in job order.
    ///
    /// A cold cache means reflecting every shader through Slang, tens of milliseconds each. Slang allows distinct sessions on distinct threads
    /// but serializes creating them (60-110 ms each), so the extra workers each bring a session of their own: that pays for itself once there
    /// are more than a handful of shaders to reflect. Cache hits need no session at all and are read on whichever worker gets to them.
    vector<DiscoveryResult> reflect_all(vector<DiscoveryJob> &jobs, ShaderCompiler &compiler, const ShaderCompileOptions &options,
                                        bool enable_disk_cache, const std::filesystem::path &cache_directory) {
        vector<DiscoveryResult> results(jobs.size());
        constexpr usize jobs_per_extra_worker = 12;
        const usize hardware = std::max<usize>(1, std::thread::hardware_concurrency());
        const usize workers = std::clamp<usize>(jobs.size() / jobs_per_extra_worker, 1, std::min<usize>(hardware, 8));
        std::atomic<usize> next{0};
        const auto work = [&](ShaderCompiler &worker_compiler) {
            for (usize index = next.fetch_add(1); index < jobs.size(); index = next.fetch_add(1)) {
                results[index] = reflect_one(jobs[index], worker_compiler, options, enable_disk_cache, cache_directory);
            }
        };
        vector<std::thread> threads;
        for (usize worker = 1; worker < workers; ++worker) {
            threads.emplace_back([&work] {
                ShaderCompiler own = ShaderCompiler::isolated();
                work(own);
            });
        }
        work(compiler);
        for (std::thread &thread : threads) {
            thread.join();
        }
        return results;
    }

} // namespace

/// Performs the discover shaders operation for `Slang` using the supplied arguments.
///
/// @param directory `directory` value used by the operation.
/// @param compiler `compiler` value used by the operation.
/// @param options Configuration values controlling the operation.
/// @param enable_disk_cache Whether the associated behavior is enabled.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
bool ShaderSelection::allows(string_view module_name) const noexcept {
        if (!enabled) {
            return false;
        }
        const auto listed = [module_name](const vector<string> &names) {
            return std::find(names.begin(), names.end(), module_name) != names.end();
        };
        if (listed(deny)) {
            return false;
        }
        return allow.empty() || listed(allow);
    }

vector<UnCompiledShader> discover_shaders(const fs::path &directory,
                                                                   ShaderCompiler &compiler,
                                                                   const ShaderCompileOptions &options,
                                                                   bool enable_disk_cache,
                                                                   const ShaderSelection &selection,
                                                                   ShaderDiscoveryReport *report) {
        vector<UnCompiledShader> shaders;
        if (!selection.enabled) {
            // The directory is still looked at once, so the engine knows whether it has anything to watch.
            std::error_code probe_ec;
            if (report != nullptr) {
                report->directory_found = fs::is_directory(directory, probe_ec) && !probe_ec;
            }
            Foundation::log_info("Slang: shader discovery is turned off; shaders are reflected when first used.");
            return shaders;
        }
        Foundation::log_info("Slang: discovering shaders under '{}'...", directory.string());
        const Foundation::Stopwatch stopwatch;
        const std::filesystem::path cache_directory = shader_cache_directory();
        usize considered = 0;
        usize failed = 0;
        usize cache_hits = 0;
        usize embedded_fallbacks = 0;


        unordered_set<string> discovered_module_names;
        vector<DiscoveryJob> jobs;

        error_code ec;
        const bool directory_ok = fs::is_directory(directory, ec) && !ec;
        if (report != nullptr) {
            report->directory_found = directory_ok;
        }
        if (!directory_ok) {
            // Not a problem: an installed application ships its shaders inside the executable. Looked at once; nothing is watched afterwards.
            Foundation::log_info("Slang: shader directory '{}' not found ({}); running on the embedded shaders only, with hot reload off.",
                                 directory.string(), ec ? ec.message() : string{"not a directory"});
            ec.clear(); // reported just above; the "listing stopped early" warning below is for a directory that existed
        } else {
            fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec);
            const fs::recursive_directory_iterator end;
            for (; !ec && it != end; it.increment(ec)) {
                const fs::directory_entry &entry = *it;
                std::error_code entry_ec;
                if (!entry.is_regular_file(entry_ec) || entry_ec || entry.path().extension() != shader_file_extension) {
                    continue;
                }

                ++considered;
                const string path_string = shader_path_to_utf8(entry.path());


                const string module_name = shader_path_to_utf8(entry.path().stem());

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
                if (!selection.allows(module_name)) {
                    continue;
                }
                jobs.push_back(DiscoveryJob{ShaderSource::from_source(module_name, std::move(*text), path_string), path_string});
            }
        }


        if (ec) {
            // Not a missing directory (reported above) but one that stopped being listable part way: the shaders after that point were skipped,
            // and without this line the summary below would read as if every shader had been seen.
            Foundation::log_warn("Slang: listing '{}' stopped early ({}); shaders after that point were not discovered.", directory.string(), ec.message());
        }

        for (const EmbeddedShaderSource &embedded : embedded_shaders()) {
            const string module_name{embedded.module_name};
            if (discovered_module_names.contains(module_name) || !selection.allows(module_name)) {
                continue;
            }
            ++embedded_fallbacks;
            const string context = "<embedded>/" + string{embedded.relative_path};
            jobs.push_back(DiscoveryJob{ShaderSource::from_source(module_name, string{embedded.source}, context), context});
        }

        for (DiscoveryResult &result : reflect_all(jobs, compiler, options, enable_disk_cache, cache_directory)) {
            cache_hits += result.cache_hit ? 1 : 0;
            failed += result.failed ? 1 : 0;
            if (result.shader) {
                shaders.push_back(std::move(*result.shader));
            }
        }

        Foundation::log_info(
            "Slang: discovered {} shader(s) ({} of {} on-disk .slang file(s) failed, {} reflection cache hit(s), "
            "{} used from the embedded fallback) in {}",
            shaders.size(), failed, considered, cache_hits, embedded_fallbacks, stopwatch.elapsed_human());
        return shaders;
    }

} // namespace SFT::Core::Slang
