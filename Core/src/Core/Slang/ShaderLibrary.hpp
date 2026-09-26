#pragma once

#include <Foundation/Foundation.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace SFT::Core::Slang {

    /// Process-wide overrides for the engine's built-in shader modules.
    ///
    /// Built-in shaders `import sturdy_common;` (and friends) and the renderer loads its own effect shaders
    /// (`fullscreen_tonemap`, ...) by file name. An override registered here under a module name (the file
    /// stem, e.g. "sturdy_common") is served in place of the shipped file for every subsequent compile, both
    /// as an import and as a top-level file, so an application whose space is not Euclidean, or whose
    /// lighting is its own, can swap one library module without forking the rest of the shader set.
    ///
    /// Overrides take effect for shaders compiled after the call; the shader disk cache key includes them, so
    /// a stale cached result is never served. Names are matched case-sensitively on the file stem; a
    /// trailing ".slang" in `name` is ignored.
    void override_shader_module(std::string_view name, std::string source);

    /// Removes one override. Returns false when none was registered under that name.
    bool remove_shader_module_override(std::string_view name);

    void clear_shader_module_overrides();

    [[nodiscard]] std::optional<std::string> find_shader_module_override(std::string_view name);

    /// Same lookup by file path (uses the path's stem).
    [[nodiscard]] std::optional<std::string> find_shader_module_override_for_path(std::string_view path);

    /// Hash of every registered override (0 when there are none); mixed into shader cache keys.
    [[nodiscard]] u64 shader_override_fingerprint();

} // namespace SFT::Core::Slang
