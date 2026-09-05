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
        // Offset of the second descriptor range, valid only when descriptor_range_count > 1 (the
        // WGSL target decomposes a CombinedTextureSampler into two independent descriptor ranges --
        // texture then sampler -- unlike SPIR-V/Vulkan, which keeps it as one). ~0u otherwise.
        u32 second_binding = ~0u;
        u32 count = 0;
        // Raw SlangImageFormat value (see slang-image-format-defs.h), only meaningful for a
        // StorageTexture/MutableTexture binding range -- WGSL storage texture types are
        // format-parameterized, so this must be translated (Renderer/ReflectionBinding.cpp does
        // this) into an RHI::Format for the WebGPU backend to declare the matching layout.
        u32 image_format = 0;
        ShaderResourceAccess access = ShaderResourceAccess::Unknown;
        // True only for a Sampler binding range whose declared type is SamplerComparisonState.
        // Slang's reflection API has no dedicated flag or Kind for this (SamplerState and
        // SamplerComparisonState both report ShaderTypeKind::SamplerState); this is set instead by
        // checking the leaf type layout's own name in ShaderImpl.cpp::parse_binding_range, the only
        // signal Slang's reflection surface actually exposes for the distinction. Needed because
        // WGSL has two genuinely different sampler binding types (`sampler` vs
        // `sampler_comparison`) and WebGPU rejects a mismatch outright ("Comparison flag doesn't
        // match the shader"), unlike Vulkan/D3D12 where a sampler descriptor carries no such flag.
        b8 is_comparison_sampler = false;
        // True only for a SampledTexture binding range whose declared type is a Slang depth/shadow
        // texture (DepthTexture2D and friends -- SLANG_TEXTURE_SHADOW_FLAG on the leaf type's
        // resource shape). WGSL has two genuinely different sampled-texture kinds, `texture_2d<f32>`
        // and `texture_depth_2d`, and WebGPU rejects a mismatch ("Texture class Sampled { kind:
        // Float } doesn't match the shader Depth"); Vulkan/D3D12 make no such distinction at the
        // descriptor level (a depth texture there is just a regular sampled image/SRV).
        b8 is_depth_texture = false;
        // True only for a SampledTexture binding range whose declared type is multisampled
        // (Texture2DMS and friends, including DepthTexture2DMS -- SLANG_TEXTURE_MULTISAMPLE_FLAG on
        // the leaf type's resource shape). WGSL has genuinely distinct multisampled texture types,
        // and WebGPU rejects a mismatch ("Texture class Sampled ... doesn't match the shader
        // Sampled ... multi: true"); Vulkan/D3D12 make no such distinction at the descriptor level.
        b8 is_multisampled_texture = false;
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
