#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <array>
#include <memory>
#include <string>
#include <vector>
#pragma endregion

#include <Core/Slang/ShaderTypes.hpp>

using std::array;
using std::shared_ptr;
using std::string;
using std::vector;

namespace SFT::Core::Slang {

    // ─────────────────────────────────────────────────────────────────────────────────────────────
    //  Shader reflection: a backend-agnostic snapshot of a shader's layout.
    //
    //  `ShaderReflection` is the root. It is produced by `ShaderCompiler::reflect()` (cheap, no codegen)
    //  or read off a compiled `Shader` via `Shader::reflection()`, and it describes everything a
    //  renderer needs to wire the shader up without touching Slang types: the entry points and their
    //  stages, every parameter and its binding, the descriptor-set layout, and the full type tree.
    //
    //  Rough shape:
    //      ShaderReflection
    //        ├─ global_parameters : ShaderParameterReflection[]   (module-scope uniforms/resources)
    //        ├─ entry_points      : ShaderEntryPointReflection[]  (each with its own parameters)
    //        ├─ descriptor_sets   : ShaderDescriptorSetReflection[]
    //        └─ ...                                               (types hang off parameters as a tree)
    //
    //  ```cpp
    //  auto refl = compiler.reflect(ShaderSource::from_file("Shaders/pbr.slang"));
    //  if (!refl) return refl.error();
    //  for (const auto &ep : refl->entry_points) {
    //      log_info("entry point '{}' ({})", ep.name, static_cast<int>(ep.stage));
    //      for (const auto &p : ep.parameters)
    //          log_info("  param '{}' @ set {} binding {}", p.name, p.binding_space, p.binding);
    //  }
    //  ```
    // ─────────────────────────────────────────────────────────────────────────────────────────────

    // --- Type system enums ---

    // The broad **category** of a reflected type — mirrors Slang's `TypeReflection::Kind`. Tells you how
    // to interpret a `ShaderTypeReflection`: aggregate (`Struct`), `Array`/`Matrix`/`Vector`/`Scalar`,
    // one of the resource/buffer kinds, or a more exotic construct (`ParameterBlock`, `Interface`, ...).
    enum class ShaderTypeKind {
        Unknown,
        Struct,
        Array,
        Matrix,
        Vector,
        Scalar,
        ConstantBuffer,
        Resource,
        SamplerState,
        TextureBuffer,
        ShaderStorageBuffer,
        ParameterBlock,
        GenericTypeParameter,
        Interface,
        OutputStream,
        MeshOutput,
        Specialized,
        Feedback,
        Pointer,
        DynamicResource,
        Enum,
    };

    // The **element scalar type** of a scalar/vector/matrix — the leaf numeric type. Includes the small
    // floats used by ML/tensor work (`Float16`, `BFloat16`, `FloatE4M3`, `FloatE5M2`). `None` for types
    // that aren't scalar-based.
    enum class ShaderScalarType {
        None,
        Void,
        Bool,
        Int32,
        UInt32,
        Int64,
        UInt64,
        Float16,
        Float32,
        Float64,
        Int8,
        UInt8,
        Int16,
        UInt16,
        IntPtr,
        UIntPtr,
        BFloat16,
        FloatE4M3,
        FloatE5M2,
    };

    // Which **resource space** a parameter consumes — Slang's `ParameterCategory`. This is how a binding
    // maps onto an API: `ConstantBuffer`, `ShaderResource` (SRV/textures), `UnorderedAccess` (UAV),
    // `SamplerState`, `VaryingInput`/`VaryingOutput` (stage I/O), `PushConstantBuffer`, the ray-tracing
    // payload spaces, and so on. `Mixed` means a parameter straddles several categories.
    enum class ShaderParameterCategory {
        None,
        Mixed,
        ConstantBuffer,
        ShaderResource,
        UnorderedAccess,
        VaryingInput,
        VaryingOutput,
        SamplerState,
        Uniform,
        DescriptorTableSlot,
        SpecializationConstant,
        PushConstantBuffer,
        RegisterSpace,
        Generic,
        RayPayload,
        HitAttributes,
        CallablePayload,
        ShaderRecord,
        ExistentialTypeParam,
        ExistentialObjectParam,
        SubElementRegisterSpace,
        Subpass,
        MetalArgumentBufferElement,
        MetalAttribute,
        MetalPayload,
    };

    // The **kind of descriptor** a binding range occupies — the abstract equivalent of a Vulkan
    // `VkDescriptorType`. Distinguishes samplers, textures, the buffer flavors (`TypedBuffer`,
    // `RawBuffer`, their `Mutable*` read-write variants), acceleration structures, push constants, etc.
    enum class ShaderBindingType {
        Unknown,
        Sampler,
        Texture,
        ConstantBuffer,
        ParameterBlock,
        TypedBuffer,
        RawBuffer,
        CombinedTextureSampler,
        InputRenderTarget,
        InlineUniformData,
        RayTracingAccelerationStructure,
        VaryingInput,
        VaryingOutput,
        ExistentialValue,
        PushConstant,
        MutableTexture,
        MutableTypedBuffer,
        MutableRawBuffer,
    };

    // The **dimensionality/shape** of a resource — 1D/2D/3D/cube textures plus the buffer shapes
    // (`StructuredBuffer`, `ByteAddressBuffer`, `TextureBuffer`) and `AccelerationStructure`.
    enum class ShaderResourceShape {
        Unknown,
        Texture1D,
        Texture2D,
        Texture3D,
        TextureCube,
        TextureBuffer,
        StructuredBuffer,
        ByteAddressBuffer,
        AccelerationStructure,
        TextureSubpass,
    };

    // How a shader may **access** a resource: read-only (`Read`), read-write (`ReadWrite`/`Write`),
    // append/consume structured buffers, `RasterOrdered` (ROV), or `Feedback` (sampler-feedback).
    enum class ShaderResourceAccess {
        None,
        Read,
        ReadWrite,
        RasterOrdered,
        Append,
        Consume,
        Write,
        Feedback,
        Unknown,
    };

    // Storage order of a matrix type — `RowMajor` vs `ColumnMajor`. Affects how uniform data must be
    // laid out on the CPU side before upload.
    enum class ShaderMatrixLayout {
        Unknown,
        RowMajor,
        ColumnMajor,
    };

    // --- Reflection structs ---

    struct ShaderTypeReflection;

    // One **field of a struct** type: its `name`, its `type` (a node in the type tree), and its byte
    // `offset` / `size` / `stride` within the containing aggregate. Use these to build CPU-side structs
    // that match a constant buffer's layout exactly.
    struct ShaderFieldReflection {
        string name;
        shared_ptr<ShaderTypeReflection> type;
        u64 offset = 0;
        u64 size = 0;
        u64 stride = 0;
    };

    // A contiguous **range of descriptors** a type contributes, in descriptor-set terms: what kind of
    // binding (`type`/`category`), which set and range, the base `binding`, and how many (`count`).
    // `count` may be `0` for a runtime-sized/bindless array. `image_format` is the raw API format enum
    // for typed images (`0` when not applicable); `specializable` marks a range gated on a spec constant.
    struct ShaderBindingRangeReflection {
        ShaderBindingType type = ShaderBindingType::Unknown;
        ShaderParameterCategory category = ShaderParameterCategory::None;
        u32 descriptor_set = 0;
        u32 descriptor_range_index = 0;
        u32 descriptor_range_count = 0;
        u32 binding = 0;
        u32 count = 0;
        u32 image_format = 0;
        b8 specializable = false;
    };

    // A node in the shader's **type tree**. Carries the type's names, its `kind` and (for scalars) its
    // `scalar_type`, resource shape/access/matrix-layout where relevant, the numeric dimensions
    // (`row_count`/`column_count` for matrices, `element_count` for arrays), byte `size`/`stride`/
    // `alignment`, and — for aggregates — its `fields`. `binding_ranges` lists the descriptors this type
    // occupies. A `size` of `shader_unbounded_size` / `shader_unknown_size` marks unbounded/opaque types.
    struct ShaderTypeReflection {
        string name;
        string full_name;
        ShaderTypeKind kind = ShaderTypeKind::Unknown;
        ShaderScalarType scalar_type = ShaderScalarType::None;
        ShaderResourceShape resource_shape = ShaderResourceShape::Unknown;
        ShaderResourceAccess resource_access = ShaderResourceAccess::Unknown;
        ShaderMatrixLayout matrix_layout = ShaderMatrixLayout::Unknown;
        u32 row_count = 0;
        u32 column_count = 0;
        u64 element_count = 0;
        u64 size = 0;
        u64 stride = 0;
        i32 alignment = 0;
        vector<ShaderFieldReflection> fields;
        vector<ShaderBindingRangeReflection> binding_ranges;
    };

    // A **shader parameter** — a global uniform/resource or an entry-point parameter. Ties a `name` and
    // `type` to where it lives: its `category`, `binding`/`binding_space` (set), and byte `offset`/
    // `size`/`stride` for uniform data. For stage I/O it also carries the HLSL-style `semantic_name` /
    // `semantic_index`. `categories` / `binding_ranges` enumerate every resource space a `Mixed`
    // parameter spans.
    struct ShaderParameterReflection {
        string name;
        shared_ptr<ShaderTypeReflection> type;
        ShaderParameterCategory category = ShaderParameterCategory::None;
        ShaderStage stage = ShaderStage::Unknown;
        u32 binding = 0;
        u32 binding_space = 0;
        u64 offset = 0;
        u64 size = 0;
        u64 stride = 0;
        string semantic_name;
        u32 semantic_index = 0;
        vector<ShaderParameterCategory> categories;
        vector<ShaderBindingRangeReflection> binding_ranges;
    };

    // One **descriptor range** within a set: the binding `type`/`category`, its base `binding`, and its
    // `count`. The building block of `ShaderDescriptorSetReflection`.
    struct ShaderDescriptorRangeReflection {
        ShaderBindingType type = ShaderBindingType::Unknown;
        ShaderParameterCategory category = ShaderParameterCategory::None;
        u32 binding = 0;
        u32 count = 0;
    };

    // A **descriptor set** (register `space`) and the `ranges` bound into it — the direct input for
    // building a `VkDescriptorSetLayout` (one per `space`).
    struct ShaderDescriptorSetReflection {
        u32 space = 0;
        vector<ShaderDescriptorRangeReflection> ranges;
    };

    // Reflection for a single **entry point**. Beyond `name`/`stage` it exposes what the pipeline needs:
    // the compute `compute_thread_group_size` (the `numthreads`/`[shader]` local size) and
    // `compute_wave_size`, whether the fragment stage runs at sample rate, whether Slang synthesized a
    // default constant buffer, and the entry point's `parameters` and `result_parameters` (its outputs).
    // `name_override` is the alternate name to match against (see `Shader::entry_point_code(name)`).
    struct ShaderEntryPointReflection {
        string name;
        string name_override;
        ShaderStage stage = ShaderStage::Unknown;
        array<u32, 3> compute_thread_group_size = {0, 0, 0};
        u32 compute_wave_size = 0;
        b8 uses_sample_rate_input = false;
        b8 has_default_constant_buffer = false;
        vector<ShaderParameterReflection> parameters;
        vector<ShaderParameterReflection> result_parameters;
    };

    // The **root** of a shader's reflected layout. Holds the module-scope `global_parameters`, every
    // `entry_points`, the derived `descriptor_sets` (ready to build layouts from), and the shader's
    // implicit global constant buffer (`global_constant_buffer_binding` / `..._size`).
    // `bindless_space_index` is the register space used for bindless resources, or `-1` if the shader
    // uses none. `hashed_strings` / `json` expose Slang's string table and raw JSON reflection dump for
    // tooling.
    struct ShaderReflection {
        vector<ShaderParameterReflection> global_parameters;
        vector<ShaderEntryPointReflection> entry_points;
        vector<ShaderDescriptorSetReflection> descriptor_sets;
        vector<string> hashed_strings;
        string json;
        u32 global_constant_buffer_binding = 0;
        u64 global_constant_buffer_size = 0;
        i32 bindless_space_index = -1;
    };

} // namespace SFT::Core::Slang
