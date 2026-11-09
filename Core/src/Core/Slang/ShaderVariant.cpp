#include <Core/Slang/ShaderPath.hpp>
#include <Core/Slang/ShaderVariant.hpp>
#include <Core/Slang/EmbeddedShaders.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

#include <Async/Mutex.hpp>

namespace SFT::Core::Slang {

namespace {


    /// Resolves source text into the concrete value used by the engine.
    ///
    /// @param source Source value or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    /// Every embedded shader's text hashed together, once. Mixed into the cache key of shaders served from the embedded
    /// copies, so an engine build that changes any of them -- including a module only `import`ed, which the per-shader
    /// key cannot see -- never reuses another build's bytecode.
    [[nodiscard]] const string &embedded_library_fingerprint() {
        static const string fingerprint = [] {
            u64 value = 0xcbf29ce484222325ull;
            for (const EmbeddedShaderSource &shader : embedded_shaders()) {
                for (const char c : string{shader.source}) {
                    value = (value ^ static_cast<u8>(c)) * 0x100000001b3ull;
                }
                value = (value ^ 0xffu) * 0x100000001b3ull;
            }
            return std::to_string(value);
        }();
        return fingerprint;
    }

    /// The names, sizes and times of every `.slang` file beside `file`, hashed. A shader on disk can `import` its siblings, and the per-shader
    /// key only sees the shader's own text, so without this an edit to a shared module served the old bytecode of everything that imports
    /// it. Memoised for a second: it is asked once per variant, and a directory scan per variant would cost more than it saves.
    [[nodiscard]] string sibling_modules_fingerprint(const std::filesystem::path &file) {
        struct Memo {
            std::filesystem::path directory;
            std::chrono::steady_clock::time_point at;
            string value;
        };
        static auto *memo = new Async::Mutex<Memo>();
        const std::filesystem::path directory = file.parent_path().empty() ? std::filesystem::path{"."} : file.parent_path();
        auto guard = memo->lock();
        const auto now = std::chrono::steady_clock::now();
        if (guard->directory == directory && now - guard->at < std::chrono::seconds{1}) {
            return guard->value;
        }
        u64 value = 0xcbf29ce484222325ull;
        const auto mix = [&value](string_view text) {
            for (const char c : text) {
                value = (value ^ static_cast<u8>(c)) * 0x100000001b3ull;
            }
            value = (value ^ 0xffu) * 0x100000001b3ull;
        };
        std::error_code error;
        for (std::filesystem::directory_iterator it{directory, std::filesystem::directory_options::skip_permission_denied, error}, end;
             !error && it != end; it.increment(error)) {
            std::error_code entry_error;
            if (!it->is_regular_file(entry_error) || it->path().extension() != ".slang") {
                continue;
            }
            mix(shader_path_to_utf8(it->path().filename()));
            mix(std::to_string(it->file_size(entry_error)));
            mix(std::to_string(it->last_write_time(entry_error).time_since_epoch().count()));
        }
        if (error) {
            // The fingerprint then misses edits to modules in this folder, so a stale cache entry could be served for them: say so, once.
            static auto *reported = new Async::Mutex<std::vector<std::filesystem::path>>();
            auto seen = reported->lock();
            if (std::find(seen->begin(), seen->end(), directory) == seen->end()) {
                seen->push_back(directory);
                Foundation::log_warn("Shader cache: could not list '{}' ({}); edits to modules imported from it may not invalidate cached shaders.",
                                     shader_path_to_utf8(directory), error.message());
            }
        }
        guard->directory = directory;
        guard->at = now;
        guard->value = std::to_string(value);
        return guard->value;
    }

    [[nodiscard]] string resolve_source_text(const ShaderSource &source) {
        if (source.kind == ShaderSourceKind::File) {
            const std::filesystem::path file = shader_path_from_utf8(source.path);
            if (auto loaded = Foundation::read_file_to_string(file)) {
                return std::move(*loaded) + "\n//sturdy-siblings:" + sibling_modules_fingerprint(file);
            }
            // Not on disk: the compiler falls back to the embedded copy, so key the cache on that text (plus the whole
            // embedded library, for its imports). Keying on "" made every embedded shader's key ignore its content.
            if (const EmbeddedShaderSource *embedded = find_embedded_shader(source.path)) {
                return string{embedded->source} + "\n//sturdy-embedded:" + embedded_library_fingerprint();
            }
            return {};
        }
        return source.source;
    }

    /// Reports whether disk cache is fresh for source.
    ///
    /// @param source Source value or resource.
    /// @param cache_directory `cache_directory` value used by the operation.
    /// @param cache_key Key used to identify the requested entry.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool disk_cache_is_fresh_for_source(
        const ShaderSource &source,
        const std::filesystem::path &cache_directory,
        u64 cache_key) noexcept {


        if (source.kind != ShaderSourceKind::File) {
            return true;
        }
        // A shader served from the embedded copy has no file to compare times with; its key already covers the embedded
        // text (see resolve_source_text), so an entry under that key is fresh by construction. Without this every embedded
        // shader recompiled on every launch (~100 ms each).
        std::error_code error;
        if (!std::filesystem::exists(shader_path_from_utf8(source.path), error) && find_embedded_shader(source.path) != nullptr) {
            return std::filesystem::exists(cache_directory, error);
        }
        return shader_cache_entry_is_fresh(cache_directory, cache_key, shader_path_from_utf8(source.path));
    }

    /// A compile that failed, remembered process-wide. Renderer features build their pipelines from `ensure_*` calls that run
    /// every frame and each makes a fresh `ShaderVariantCache`; without this a shader that cannot compile is recompiled from
    /// scratch (and its error re-logged) on every frame, which presents as an endless loop inside shader compilation.
    struct RememberedShaderFailure {
        ShaderError error;
        std::chrono::steady_clock::time_point failed_at;
        u64 suppressed = 0;
    };

    /// How long a failed compile is answered from memory before it is tried again. A retry (rather than "never") lets a fix to a
    /// file the shader only `import`s -- which the key cannot see -- take effect without a restart.
    constexpr std::chrono::seconds shader_failure_retry_interval{5};

    Async::Mutex<std::unordered_map<u64, RememberedShaderFailure>> &remembered_shader_failures() {
        static auto *table = new Async::Mutex<std::unordered_map<u64, RememberedShaderFailure>>();
        return *table;
    }

    /// Key of one (source text, variant, options) compile. Covers the same inputs as the disk-cache key, so editing the shader
    /// (or changing a macro, target or override) is a different key and compiles at once.
    [[nodiscard]] u64 failure_memo_key(const ShaderSource &source, string_view variant_canonical, const ShaderCompileOptions &options) {
        const u64 text_key = compute_shader_cache_key(source.module_name, resolve_source_text(source), variant_canonical, options);
        // The path is not part of the text key, and two files can hold identical text.
        u64 value = text_key;
        for (const char c : source.path) {
            value = (value ^ static_cast<u8>(c)) * 0x100000001b3ull;
        }
        return value;
    }

    /// The remembered failure for `key`, if one is still within the retry interval.
    [[nodiscard]] optional<ShaderError> recall_shader_failure(u64 key) {
        auto table = remembered_shader_failures().lock();
        const auto it = table->find(key);
        if (it == table->end()) {
            return nullopt;
        }
        if (std::chrono::steady_clock::now() - it->second.failed_at >= shader_failure_retry_interval) {
            table->erase(it);
            return nullopt;
        }
        ++it->second.suppressed;
        return it->second.error;
    }

    void remember_shader_failure(u64 key, const ShaderSource &source, const ShaderError &error) {
        u64 suppressed_before = 0;
        {
            auto table = remembered_shader_failures().lock();
            if (const auto it = table->find(key); it != table->end()) {
                suppressed_before = it->second.suppressed;
            }
            (*table)[key] = RememberedShaderFailure{.error = error, .failed_at = std::chrono::steady_clock::now(), .suppressed = 0};
        }
        const string where = source.kind == ShaderSourceKind::File ? source.path : source.module_name;
        Foundation::log_error(
            "Shader '{}' failed to compile ({}): {}{}{}\n  It will not be recompiled for {}s unless its source changes{}.",
            where, shader_error_code_name(error.code), error.message, error.diagnostics.empty() ? "" : "\n", error.diagnostics,
            shader_failure_retry_interval.count(),
            suppressed_before > 0 ? std::format("; {} repeat request(s) were answered from memory", suppressed_before) : string{});
    }

} // namespace

/// Performs the shader variant key operation for `Slang` using the supplied arguments.
///
/// @param defines `defines` value used by the operation.
///
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
ShaderVariantKey::ShaderVariantKey(std::initializer_list<ShaderMacro> defines) {
            for (const ShaderMacro &define : defines) {
                set(define.name, define.value);
            }
        }

/// Performs the set operation for `Slang` using the supplied arguments.
///
/// @param name Name used to identify or label the target.
/// @param value Value consumed by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
ShaderVariantKey &ShaderVariantKey::set(string name, string value) {
            const auto it = std::lower_bound(defines_.begin(), defines_.end(), name,
                                             [](const ShaderMacro &macro, const string &key) { return macro.name < key; });
            if (it != defines_.end() && it->name == name) {
                it->value = std::move(value);
            } else {
                defines_.insert(it, ShaderMacro{std::move(name), std::move(value)});
            }
            return *this;
        }

/// Performs the unset operation for `Slang` using the supplied arguments.
///
/// @param name Name used to identify or label the target.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
ShaderVariantKey &ShaderVariantKey::unset(string_view name) {
            const auto it = std::lower_bound(defines_.begin(), defines_.end(), name,
                                             [](const ShaderMacro &macro, string_view key) { return macro.name < key; });
            if (it != defines_.end() && it->name == name) {
                defines_.erase(it);
            }
            return *this;
        }

/// Performs the has operation for `Slang` using the supplied arguments.
///
/// @param name Name used to identify or label the target.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] bool ShaderVariantKey::has(string_view name) const noexcept {
            const auto it = std::lower_bound(defines_.begin(), defines_.end(), name,
                                             [](const ShaderMacro &macro, string_view key) { return macro.name < key; });
            return it != defines_.end() && it->name == name;
        }

/// Reports whether this `Slang` contains no elements or payload.
///
/// @return Returns the current empty value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool ShaderVariantKey::empty() const noexcept { return defines_.empty(); }

/// Returns the current or globally available defines value.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const vector<ShaderMacro> &ShaderVariantKey::defines() const noexcept { return defines_; }

/// Converts the value to macros representation.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const vector<ShaderMacro> &ShaderVariantKey::to_macros() const noexcept { return defines_; }

/// Returns the current or globally available canonical value.
///
/// @return Returns the current canonical value.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] string ShaderVariantKey::canonical() const {
            string out;
            for (const ShaderMacro &define : defines_) {
                if (!out.empty()) {
                    out.push_back(';');
                }
                out.append(define.name);
                out.push_back('=');
                out.append(define.value);
            }
            return out;
        }

/// Hashes the supplied or associated value/state using the supplied arguments and current state.
///
/// @return Returns the current hash value.
/// @note This function does not throw exceptions.
[[nodiscard]] u64 ShaderVariantKey::hash() const noexcept {
            u64 value = 0xcbf29ce484222325ull;
            for (const ShaderMacro &define : defines_) {
                for (const char c : define.name) {
                    value = (value ^ static_cast<u8>(c)) * 0x100000001b3ull;
                }
                value = (value ^ static_cast<u8>('=')) * 0x100000001b3ull;
                for (const char c : define.value) {
                    value = (value ^ static_cast<u8>(c)) * 0x100000001b3ull;
                }
                value = (value ^ static_cast<u8>(';')) * 0x100000001b3ull;
            }
            return value;
        }

/// Performs the shader variant cache operation for `Slang` using the supplied arguments.
///
/// @param source Source value or resource.
/// @param base_options Configuration values controlling the operation.
/// @param compiler `compiler` value used by the operation.
/// @param enable_disk_cache Whether the associated behavior is enabled.
/// @param disk_cache_directory `disk_cache_directory` value used by the operation.
///
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
ShaderVariantCache::ShaderVariantCache(ShaderSource source, ShaderCompileOptions base_options, ShaderCompiler compiler,
                                       bool enable_disk_cache, std::filesystem::path disk_cache_directory)
            : compiler_(std::move(compiler)), source_(std::move(source)), base_options_(std::move(base_options)),
              enable_disk_cache_(enable_disk_cache), disk_cache_directory_(std::move(disk_cache_directory)) {}

/// Returns the current or globally available source value.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const ShaderSource &ShaderVariantCache::source() const noexcept { return source_; }

/// Returns the current or globally available base options value.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const ShaderCompileOptions &ShaderVariantCache::base_options() const noexcept { return base_options_; }

/// Sets the source for this `Slang`.
///
/// @param source Source value or resource.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void ShaderVariantCache::set_source(ShaderSource source) {
            source_ = std::move(source);
            variants_.clear();
        }

/// Returns the current or globally available invalidate value.
///
/// @return Returns the current invalidate value.
/// @note This function does not throw exceptions.
void ShaderVariantCache::invalidate() noexcept {
    variants_.clear();
    // An explicit invalidate is a request to try again (a hot-reload after fixing an imported file, say), so remembered failures go too.
    try {
        remembered_shader_failures().lock()->clear();
    } catch (const std::exception &exception) {
        Foundation::log_warn("Shader cache: could not clear the remembered shader failures ({}); a fixed shader may keep reporting its old error for a few seconds.",
                             exception.what());
    } catch (...) {
        Foundation::log_warn("Shader cache: could not clear the remembered shader failures (unknown exception).");
    }
}

/// Releases compiler memory using the supplied arguments and current state.
///
/// @return Returns the current release compiler memory value.
/// @note This function does not throw exceptions.
void ShaderVariantCache::release_compiler_memory() noexcept {
    variants_.clear();
    compiler_.release_session();
}

/// Returns the size for this `Slang`.
///
/// @return Returns the current size value.
/// @note This function does not throw exceptions.
[[nodiscard]] usize ShaderVariantCache::size() const noexcept { return variants_.size(); }

/// Reports whether contains holds for this `Slang`.
///
/// @param key Key used to identify the requested entry.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] bool ShaderVariantCache::contains(const ShaderVariantKey &key) const {
            return variants_.find(key.canonical()) != variants_.end();
        }

/// Returns the or compile associated with this `Slang`.
///
/// @param key Key used to identify the requested entry.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
[[nodiscard]] ShaderExpected<Shader> ShaderVariantCache::get_or_compile(const ShaderVariantKey &key) {
            const string canonical = key.canonical();
            if (const auto it = variants_.find(canonical); it != variants_.end()) {
                return it->second;
            }

            ShaderCompileOptions options = base_options_;
            options.macros.reserve(options.macros.size() + key.defines().size());
            for (const ShaderMacro &define : key.to_macros()) {
                options.macros.push_back(define);
            }

            const string source_text = enable_disk_cache_ ? resolve_source_text(source_) : string{};
            const u64 disk_cache_key = enable_disk_cache_
                ? compute_shader_cache_key(source_.module_name, source_text, canonical, options)
                : 0;

            const auto same_target = [](const ShaderTarget &lhs, const ShaderTarget &rhs) {
                return lhs.format == rhs.format && lhs.profile == rhs.profile;
            };
            if (enable_disk_cache_ && disk_cache_is_fresh_for_source(source_, disk_cache_directory_, disk_cache_key)) {
                auto loaded_entry = load_shader_cache_entry(disk_cache_directory_, disk_cache_key);
                if (!loaded_entry) {
                    report_shader_cache_issue(loaded_entry.error(), "loading the cache entry of", source_.module_name);
                }
                if (auto entry = std::move(loaded_entry)) {
                    vector<const ShaderCacheTargetArtifact *> requested_artifacts;
                    requested_artifacts.reserve(options.targets.size());
                    for (const ShaderTarget &target : options.targets) {
                        const auto found = std::ranges::find_if(
                            entry->artifacts,
                            [&target, &same_target](const ShaderCacheTargetArtifact &artifact) {
                                return same_target(artifact.target, target);
                            });
                        if (found == entry->artifacts.end()) {
                            requested_artifacts.clear();
                            break;
                        }
                        requested_artifacts.push_back(&*found);
                    }

                    if (requested_artifacts.empty()) {
                        Foundation::log_debug("Shader cache: the entry of '{}' has no artifact for every requested target; compiling.", source_.module_name);
                    }
                    if (!requested_artifacts.empty()) {
                        const usize entry_point_count = requested_artifacts.front()->reflection.entry_points.size();
                        bool complete = entry_point_count != 0;
                        for (const ShaderCacheTargetArtifact *artifact : requested_artifacts) {
                            complete &= artifact->reflection.entry_points.size() == entry_point_count &&
                                        artifact->bytecode.size() == entry_point_count;
                        }
                        if (!complete) {
                            Foundation::log_debug("Shader cache: the entry of '{}' is incomplete (entry points or bytecode missing for a target); compiling.",
                                                  source_.module_name);
                        }
                        if (complete) {
                            vector<ShaderBytecode> bytecode;
                            bytecode.reserve(entry_point_count * requested_artifacts.size());

                            for (usize entry_point_index = 0; entry_point_index < entry_point_count; ++entry_point_index) {
                                for (const ShaderCacheTargetArtifact *artifact : requested_artifacts) {
                                    bytecode.push_back(artifact->bytecode[entry_point_index]);
                                }
                            }


                            const ShaderCacheTargetArtifact *reflection_artifact = requested_artifacts.front();
                            if (const auto dxil = std::ranges::find_if(
                                    requested_artifacts,
                                    [](const ShaderCacheTargetArtifact *artifact) {
                                        return artifact->target.format == ShaderTargetFormat::Dxil;
                                    });
                                dxil != requested_artifacts.end()) {
                                reflection_artifact = *dxil;
                            }
                            Shader baked = compiler_.from_cached_bytecode(
                                entry->module_name, options.targets,
                                reflection_artifact->reflection, std::move(bytecode));
                            const auto [inserted, _] = variants_.emplace(canonical, std::move(baked));
                            return inserted->second;
                        }
                    }
                }
            }

            const u64 failure_key = failure_memo_key(source_, canonical, options);
            if (optional<ShaderError> remembered = recall_shader_failure(failure_key)) {
                return unexpected(std::move(*remembered));
            }

            ShaderExpected<Shader> compiled = compiler_.compile(source_, options);
            if (!compiled) {
                remember_shader_failure(failure_key, source_, compiled.error());
                return compiled;
            }

            // Slang can compile a module that it then cannot generate code for (no DXC for DXIL, a construct a target rejects).
            // Callers only find that out when they ask for bytecode, so check here: a failure then reaches the failure memo
            // instead of being rediscovered, after a full recompile, on every frame.
            if (!enable_disk_cache_) {
                const usize entry_point_total = compiled->reflection().entry_points.size();
                for (usize target_index = 0; target_index < options.targets.size(); ++target_index) {
                    for (usize entry_point_index = 0; entry_point_index < entry_point_total; ++entry_point_index) {
                        if (ShaderExpected<ShaderBytecode> bytecode = compiled->entry_point_code(entry_point_index, target_index);
                            !bytecode) {
                            remember_shader_failure(failure_key, source_, bytecode.error());
                            return unexpected(std::move(bytecode.error()));
                        }
                    }
                }
            }

            if (enable_disk_cache_) {
                optional<ShaderError> codegen_error;

                ShaderCacheEntry entry =
                    disk_cache_is_fresh_for_source(source_, disk_cache_directory_, disk_cache_key)
                        ? load_shader_cache_entry(disk_cache_directory_, disk_cache_key).value_or(ShaderCacheEntry{})
                        : ShaderCacheEntry{};
                entry.module_name = string{compiled->module_name()};
                const ShaderReflection &reflection = compiled->reflection();
                bool all_ok = true;
                for (usize target_index = 0; target_index < options.targets.size(); ++target_index) {
                    ShaderCacheTargetArtifact artifact{
                        .target = options.targets[target_index],
                        .reflection = reflection,
                        .bytecode = {},
                    };
                    artifact.bytecode.reserve(reflection.entry_points.size());
                    for (usize entry_point_index = 0; entry_point_index < reflection.entry_points.size(); ++entry_point_index) {
                        ShaderExpected<ShaderBytecode> bytecode = compiled->entry_point_code(entry_point_index, target_index);
                        if (!bytecode) {
                            all_ok = false;
                            codegen_error = bytecode.error();
                            break;
                        }
                        artifact.bytecode.push_back(std::move(*bytecode));
                    }
                    if (!all_ok) {
                        break;
                    }
                    const auto existing = std::ranges::find_if(
                        entry.artifacts,
                        [&artifact, &same_target](const ShaderCacheTargetArtifact &candidate) {
                            return same_target(candidate.target, artifact.target);
                        });
                    if (existing == entry.artifacts.end()) {
                        entry.artifacts.push_back(std::move(artifact));
                    } else if (options.targets.size() == 1) {


                        *existing = std::move(artifact);
                    }
                }
                if (codegen_error) {
                    remember_shader_failure(failure_key, source_, *codegen_error);
                    return unexpected(std::move(*codegen_error));
                }
                if (all_ok) {
                    if (auto stored = store_shader_cache_entry(disk_cache_directory_, disk_cache_key, entry); !stored) {
                        // Not fatal -- the shader works, it just recompiles next launch -- but a cache that never saves is otherwise silent.
                        report_shader_cache_issue(stored.error(), "storing the cache entry of",
                                                  source_.kind == ShaderSourceKind::File ? source_.path : source_.module_name);
                    }
                }
            }

            const auto [inserted, _] = variants_.emplace(canonical, std::move(*compiled));
            return inserted->second;
        }

/// Returns the or compile base associated with this `Slang`.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
[[nodiscard]] ShaderExpected<Shader> ShaderVariantCache::get_or_compile_base() { return get_or_compile(ShaderVariantKey{}); }

} // namespace SFT::Core::Slang

namespace SFT::Core::Slang {

    /// Compares the operands for equality.
    ///
    /// @param a `a` value used by the operation.
    /// @param b `b` value used by the operation.
    ///
    /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    bool operator==(const ShaderVariantKey &a, const ShaderVariantKey &b) noexcept {
        if (a.defines_.size() != b.defines_.size()) {
            return false;
        }
        for (usize i = 0; i < a.defines_.size(); ++i) {
            if (a.defines_[i].name != b.defines_[i].name || a.defines_[i].value != b.defines_[i].value) {
                return false;
            }
        }
        return true;
    }

} // namespace SFT::Core::Slang

