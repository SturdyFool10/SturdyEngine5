#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#pragma endregion

#include <Core/Slang/ShaderReflection.hpp>
#include <Core/Slang/ShaderTypes.hpp>

using std::optional;
using std::string;
using std::string_view;
using std::vector;

namespace SFT::Core::Slang {

    // ─────────────────────────────────────────────────────────────────────────────────────────────
    //  On-disk shader cache: persists a compiled variant's bytecode + reflection across runs, so a
    //  ShaderVariantCache::get_or_compile() call can skip Slang entirely on a cache hit (see
    //  ShaderCompiler::from_cached_bytecode() / Shader's "baked" mode, Shader.hpp). Opt-in via
    //  EngineConfig::enable_shader_disk_cache (Engine/EngineModule.hpp), wired through
    //  ShaderVariantCache's constructor.
    // ─────────────────────────────────────────────────────────────────────────────────────────────

    // Default cache location relative to the working directory — mirrors the repo-root `/.cache/`
    // naming convention ("generated, not source"), scoped under Shaders/ since these files are keyed
    // to shader content specifically. See .gitignore's matching `/Shaders/.cache/` entry.
    inline constexpr string_view default_shader_cache_directory = "Shaders/.cache";

    // Everything needed to reconstruct a baked (Slang-free) `Shader` from disk: the module identity,
    // the resolved target list (so bytecode indices line up the same way Shader::entry_point_code()
    // expects — row-major `entry_point_index * targets.size() + target_index`), the full reflection,
    // and one ShaderBytecode per entry point x target.
    struct ShaderCacheEntry {
        string module_name;
        vector<ShaderTarget> targets;
        ShaderReflection reflection;
        vector<ShaderBytecode> bytecode;
    };

    // Content hash over everything that can change a compiled result: the shader's full source text,
    // the variant's canonical define string (ShaderVariantKey::canonical()), and every compile option
    // that affects codegen. Deliberately NOT based on file mtime — mtime is what ShaderWatcher already
    // uses for hot-reload polling (weak signal: touch-without-edit, git checkouts don't preserve it),
    // and a persistent cross-run cache needs the stronger content-based signal instead.
    [[nodiscard]] u64 compute_shader_cache_key(
        string_view module_name,
        string_view source_text,
        string_view variant_canonical,
        const ShaderCompileOptions &options) noexcept;

    // Reads `directory/<key as hex>.sc`. Returns nullopt on any I/O problem, corrupt data, or a
    // version mismatch from an older cache-format build — every failure mode is treated as a cache
    // miss (fall back to compiling normally), never as an error a caller needs to handle specially.
    [[nodiscard]] optional<ShaderCacheEntry> load_shader_cache_entry(
        const std::filesystem::path &directory, u64 key);

    // Writes `entry` to `directory/<key as hex>.sc`, creating `directory` if needed. Returns false on
    // any I/O failure — callers should treat that as "the compile still succeeded, just isn't cached
    // this time," not a hard error.
    bool store_shader_cache_entry(
        const std::filesystem::path &directory, u64 key, const ShaderCacheEntry &entry);

    // Reflection-only counterpart of the pair above, for Core::Slang::discover_shaders() (which only
    // ever reflects, never compiles to bytecode — see ShaderDiscovery.cpp). Uses a distinct file suffix
    // (`.sr` vs `.sc`) and magic, so a reflection entry and a compiled-bytecode entry for the same
    // shader never collide on disk even if their keys happened to coincide. Same miss-is-never-an-error,
    // write-failure-is-silent contract as the pair above.
    [[nodiscard]] optional<ShaderReflection> load_shader_reflection_cache_entry(
        const std::filesystem::path &directory, u64 key);
    bool store_shader_reflection_cache_entry(
        const std::filesystem::path &directory, u64 key, const ShaderReflection &reflection);

} // namespace SFT::Core::Slang
