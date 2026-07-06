module;

#pragma region Imports
#include <string>
#include <vector>
#pragma endregion

export module Sturdy.Core:ShaderTypes;

import Sturdy.Foundation;

using std::string;
using std::vector;

export namespace SFT::Core::Slang {

    // The **output language** a shader is compiled to. Slang is a cross-compiler, so one `.slang`
    // source can be lowered to any of these; this engine targets `Spirv` for its Vulkan backend.
    //
    // - `Spirv` — Vulkan / OpenGL (the engine default).
    // - `Dxil` / `Hlsl` — Direct3D 12 / 11.
    // - `Glsl` — OpenGL / Vulkan (text).
    // - `Metal` — Apple.
    // - `Wgsl` — WebGPU.
    enum class ShaderTargetFormat {
        Spirv,
        Dxil,
        Hlsl,
        Glsl,
        Metal,
        Wgsl,
    };

    // The **pipeline stage** an entry point runs at. `Unknown` means "not yet determined" (e.g. before
    // reflection fills it in). Covers the classic raster stages, compute, the full ray-tracing set, and
    // the mesh-shading pipeline (`Mesh` / `Amplification`).
    enum class ShaderStage {
        Unknown,
        Vertex,
        Hull,
        Domain,
        Geometry,
        Fragment,
        Compute,
        RayGeneration,
        Intersection,
        AnyHit,
        ClosestHit,
        Miss,
        Callable,
        Mesh,
        Amplification,
        Dispatch,
    };

    // How hard the Slang back-end works on the generated code. Higher levels trade compile time (and
    // sometimes code size) for runtime speed.
    enum class ShaderOptimizationLevel {
        None,    // Don't optimize at all.
        Default, // Balance code quality and compilation time.
        High,    // Optimize aggressively.
        Maximal, // Include optimizations that may take a very long time / severe space-speed tradeoffs.
    };

    // A `#define` fed to the Slang preprocessor before compilation, e.g. `{"MAX_LIGHTS", "8"}`.
    struct ShaderMacro {
        string name;
        string value;
    };

    // One requested output: a `ShaderTargetFormat` plus the profile/feature level string Slang compiles
    // against (e.g. `"spirv_1_5"`, `"spirv_1_6"`, `"sm_6_6"`). A `ShaderCompileOptions` may list several.
    struct ShaderTarget {
        ShaderTargetFormat format = ShaderTargetFormat::Spirv;
        string profile = "spirv_1_5";
    };

    // Names an entry point to compile and, optionally, the stage it should be treated as. Leave `stage`
    // as `Unknown` to let Slang infer it from the source's `[shader("...")]` attribute.
    struct ShaderEntryPointRequest {
        string name;
        ShaderStage stage = ShaderStage::Unknown;
    };

    // Everything that shapes a `ShaderCompiler::compile()` / `reflect()` call. All fields are optional —
    // the defaults compile every `[shader]`-annotated entry point to SPIR-V with balanced optimization.
    //
    // - `targets`       — one or more output formats/profiles (defaults to a single SPIR-V target).
    // - `entry_points`  — restrict to specific entry points; empty means "all annotated ones."
    // - `search_paths`  — extra directories for `#include` / `import` resolution.
    // - `macros`        — preprocessor defines.
    // - `optimization`  — back-end effort (see `ShaderOptimizationLevel`).
    // - `allow_glsl_syntax` / `skip_spirv_validation` / `enable_effect_annotations` — niche toggles.
    //
    // ```cpp
    // ShaderCompileOptions opts{
    //     .targets      = {{ .format = ShaderTargetFormat::Spirv, .profile = "spirv_1_6" }},
    //     .macros       = {{ "MAX_LIGHTS", "8" }},
    //     .optimization = ShaderOptimizationLevel::High,
    // };
    // auto shader = compiler.compile(ShaderSource::from_file("Shaders/pbr.slang"), opts);
    // ```
    struct ShaderCompileOptions {
        vector<ShaderTarget> targets = {ShaderTarget{}};
        vector<ShaderEntryPointRequest> entry_points;
        vector<string> search_paths;
        vector<ShaderMacro> macros;
        ShaderOptimizationLevel optimization = ShaderOptimizationLevel::Default;
        b8 allow_glsl_syntax = false;
        b8 skip_spirv_validation = false;
        b8 enable_effect_annotations = false;
    };

    // Compiled output for **one entry point** at **one target**: the raw `bytes` (SPIR-V words, DXIL,
    // ...) plus the `target`/`profile` they were produced for and the `entry_point` name they belong to.
    // Hand `bytes` straight to the graphics API (for Vulkan: `VkShaderModuleCreateInfo::pCode`).
    struct ShaderBytecode {
        ShaderTargetFormat target = ShaderTargetFormat::Spirv;
        string profile;
        string entry_point;
        vector<byte> bytes;
    };

} // namespace SFT::Core::Slang
