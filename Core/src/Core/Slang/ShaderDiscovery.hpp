#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#pragma endregion

#include <Core/Slang/Shader.hpp>
#include <Core/Slang/ShaderError.hpp>
#include <Core/Slang/ShaderSource.hpp>
#include <Core/Slang/ShaderTypes.hpp>
#include <Core/Slang/ShaderReflection.hpp>

using std::error_code;
using std::string;
using std::string_view;
using std::vector;

namespace fs = std::filesystem;

namespace SFT::Core::Slang {

    // File extension `discover_shaders()` scans for. Slang source files end in `.slang`.
    inline constexpr string_view shader_file_extension = ".slang";

    // A shader that Slang has parsed and **reflected but not yet compiled** to any target's bytecode.
    //
    // It holds only `source` and `reflection` — no session, module, or linked program — so discovering
    // and reflecting every shader on disk at startup, *before a graphics backend even exists*, costs no
    // GPU or codegen work and leaves nothing Slang-specific alive. Actually generating bytecode for an
    // entry point means recompiling `source` later through `ShaderCompiler::compile()`.
    struct UnCompiledShader {
        ShaderSource source;
        ShaderReflection reflection;

        // Convenience accessor for `source.module_name` — the shader's stable identity.
        [[nodiscard]] string_view module_name() const noexcept;
    };

    // Recursively scan `directory` for `*.slang` files and reflect each one into an `UnCompiledShader`.
    //
    // Resilient by design: a file that fails to read or reflect is **logged and skipped**, not fatal —
    // one broken shader shouldn't stop the engine from starting. A missing directory logs a warning and
    // yields an empty list. Each shader's module name is set to the file stem, so it keeps one stable
    // name from discovery through later compilation.
    //
    // @param directory root to walk (recursively).
    // @param compiler  used for the reflection pass (`ShaderCompiler::reflect`).
    // @param options   reflection options (search paths, macros, ...); defaults are usually fine.
    // @returns every successfully reflected shader found, in traversal order.
    //
    // ```cpp
    // ShaderCompiler compiler;
    // auto shaders = discover_shaders("Shaders", compiler);
    // for (const auto &s : shaders)
    //     log_info("found shader '{}' with {} entry point(s)",
    //              s.module_name(), s.reflection.entry_points.size());
    // ```
    [[nodiscard]] vector<UnCompiledShader> discover_shaders(const fs::path &directory,
                                                                   ShaderCompiler &compiler,
                                                                   const ShaderCompileOptions &options = {});

} // namespace SFT::Core::Slang
