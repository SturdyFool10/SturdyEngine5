#pragma once

#include <filesystem>
#include <optional>

namespace SFT::Foundation {

    /// Where the engine's on-disk caches (compressed textures, shader bytecode) live.
    ///
    /// The historical defaults are relative to the working directory (`./.cache/...`,
    /// `./Shaders/.cache`), which dirties a source checkout whenever any binary runs from it. A host
    /// that wants them elsewhere calls `set_cache_root` before the first cache access, or sets the
    /// `STURDY_CACHE_DIR` environment variable. With neither, `cache_root()` is empty and each cache
    /// keeps its historical location.

    /// Overrides the cache root for this process. An empty path clears the override.
    ///
    /// @note Thread-safe; takes effect for cache accesses made afterwards.
    void set_cache_root(std::filesystem::path root);

    /// Returns the configured cache root: the `set_cache_root` override, else `STURDY_CACHE_DIR`,
    /// else `std::nullopt`.
    [[nodiscard]] std::optional<std::filesystem::path> cache_root();

    /// Resolves a cache subdirectory: `<cache_root>/<name>` when a root is configured, otherwise
    /// `legacy_default` unchanged.
    [[nodiscard]] std::filesystem::path cache_subdirectory(const char *name, const std::filesystem::path &legacy_default);

} // namespace SFT::Foundation
