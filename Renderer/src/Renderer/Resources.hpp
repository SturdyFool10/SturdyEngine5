#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <vector>
#pragma endregion

#include <glm/vec3.hpp>

#include <RHI/RHI.hpp>
#include "Handles.hpp"
#include "Geometry.hpp"

using std::vector;

namespace SFT::Renderer {

                                                                                                         
                                                                                                         
                                                                                                      
                                                                               
    struct MeshResource {
        MeshHandle handle{};
        UString label;
        vector<GeometryVertex> vertices;
        vector<u32> indices;
                                                                                    
                                                                                                 
        u32 vertex_offset = 0;
        u32 index_offset = 0;
                                                                                               
                                                                 
        u32 vertex_count = 0;
        u32 index_count = 0;
        bool gpu_resident = false;
        RHI::AccelerationStructureHandle bottom_level_acceleration_structure{};
        bool alive = false;
                                                                                                      
                                                                                                       
                                                                        
                                                                                                     
                                                                                                        
                                                                                                   
                                                         
        glm::vec3 bounds_center{0.0f};
        f32 bounds_radius = 0.0f;
    };

    struct MaterialResource {
        MaterialHandle handle{};
        UString label;
        bool alive = false;
    };

    struct TextureResource {
        TextureHandle handle{};
        UString label;
        RHI::TextureHandle texture{};
        RHI::TextureViewHandle view{};
        RHI::SamplerHandle sampler{};
                                                                                                      
                                                                                                           
        u32 width = 0;
        u32 height = 0;
        u32 mip_levels = 1;
        RHI::Format format = RHI::Format::Undefined;
        vector<byte> pixel_data;
                                                                                                    
                                                                              
        vector<RHI::QueueClass> concurrent_queue_classes;
        bool alive = false;
                                                                                                    
                                                                                                   
                                                  
        bool owns_gpu_resources = true;
                                                                                                    
                                                                                                         
                                                               
        bool externally_destroyable = true;
    };

                                                                                                    
                                                                                             
                                                                                                     
                                                                                                       
                                                                                             
                                     
    struct TextureUploadSubmission {
        RHI::CommandBufferHandle command_buffer{};
        RHI::FenceHandle fence{};
    };

} // namespace SFT::Renderer
