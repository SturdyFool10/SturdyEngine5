#include "ShaderVariant.hpp"

namespace SFT::Core::Slang {

namespace {

    // File-kind sources only carry a path; ShaderCache's key needs the actual text (mirrors the read
    // load_shader_module() itself does in ShaderImpl.cpp) — both to hash it and, on a disk-cache
    // miss, so the key is ready without re-reading the file a second time.
    [[nodiscard]] string resolve_source_text(const ShaderSource &source) {
        if (source.kind == ShaderSourceKind::File) {
            if (auto loaded = Foundation::read_file_to_string(source.path)) {
                return std::move(*loaded);
            }
            return {};
        }
        return source.source;
    }

    [[nodiscard]] bool disk_cache_is_fresh_for_source(
        const ShaderSource &source,
        const std::filesystem::path &cache_directory,
        u64 cache_key) noexcept {
        // In-memory and embedded sources have no meaningful filesystem timestamp. Their content is
        // already part of the cache key, so keep the existing hash-only validation for those cases.
        return source.kind != ShaderSourceKind::File ||
               shader_cache_entry_is_fresh(cache_directory, cache_key, source.path);
    }

} // namespace

ShaderVariantKey::ShaderVariantKey(std::initializer_list<ShaderMacro> defines) {
            for (const ShaderMacro &define : defines) {
                set(define.name, define.value);
            }
        }

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

ShaderVariantKey &ShaderVariantKey::unset(string_view name) {
            const auto it = std::lower_bound(defines_.begin(), defines_.end(), name,
                                             [](const ShaderMacro &macro, string_view key) { return macro.name < key; });
            if (it != defines_.end() && it->name == name) {
                defines_.erase(it);
            }
            return *this;
        }

[[nodiscard]] bool ShaderVariantKey::has(string_view name) const noexcept {
            const auto it = std::lower_bound(defines_.begin(), defines_.end(), name,
                                             [](const ShaderMacro &macro, string_view key) { return macro.name < key; });
            return it != defines_.end() && it->name == name;
        }

[[nodiscard]] bool ShaderVariantKey::empty() const noexcept { return defines_.empty(); }

[[nodiscard]] const vector<ShaderMacro> &ShaderVariantKey::defines() const noexcept { return defines_; }

[[nodiscard]] const vector<ShaderMacro> &ShaderVariantKey::to_macros() const noexcept { return defines_; }

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

ShaderVariantCache::ShaderVariantCache(ShaderSource source, ShaderCompileOptions base_options, ShaderCompiler compiler,
                                       bool enable_disk_cache, std::filesystem::path disk_cache_directory)
            : compiler_(std::move(compiler)), source_(std::move(source)), base_options_(std::move(base_options)),
              enable_disk_cache_(enable_disk_cache), disk_cache_directory_(std::move(disk_cache_directory)) {}

[[nodiscard]] const ShaderSource &ShaderVariantCache::source() const noexcept { return source_; }

[[nodiscard]] const ShaderCompileOptions &ShaderVariantCache::base_options() const noexcept { return base_options_; }

void ShaderVariantCache::set_source(ShaderSource source) {
            source_ = std::move(source);
            variants_.clear();
        }

void ShaderVariantCache::invalidate() noexcept { variants_.clear(); }

void ShaderVariantCache::release_compiler_memory() noexcept {
    variants_.clear();
    compiler_.release_session();
}

[[nodiscard]] usize ShaderVariantCache::size() const noexcept { return variants_.size(); }

[[nodiscard]] bool ShaderVariantCache::contains(const ShaderVariantKey &key) const {
            return variants_.find(key.canonical()) != variants_.end();
        }

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
                            // Shader::entry_point_code() indexes row-major by entry point then target.
                            for (usize entry_point_index = 0; entry_point_index < entry_point_count; ++entry_point_index) {
                                for (const ShaderCacheTargetArtifact *artifact : requested_artifacts) {
                                    bytecode.push_back(artifact->bytecode[entry_point_index]);
                                }
                            }
                            // Multi-target D3D12 requests put SPIR-V first solely to retain portable
                            // push-constant semantics, but descriptor registers must come from DXIL's
                            // class-local b/t/u/s layout. The DXIL artifact stores that composite
                            // reflection; selecting the first (SPIR-V) artifact would map s0 as the
                            // SPIR-V descriptor binding and produce an incompatible root signature.
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
                // Keep one independently selectable artifact per requested output target. A later
                // backend switch merges its DXIL/SPIR-V artifact into this same source/variant record
                // rather than evicting an already-cached API's reflection or bytecode.
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
                        // A single-target compile owns target-native reflection. Multi-target DX12
                        // compilation currently exposes one composite reflection; never let it replace
                        // a native SPIR-V artifact populated by an earlier Vulkan compile.
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

[[nodiscard]] ShaderExpected<Shader> ShaderVariantCache::get_or_compile_base() { return get_or_compile(ShaderVariantKey{}); }

} // namespace SFT::Core::Slang
