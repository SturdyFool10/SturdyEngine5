module;
#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <concepts>
#include <string>
#include <string_view>
#pragma endregion

export module Sturdy.Core:ShaderSource;


using std::convertible_to;
using std::string;
using std::string_view;

export namespace SFT::Core::Slang {

    // Where a `ShaderSource`'s code comes from: an in-memory `SourceString` already in hand, or a `File`
    // the compiler reads from disk by `path`.
    enum class ShaderSourceKind {
        SourceString,
        File,
    };

    // The input handed to `ShaderCompiler::compile()` / `reflect()`. Prefer the `from_source` /
    // `from_file` factories over filling the fields by hand — they set `kind` consistently.
    //
    // - `module_name` — Slang module name; also the stable identity a backend keys the shader by.
    // - `path`        — file path (required for `File`, optional provenance for `SourceString`).
    // - `source`      — the code itself (only for `SourceString`; empty for `File`, loaded on compile).
    //
    // ```cpp
    // auto from_disk = ShaderSource::from_file("Shaders/triangle.slang");   // module name = "triangle"
    // auto inline_hlsl = ShaderSource::from_source("gen", generated_slang_text);
    // ```
    struct ShaderSource {
        ShaderSourceKind kind = ShaderSourceKind::SourceString;
        string module_name;
        string path;
        string source;

        // In-memory shader from a `source` string. `module_name` is the Slang module name; `path` is
        // optional provenance used only for diagnostics. Sets `kind = SourceString`.
        [[nodiscard]] static ShaderSource from_source(string module_name, string source, string path = {});

        // File-based shader: the compiler reads `path` from disk at compile time (`source` stays empty).
        // `module_name` defaults to empty — callers typically pass the file stem so the module keeps one
        // stable name end to end. Sets `kind = File`.
        [[nodiscard]] static ShaderSource from_file(string path, string module_name = {});
    };

    // Satisfied by any type exposing `static constexpr` `module_name` and `source` string views (and,
    // optionally, `path`). Lets a shader be **embedded as a type** and compiled with zero runtime file
    // I/O — pair it with a build step that generates such a type from a `.slang` file, or the
    // `Foundation::Embed` machinery.
    //
    // ```cpp
    // struct TriangleShader {
    //     static constexpr std::string_view module_name = "triangle";
    //     static constexpr std::string_view source      = "/* ...slang... */";
    // };
    // static_assert(StaticShaderSource<TriangleShader>);
    // auto shader = compiler.compile<TriangleShader>();   // see ShaderCompiler
    // ```
    template <typename StaticShader>
    concept StaticShaderSource = requires {
        { StaticShader::module_name } -> convertible_to<string_view>;
        { StaticShader::source } -> convertible_to<string_view>;
    };

    // Materialize a runtime `ShaderSource` from a compile-time `StaticShaderSource` type, copying its
    // `module_name` / `source` (and `path`, if present) into owned strings. Usually reached indirectly
    // through `ShaderCompiler::compile<StaticShader>()` rather than called by hand.
    template <StaticShaderSource StaticShader>
    [[nodiscard]] ShaderSource shader_source_from_type() {
        ShaderSource source{};
        source.kind = ShaderSourceKind::SourceString;
        source.module_name = string{string_view{StaticShader::module_name}};
        source.source = string{string_view{StaticShader::source}};

        if constexpr (requires { { StaticShader::path } -> convertible_to<string_view>; }) {
            source.path = string{string_view{StaticShader::path}};
        }

        return source;
    }

} // namespace SFT::Core::Slang
