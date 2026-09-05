#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <type_traits>
#pragma endregion

#include <RHI/Flags.hpp>
#include <RHI/Handles.hpp>
#include <RHI/Shader.hpp>
#include <RHI/Types.hpp>

using std::span;

namespace SFT::RHI {

                                                                                                      
                                                                           
    enum class BindingType : u32 {
        UniformBuffer,
        StorageBuffer,
        ReadOnlyStorageBuffer,
        SampledTexture,
        StorageTexture,
        Sampler,
        CombinedImageSampler,
        AccelerationStructure,
                                                                                                        
                                                                                                      
                                                                                                            
                                                                                     
        InputAttachment,
    };

    /// Read/write mode of a StorageTexture binding. Vulkan/D3D12 storage images are generically
    /// read-write capable regardless of a declared mode -- only WebGPU's WGSL requires the exact
    /// mode (`write`/`read`/`read_write`) to be declared statically in the bind group layout, so
    /// this is meaningful only to the WebGPU backend (see BindGroupLayoutEntry::storage_access).
    enum class StorageTextureAccess : u32 {
        WriteOnly,
        ReadOnly,
        ReadWrite,
    };

                                                                                                         
                                                                                                      
                                                                                         
                                                                                                      
                                                                                            
                                                                                                        
                                                                                  
                                                                                                           
                                                                                                       
                                                                              
                                                                                                     
                                                                                             
                                                                                              
                                                                                                           
                                                                                                     
                                                                            
    enum class BindingFlags : u32 {
        None = 0,
        PartiallyBound = 1u << 0,
        UpdateAfterBind = 1u << 1,
        VariableDescriptorCount = 1u << 2,
    };

                                                                                                     
                                                                                                    
                                                                                               
                                                                                                        
    struct BindGroupLayoutEntry {
                                                                                                    
                                                                                                    
        u32 binding = 0;
                                                                                                  
                                                                                                  
                                                                                              
        u32 shader_register = ~0u;
        BindingType type = BindingType::UniformBuffer;
        ShaderStage visibility = ShaderStage::None;
        u32 count = 1;
        bool has_dynamic_offset = false;
                                                                                                      
                                                                                                       
                                                                                                      
                                                                    
        BindingFlags flags = BindingFlags::None;
                                                                                                    
                                                                                                    
                                                        
        u32 input_attachment_index = 0;
        // Only meaningful when type == CombinedImageSampler: the binding slot of the separate
        // sampler half, for backends with no combined-image-sampler descriptor type of their own
        // (WebGPU). Vulkan/D3D12 ignore this -- Vulkan has one native descriptor type for the pair,
        // and D3D12 already splits SRV/Sampler by register class using `binding` for both. ~0u means
        // "not applicable" (the reflection layer that populates this always sets it for a
        // WGSL-target CombinedImageSampler entry; see Renderer/ReflectionBinding.cpp).
        u32 paired_binding = ~0u;
        // Only meaningful when type == StorageTexture: WGSL storage texture types are
        // format-parameterized (e.g. `texture_storage_2d<rgba32float, write>`), so WebGPU's
        // WGPUBindGroupLayoutEntry::storageTexture.format must exactly match whatever format the
        // shader declared -- unlike Vulkan/D3D12, where a storage image/UAV descriptor carries no
        // format at layout-creation time (format compatibility is enforced by the shader binary
        // itself, not the descriptor). Vulkan/D3D12 backends ignore both fields below. Undefined/
        // WriteOnly are the pre-existing WebGPU-backend defaults this replaces (previously
        // hardcoded to RGBA8Unorm/WriteOnly for every StorageTexture binding regardless of the
        // shader's real declaration).
        Format storage_format = Format::Undefined;
        StorageTextureAccess storage_access = StorageTextureAccess::WriteOnly;
        // Only meaningful when type == Sampler: WGSL has two distinct sampler binding types,
        // `sampler` and `sampler_comparison`, and WebGPU rejects a mismatch against what the
        // shader declared ("Comparison flag doesn't match the shader"). Vulkan/D3D12 sampler
        // descriptors carry no such flag -- a comparison sampler is just a VkSampler/D3D12 sampler
        // with compareEnable set on the sampler object itself, unrelated to the descriptor/binding
        // layout -- so both backends ignore this field.
        bool sampler_is_comparison = false;
        // Only meaningful when type == SampledTexture: WGSL has two genuinely different
        // sampled-texture kinds, `texture_2d<f32>` and `texture_depth_2d`, and WebGPU rejects a
        // mismatch ("Texture class Sampled doesn't match the shader Depth"). Vulkan/D3D12 make no
        // such distinction at the descriptor level -- a depth texture there is just a regular
        // sampled image/SRV -- so both backends ignore this field.
        bool sampled_texture_is_depth = false;
        // Only meaningful when type == SampledTexture: WGSL has genuinely distinct multisampled
        // texture types (`texture_multisampled_2d<f32>`/`texture_depth_multisampled_2d`) from their
        // non-multisampled counterparts, and WebGPU rejects a mismatch ("Texture class Sampled ...
        // doesn't match the shader Sampled ... multi: true"). Vulkan/D3D12 make no such distinction
        // at the descriptor level -- an MSAA image there is bound the same way as any other sampled
        // image/SRV, with the sample count carried by the image itself -- so both backends ignore
        // this field.
        bool sampled_texture_is_multisampled = false;
    };

    template <>
    struct enable_flag_ops<BindingFlags> : std::true_type {};

                                                                                                   
                                                                             
                                                 
    struct BindGroupLayoutDesc {
        span<const BindGroupLayoutEntry> entries;
        const char *label = nullptr;
    };

                                                                                                  
                                                                                                 
                                                                                              
                                                                                                    
                                                                           
    struct BindGroupEntry {
        u32 binding = 0;
                                                                                                  
        u32 array_element = 0;
        BufferHandle buffer{};
        u64 offset = 0;
        u64 size = 0;
        u32 structure_stride = 0;
        TextureViewHandle texture_view{};
        SamplerHandle sampler{};
        AccelerationStructureHandle acceleration_structure{};
    };

                                                                                                 
                                                         
    /// Describes the expected lifetime/allocation behavior of a bind group.
    enum class BindGroupLifetime : u8 {
        Persistent,
        FrameTransient,
    };

    struct BindGroupDesc {
        BindGroupLayoutHandle layout{};
        span<const BindGroupEntry> entries;
                                                                                                        
                                                                                                     
        u32 variable_descriptor_count = 0;
        BindGroupLifetime lifetime = BindGroupLifetime::Persistent;
        const char *label = nullptr;
    };

                                                                                                    
                                                                                            
    struct PushConstantRange {
        ShaderStage stages = ShaderStage::None;
        u32 offset = 0;
        u32 size = 0;

        /// D3D-style register/space this push-constant buffer was compiled to.
        ///
        /// Vulkan ignores this: push constants there are a distinct hardware mechanism with no
        /// descriptor register of their own, addressed by offset alone. D3D12 has no such
        /// mechanism — Slang lowers `[[push_constant]]` to an ordinary `cbuffer` for the DXIL
        /// target, at whatever register its whole-program layout happens to assign, which is not
        /// reliably 0 once other constant buffers are reflected into the same program. The D3D12
        /// backend must place its root-constants entry at this exact register or
        /// `CreateGraphicsPipelineState` rejects the pipeline as root-signature-incompatible.
        u32 shader_register = 0;
        u32 register_space = 0;
    };

                                                                                                     
                                                                                      
                                  
    struct PipelineLayoutDesc {
        span<const BindGroupLayoutHandle> bind_group_layouts;
        span<const PushConstantRange> push_constant_ranges;
        const char *label = nullptr;
    };

} // namespace SFT::RHI
