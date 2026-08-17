#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <span>
#include <string_view>
#pragma endregion

namespace SFT::Core::Slang {

    /// One .slang file's source, embedded into the binary at compile time from everything under this
    /// engine's own Shaders/ tree (see cmake/SturdyShaders.cmake's sturdy_generate_shader_embeds()).
    /// This is what "bundles the shader code into the executable": a shipped build works even with no
    /// Shaders/ directory sitting next to it at all. Three code paths fall back to this table whenever
    /// the real file can't be found/read on disk -- disk always wins when present, so hot-reload
    /// (editing a .slang file on disk while the engine runs) keeps working exactly as before; this only
    /// kicks in for a file that is actually missing:
    ///   - ShaderDiscovery.cpp's discover_shaders(), for the initial bulk scan/reflection pass.
    ///   - ShaderImpl.cpp's read_text_file(), for a File-kind ShaderSource's top-level source (the path
    ///     hot-reloadable material templates recompile from -- see Renderer::reload_material_template).
    ///   - ShaderImpl.cpp's EmbeddedFallbackFileSystem, wired into every Slang session via
    ///     SessionDesc::fileSystem, for `import`/`#include`-resolved files (every shader that imports
    ///     e.g. sturdy_common.slang needs this, not just the top-level file).
    struct EmbeddedShaderSource {
        std::string_view relative_path;
        std::string_view module_name;
        Foundation::EmbeddedText source;
    };

    /// Every .slang file embedded at compile time. Regenerated at configure time from whatever
    /// actually exists under Shaders/, so it always matches this build's real shader tree.
    [[nodiscard]] std::span<const EmbeddedShaderSource> embedded_shaders() noexcept;

    /// Looks up an embedded shader by relative path (as passed to Slang's file system hook, which may
    /// arrive as a bare filename, a search-path-combined path, or anything in between depending on how
    /// the caller structured search paths) or by bare module name (e.g. discover_shaders()'s file-stem
    /// convention). Matching is by filename only -- this engine's whole Shaders/ tree is flat, no
    /// subdirectories (see sturdy_generate_shader_embeds()'s own doc comment), so a suffix/filename
    /// match is unambiguous. Returns nullptr if nothing was embedded under that name.
    [[nodiscard]] const EmbeddedShaderSource *find_embedded_shader(std::string_view path_or_module_name) noexcept;

} // namespace SFT::Core::Slang
