#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <span>
#include <type_traits>
#pragma endregion

#include <RHI/Flags.hpp>
#include <RHI/Handles.hpp>

using std::span;

namespace SFT::RHI {

                                                                                                 
                                                                                                    
                                                                                  
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

                                                                                                
                                                                                                    
                                                                                                    
    enum class ShaderLanguage : u32 {
        SpirV,
        Dxil,
        Msl,
        Wgsl,
    };

    struct ShaderModuleDesc {
        ShaderLanguage language = ShaderLanguage::SpirV;
                                                                                                  
                                                                                      
        span<const std::byte> code;
        const char *label = nullptr;
    };

                                                                                                 
                                                                                        
    struct ShaderEntry {
        ShaderModuleHandle module{};
                                                                                                    
        const char *entry_point = "main";
                                                                                                          
                                                                                                   
                                                                                                           
                                     
        ShaderStage stage = ShaderStage::None;
    };



                                                                                                     
                                                                                                   
                                                       
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
        Uint8x4Unorm,
        Sint8x4Norm,
        Uint16x2Unorm,
        Uint16x4Unorm,
        Float16x2,
        Float16x4,
    };

                                                                                                       
    enum class VertexStepMode : u32 {
        Vertex,
        Instance,
    };

                                                                                                     
                                                                           
    struct VertexAttribute {
        VertexFormat format = VertexFormat::Float32x3;
        u32 offset = 0;
        u32 shader_location = 0;
    };

                                                                                              
                                                                                                     
                                                     
    struct VertexBufferLayout {
        u64 stride = 0;
        VertexStepMode step_mode = VertexStepMode::Vertex;
        span<const VertexAttribute> attributes;
    };

    template <>
    struct enable_flag_ops<ShaderStage> : std::true_type {};

} // namespace SFT::RHI
