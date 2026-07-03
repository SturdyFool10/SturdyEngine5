module;

#include <memory>
#include <string_view>

export module Sturdy.Core:Shader;

import Sturdy.Foundation;

// Re-export all Slang sub-partitions so callers that import :Shader or Sturdy.Core get
// the full shader API without importing each partition individually.
export import :ShaderError;
export import :ShaderSource;
export import :ShaderTypes;
export import :ShaderReflection;

using std::shared_ptr;
using std::string_view;

export namespace SFT::Core::Slang {

    // Sentinel `size` for an **unbounded** array/resource in reflection (e.g. a bindless `Texture2D t[]`)
    // — the largest `u64`. Compare a reflected `size` against this to detect runtime-sized arrays.
    inline constexpr u64 shader_unbounded_size = ~u64{0};

    // Sentinel `size` for a resource whose byte size Slang could **not determine** (opaque/dependent
    // layout). One less than `shader_unbounded_size`, so the two never collide.
    inline constexpr u64 shader_unknown_size = shader_unbounded_size - 1;

    struct ShaderCompilerState;
    struct ShaderState;

    // A **compiled** Slang shader: its reflected layout plus a linked program you can pull target
    // bytecode out of, one entry point at a time. Produced by `ShaderCompiler::compile()`.
    //
    // Cheap to copy and move — the heavy Slang state lives behind a `shared_ptr`, so copies share it.
    // A default-constructed `Shader` is **empty** (`operator bool` is `false`); every accessor is safe
    // on an empty shader (returns empty/error rather than crashing).
    //
    // ```cpp
    // ShaderCompiler compiler;
    // auto shader = compiler.compile(ShaderSource::from_file("Shaders/triangle.slang"));
    // if (!shader) {
    //     log_error("{}\n{}", shader.error().message, shader.error().diagnostics);
    //     return;
    // }
    // // Pull SPIR-V for one entry point and hand it to Vulkan:
    // auto vs = shader->entry_point_code("vertexMain");
    // if (vs) create_shader_module(vs->bytes);
    // ```
    class Shader {
      public:
        // Constructs an **empty** shader (`operator bool` == `false`). Fill it via `ShaderCompiler`.
        Shader() = default;
        ~Shader();

        // Copyable and movable; copies share the same underlying compiled program (`shared_ptr`).
        Shader(const Shader &) = default;
        Shader &operator=(const Shader &) = default;
        Shader(Shader &&) noexcept = default;
        Shader &operator=(Shader &&) noexcept = default;

        // Whether this shader holds a compiled program. `false` for a default-constructed `Shader`.
        [[nodiscard]] explicit operator bool() const noexcept;

        // The shader's reflected layout (entry points, parameters, bindings, descriptor sets). Returns a
        // shared empty `ShaderReflection` when the shader is empty, so it is always safe to call.
        [[nodiscard]] const ShaderReflection &reflection() const noexcept;

        // The Slang module name this shader was compiled under (empty for an empty shader).
        [[nodiscard]] string_view module_name() const noexcept;

        // Generate (or fetch) the compiled bytecode for one entry point at one target.
        //
        // @param entry_point_index index into `reflection().entry_points`.
        // @param target_index      index into the `ShaderCompileOptions::targets` used at compile time.
        // @returns the `ShaderBytecode`, or a `ShaderError` — `OperationFailed` (empty shader),
        //          `InvalidArgument` (index out of range), or `CodeGenerationFailed`.
        [[nodiscard]] ShaderExpected<ShaderBytecode> entry_point_code(usize entry_point_index, usize target_index = 0) const;

        // Same as above, but looks the entry point up **by name** — matching either its reflected `name`
        // or its `name_override`. Returns `EntryPointNotFound` if no entry point matches.
        //
        // ```cpp
        // auto fs = shader->entry_point_code("fragmentMain");
        // ```
        [[nodiscard]] ShaderExpected<ShaderBytecode> entry_point_code(string_view entry_point_name, usize target_index = 0) const;

      private:
        friend class ShaderCompiler;
        explicit Shader(shared_ptr<ShaderState> state) noexcept;

        shared_ptr<ShaderState> state_;
    };

    // Owns the Slang global session and turns `ShaderSource` into compiled `Shader`s (or reflection
    // only). Cheap to copy/move (state is `shared_ptr`-held) and **thread-safe** — `compile()` and
    // `reflect()` serialize on an internal mutex, and the global session is created lazily on first use,
    // so one compiler can be shared across threads.
    //
    // ```cpp
    // ShaderCompiler compiler;
    // auto shader = compiler.compile(ShaderSource::from_file("Shaders/pbr.slang"),
    //                                ShaderCompileOptions{ .optimization = ShaderOptimizationLevel::High });
    // ```
    class ShaderCompiler {
      public:
        // Constructs the compiler. The underlying Slang global session is created lazily on the first
        // `compile()` / `reflect()`, so construction itself is cheap and can't fail here.
        ShaderCompiler();
        ~ShaderCompiler();

        // Copyable and movable; copies share the same Slang session.
        ShaderCompiler(const ShaderCompiler &) = default;
        ShaderCompiler &operator=(const ShaderCompiler &) = default;
        ShaderCompiler(ShaderCompiler &&) noexcept = default;
        ShaderCompiler &operator=(ShaderCompiler &&) noexcept = default;

        // Compile `source` into a linked, reflectable `Shader` per `options` (see `ShaderCompileOptions`).
        // On failure the `ShaderError` carries the Slang `diagnostics` — surface them, not just the
        // message. Thread-safe.
        [[nodiscard]] ShaderExpected<Shader> compile(const ShaderSource &source, const ShaderCompileOptions &options = {});

        // Compile a shader **embedded as a type** (see `StaticShaderSource`) with no runtime file I/O.
        //
        // ```cpp
        // auto shader = compiler.compile<TriangleShader>();
        // ```
        template <StaticShaderSource StaticShader>
        [[nodiscard]] ShaderExpected<Shader> compile(const ShaderCompileOptions &options = {}) {
            return compile(shader_source_from_type<StaticShader>(), options);
        }

        // Reflection only: parse the source into a module and read its layout, without composing
        // entry points, linking, or generating any target code. Much lighter than compile() — use
        // it to inventory shaders (bindings, entry points, parameters) up front, then compile() the
        // ones a backend actually needs.
        //
        // ```cpp
        // auto refl = compiler.reflect(ShaderSource::from_file("Shaders/pbr.slang"));
        // if (refl) for (auto &ep : refl->entry_points) log_info("entry point: {}", ep.name);
        // ```
        [[nodiscard]] ShaderExpected<ShaderReflection> reflect(const ShaderSource &source, const ShaderCompileOptions &options = {});

      private:
        shared_ptr<ShaderCompilerState> state_;
    };

} // namespace SFT::Core::Slang
