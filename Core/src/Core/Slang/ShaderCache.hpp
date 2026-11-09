#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <expected>
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

    /// The shader cache directory in effect: `<cache root>/shaders` when a cache root is configured
    /// (see `Foundation::set_cache_root`), otherwise the working-directory-relative legacy default.
    [[nodiscard]] inline std::filesystem::path shader_cache_directory() {
        return Foundation::cache_subdirectory("shaders", std::filesystem::path{string{default_shader_cache_directory}});
    }


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


    /// Why a cache read or write did not work. A cache is an optimisation, so none of these fails a compile; they are reported so that "the
    /// shaders recompile on every launch" has a stated cause instead of being a mystery.
    enum class ShaderCacheCode {
        /// No entry under that key. The normal first-time case.
        Missing,
        /// An entry made by another format version. Normal after an engine update.
        VersionMismatch,
        /// The file exists but cannot be read.
        Unreadable,
        /// The file is larger than any entry the engine writes.
        TooLarge,
        /// The file ends before its contents do.
        Truncated,
        /// The file is not a cache entry at all.
        BadMagic,
        /// The contents do not parse (damaged, or written by a different build).
        Corrupt,
        /// The cache folder cannot be created.
        DirectoryFailed,
        /// The entry could not be written.
        WriteFailed,
        /// The written entry could not be moved into place.
        ReplaceFailed,
        /// An exception was raised while reading or writing.
        Exception,
    };

    struct ShaderCacheIssue {
        ShaderCacheCode code = ShaderCacheCode::Exception;
        /// What happened, with the file and the operating system's reason.
        string message;

        /// True for the outcomes that are routine (no entry yet, an older format): not worth a warning.
        [[nodiscard]] bool routine() const noexcept {
            return code == ShaderCacheCode::Missing || code == ShaderCacheCode::VersionMismatch;
        }
    };

    template <typename Value>
    using ShaderCacheResult = std::expected<Value, ShaderCacheIssue>;

    /// Reports a cache problem: a debug line for routine ones, a (throttled) warning for the rest. `operation` says what was being attempted
    /// ("loading", "storing"), `what` names the entry.
    void report_shader_cache_issue(const ShaderCacheIssue &issue, string_view operation, string_view what);

    /// Loads the cache entry for `key`. The error says why there is none; `Missing` and `VersionMismatch` are routine.
    [[nodiscard]] ShaderCacheResult<ShaderCacheEntry> load_shader_cache_entry(
        const std::filesystem::path &directory, u64 key);

    /// Stores `entry` under `key`; written beside and renamed into place, so a reader never sees half an entry. The error says what failed.
    [[nodiscard]] ShaderCacheResult<void> store_shader_cache_entry(
        const std::filesystem::path &directory, u64 key, const ShaderCacheEntry &entry);

    /// Reports whether the entry for `key` is at least as new as the shader source at `shader_source_path`.
    /// @note Does not throw. False when either time cannot be read.
    [[nodiscard]] bool shader_cache_entry_is_fresh(
        const std::filesystem::path &directory,
        u64 key,
        const std::filesystem::path &shader_source_path) noexcept;

    /// As `load_shader_cache_entry`, for a reflection-only entry (shader discovery).
    [[nodiscard]] ShaderCacheResult<ShaderReflection> load_shader_reflection_cache_entry(
        const std::filesystem::path &directory, u64 key);

    /// As `store_shader_cache_entry`, for a reflection-only entry.
    [[nodiscard]] ShaderCacheResult<void> store_shader_reflection_cache_entry(
        const std::filesystem::path &directory, u64 key, const ShaderReflection &reflection);

} // namespace SFT::Core::Slang
