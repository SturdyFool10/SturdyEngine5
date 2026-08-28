#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <type_traits>
#pragma endregion

#include <RHI/Flags.hpp>
#include <RHI/Handles.hpp>
#include <RHI/Shader.hpp>

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
