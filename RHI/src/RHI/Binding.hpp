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

                                                                                                 
                                                         
    struct BindGroupDesc {
        BindGroupLayoutHandle layout{};
        span<const BindGroupEntry> entries;
                                                                                                        
                                                                                                     
        u32 variable_descriptor_count = 0;
        const char *label = nullptr;
    };

                                                                                                    
                                                                                            
    struct PushConstantRange {
        ShaderStage stages = ShaderStage::None;
        u32 offset = 0;
        u32 size = 0;
    };

                                                                                                     
                                                                                      
                                  
    struct PipelineLayoutDesc {
        span<const BindGroupLayoutHandle> bind_group_layouts;
        span<const PushConstantRange> push_constant_ranges;
        const char *label = nullptr;
    };

} // namespace SFT::RHI
