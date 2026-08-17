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


    inline constexpr string_view default_shader_cache_directory = "Shaders/.cache";


    struct ShaderCacheTargetArtifact {
        ShaderTarget target;
        ShaderReflection reflection;
        vector<ShaderBytecode> bytecode;
    };


    struct ShaderCacheEntry {
        string module_name;
        vector<ShaderCacheTargetArtifact> artifacts;
    };


    /// Computes shader cache key using the supplied arguments and current state.
    ///
    /// @param module_name Name used to identify or label the target.
    /// @param source_text `source_text` value used by the operation.
    /// @param variant_canonical `variant_canonical` value used by the operation.
    /// @param options Configuration values controlling the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] u64 compute_shader_cache_key(
        string_view module_name,
        string_view source_text,
        string_view variant_canonical,
        const ShaderCompileOptions &options) noexcept;


    /// Loads shader cache entry.
    ///
    /// @param directory `directory` value used by the operation.
    /// @param key Key used to identify the requested entry.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    [[nodiscard]] optional<ShaderCacheEntry> load_shader_cache_entry(
        const std::filesystem::path &directory, u64 key);


    /// Performs the store shader cache entry operation using the supplied arguments.
    ///
    /// @param directory `directory` value used by the operation.
    /// @param key Key used to identify the requested entry.
    /// @param entry `entry` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool store_shader_cache_entry(
        const std::filesystem::path &directory, u64 key, const ShaderCacheEntry &entry);


    /// Reports whether shader cache entry is fresh.
    ///
    /// @param directory `directory` value used by the operation.
    /// @param key Key used to identify the requested entry.
    /// @param shader_source_path Filesystem path identifying the target resource.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool shader_cache_entry_is_fresh(
        const std::filesystem::path &directory,
        u64 key,
        const std::filesystem::path &shader_source_path) noexcept;


    /// Loads shader reflection cache entry.
    ///
    /// @param directory `directory` value used by the operation.
    /// @param key Key used to identify the requested entry.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    [[nodiscard]] optional<ShaderReflection> load_shader_reflection_cache_entry(
        const std::filesystem::path &directory, u64 key);
    /// Performs the store shader reflection cache entry operation for `Slang` using the supplied arguments.
    ///
    /// @param directory `directory` value used by the operation.
    /// @param key Key used to identify the requested entry.
    /// @param reflection `reflection` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool store_shader_reflection_cache_entry(
        const std::filesystem::path &directory, u64 key, const ShaderReflection &reflection);

} // namespace SFT::Core::Slang
