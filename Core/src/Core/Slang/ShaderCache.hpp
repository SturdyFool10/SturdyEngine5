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

    // One target-specific artifact for a shader variant. Reflection is kept beside its target's
    // bytecode because Slang's lowered layout can differ by output API (notably push/root constants).
    // `bytecode` contains one entry for each entry point, in reflection entry-point order.
    struct ShaderCacheTargetArtifact {
        ShaderTarget target;
        ShaderReflection reflection;
        vector<ShaderBytecode> bytecode;
    };

    // Everything persisted for one source + define variant. A single cache record accumulates target
    // artifacts over time, so switching Vulkan <-> D3D12 can reuse both output formats without either
    // API replacing the other's reflection snapshot or bytecode.
    struct ShaderCacheEntry {
        string module_name;
        vector<ShaderCacheTargetArtifact> artifacts;
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

    // Fast filesystem freshness gate layered on top of the content/options key. For a file-backed
    // shader, a cache artifact is usable only when its last-write timestamp is at least as new as the
    // shader source's. Any unavailable timestamp is conservatively a miss; callers with embedded or
    // in-memory sources should skip this gate and rely on compute_shader_cache_key() instead.
    [[nodiscard]] bool shader_cache_entry_is_fresh(
        const std::filesystem::path &directory,
        u64 key,
        const std::filesystem::path &shader_source_path) noexcept;

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
