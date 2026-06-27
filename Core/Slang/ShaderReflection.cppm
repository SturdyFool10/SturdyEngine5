module;

#include <array>
#include <memory>
#include <string>
#include <vector>

export module Sturdy.Core:ShaderReflection;

import Sturdy.Foundation;
import :ShaderTypes;

using std::array;
using std::shared_ptr;
using std::string;
using std::vector;

export namespace SFT::Core::Slang {

    // --- Type system enums ---

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

    enum class ShaderMatrixLayout {
        Unknown,
        RowMajor,
        ColumnMajor,
    };

    // --- Reflection structs ---

    struct ShaderTypeReflection;

    struct ShaderFieldReflection {
        string name;
        shared_ptr<ShaderTypeReflection> type;
        u64 offset = 0;
        u64 size = 0;
        u64 stride = 0;
    };

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

    struct ShaderDescriptorRangeReflection {
        ShaderBindingType type = ShaderBindingType::Unknown;
        ShaderParameterCategory category = ShaderParameterCategory::None;
        u32 binding = 0;
        u32 count = 0;
    };

    struct ShaderDescriptorSetReflection {
        u32 space = 0;
        vector<ShaderDescriptorRangeReflection> ranges;
    };

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
