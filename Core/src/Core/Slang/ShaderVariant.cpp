#include <Core/Slang/ShaderVariant.hpp>

namespace SFT::Core::Slang {

namespace {


    /// Resolves source text into the concrete value used by the engine.
    ///
    /// @param source Source value or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] string resolve_source_text(const ShaderSource &source) {
        if (source.kind == ShaderSourceKind::File) {
            if (auto loaded = Foundation::read_file_to_string(source.path)) {
                return std::move(*loaded);
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


        return source.kind != ShaderSourceKind::File ||
               shader_cache_entry_is_fresh(cache_directory, cache_key, source.path);
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
void ShaderVariantCache::invalidate() noexcept { variants_.clear(); }

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
                if (auto entry = load_shader_cache_entry(disk_cache_directory_, disk_cache_key)) {
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

                    if (!requested_artifacts.empty()) {
                        const usize entry_point_count = requested_artifacts.front()->reflection.entry_points.size();
                        bool complete = entry_point_count != 0;
                        for (const ShaderCacheTargetArtifact *artifact : requested_artifacts) {
                            complete &= artifact->reflection.entry_points.size() == entry_point_count &&
                                        artifact->bytecode.size() == entry_point_count;
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

            ShaderExpected<Shader> compiled = compiler_.compile(source_, options);
            if (!compiled) {
                return compiled;
            }

            if (enable_disk_cache_) {


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
                if (all_ok) {
                    (void)store_shader_cache_entry(disk_cache_directory_, disk_cache_key, entry);
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

