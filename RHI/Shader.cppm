module;
#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <span>
#include <type_traits>
#pragma endregion

export module Sturdy.RHI:Shader;

import :Flags;
import :Handles;

using std::span;

export namespace SFT::RHI {

    // Which pipeline stage(s) a shader module, a binding's visibility, or a push-constant range
    // applies to. A bitmask because bindings/push-constants are routinely visible to more than one
    // stage at once (e.g. `Vertex | Fragment`). See :Flags for the operator set.
    enum class ShaderStage : u32 {
        None = 0,
        Vertex = 1u << 0,
        Fragment = 1u << 1,
        Compute = 1u << 2,
        Geometry = 1u << 3,
        TessControl = 1u << 4,
        TessEval = 1u << 5,
        Task = 1u << 6,
        Mesh = 1u << 7,
        RayGeneration = 1u << 8,
        AnyHit = 1u << 9,
        ClosestHit = 1u << 10,
        Miss = 1u << 11,
        Intersection = 1u << 12,
        Callable = 1u << 13,

        AllGraphics = Vertex | Fragment | Geometry | TessControl | TessEval | Task | Mesh,
        AllRayTracing = RayGeneration | AnyHit | ClosestHit | Miss | Intersection | Callable,
        All = AllGraphics | Compute | AllRayTracing,
    };

    // The intermediate representation a shader module's bytecode is in. The RHI carries opaque
    // bytecode + a language tag rather than source: compilation from Slang/HLSL/GLSL happens above
    // the RHI (see Core/Slang), and each backend consumes the IR its API accepts (Vulkan: SPIR-V).
    enum class ShaderLanguage : u32 {
        SpirV, // Vulkan
        Dxil,  // D3D12
        Msl,   // Metal (source or AIR)
        Wgsl,  // WebGPU (source)
    };

    struct ShaderModuleDesc {
        ShaderLanguage language = ShaderLanguage::SpirV;
        // Opaque module bytecode/source. Non-owning: the backend copies whatever it needs during
        // create_shader_module(), so the span's storage need only outlive that call.
        span<const std::byte> code;
        const char *label = nullptr;
    };

    // Names one entry point within a compiled module for a pipeline stage — a module can hold
    // several (a Slang file often compiles vertexMain + fragmentMain into one module).
    struct ShaderEntry {
        ShaderModuleHandle module{};
        // Entry-point name (e.g. "vertexMain"). Non-owning; must outlive the create-pipeline call.
        const char *entry_point = "main";
        // Explicit stage intent for APIs/pipeline types that cannot infer it safely (notably ray-tracing
        // general groups, where a general entry may be raygen, miss, or callable). Raster/compute
        // pipeline descriptors still imply the stage from the field they place this entry in, but callers
        // may set this for clarity.
        ShaderStage stage = ShaderStage::None;
    };

    // ─── Vertex input layout ─────────────────────────────────────────────────────

    // The scalar/vector layout of one vertex attribute as fed to the input assembler. A focused set
    // covering the usual position/normal/uv/color/tangent and skinning-index cases; extend when a
    // real vertex format needs something absent here.
    enum class VertexFormat : u32 {
        Float32,
        Float32x2,
        Float32x3,
        Float32x4,
        Uint32,
        Uint32x2,
        Uint32x3,
        Uint32x4,
        Sint32,
        Sint32x2,
        Sint32x3,
        Sint32x4,
        Uint8x4Unorm, // packed color, etc.
        Sint8x4Norm,
        Uint16x2Unorm,
        Uint16x4Unorm,
        Float16x2,
        Float16x4,
    };

    // Advance the bound vertex buffer per-vertex (normal geometry) or per-instance (instanced draws).
    enum class VertexStepMode : u32 {
        Vertex,
        Instance,
    };

    // One attribute pulled from a vertex buffer: `shader_location` matches the shader's input slot,
    // `offset` is its byte offset within the buffer's per-element stride.
    struct VertexAttribute {
        VertexFormat format = VertexFormat::Float32x3;
        u32 offset = 0;
        u32 shader_location = 0;
    };

    // One bound vertex buffer's layout: its element `stride`, whether it steps per-vertex or
    // per-instance, and the attributes read out of it. `attributes` is non-owning (see the pipeline
    // descriptor that carries these — :Pipeline).
    struct VertexBufferLayout {
        u64 stride = 0;
        VertexStepMode step_mode = VertexStepMode::Vertex;
        span<const VertexAttribute> attributes;
    };

    template <>
    struct enable_flag_ops<ShaderStage> : std::true_type {};

} // namespace SFT::RHI
