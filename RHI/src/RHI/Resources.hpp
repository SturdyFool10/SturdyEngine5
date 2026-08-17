#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <optional>
#include <span>
#include <type_traits>
#pragma endregion

#include "Flags.hpp"
#include "Types.hpp"
#include "Handles.hpp"
#include "Queues.hpp"

using std::optional;
using std::span;

namespace SFT::RHI {



                                                                                                   
                                                                                                     
                                                                 
    enum class BufferUsage : u32 {
        None = 0,
        TransferSrc = 1u << 0,
        TransferDst = 1u << 1,
        Vertex = 1u << 2,
        Index = 1u << 3,
        Uniform = 1u << 4,
        Storage = 1u << 5,
        Indirect = 1u << 6,
        ShaderBindingTable = 1u << 7,
        AccelerationStructure = 1u << 8,
        AccelerationStructureInput = 1u << 9,
        AccelerationStructureScratch = 1u << 10,
    };

                                                                                                    
                                                                                                  
                                      
    enum class MemoryLocation : u32 {
                                                                                                    
                                                     
        DeviceLocal,
                                                                                                    
                                                                                            
        HostUpload,
                                                                                                 
        HostReadback,
    };

    struct BufferDesc {
        u64 size = 0;
        BufferUsage usage = BufferUsage::None;
        MemoryLocation memory = MemoryLocation::DeviceLocal;
                                                                                     
                                                                                                       
                                                                  
        const char *label = nullptr;
    };



    enum class TextureDimension : u32 {
        Dim1D,
        Dim2D,
        Dim3D,
    };

    enum class TextureUsage : u32 {
        None = 0,
        TransferSrc = 1u << 0,
        TransferDst = 1u << 1,
        Sampled = 1u << 2,
        Storage = 1u << 3,
        ColorAttachment = 1u << 4,
        DepthStencilAttachment = 1u << 5,
                                                                                               
                                                                                                      
                                                                                                       
        TransientAttachment = 1u << 6,
    };

    struct TextureDesc {
        TextureDimension dimension = TextureDimension::Dim2D;
        Format format = Format::Undefined;
        Extent3D extent{};
        u32 mip_levels = 1;
        SampleCount samples = SampleCount::X1;
        TextureUsage usage = TextureUsage::None;
                                                                                                    
                                                                                                      
                                                                                                   
                                                                                            
                                                                                                  
                                                                                                       
                                                                                   
        span<const QueueClass> concurrent_queue_classes;
        const char *label = nullptr;
    };

                                                                                                     
                                                                                                          
                                                         
    enum class TextureViewType : u32 {
        View1D,
        View2D,
        View2DArray,
        ViewCube,
        ViewCubeArray,
        View3D,
    };

                                                                                         
                                                                                            
    inline constexpr u32 all_remaining = ~0u;

    struct TextureViewDesc {
        TextureHandle texture{};
        TextureViewType view_type = TextureViewType::View2D;
                                                                                           
                                                                    
        Format format = Format::Undefined;
        u32 base_mip_level = 0;
        u32 mip_level_count = all_remaining;
        u32 base_array_layer = 0;
        u32 array_layer_count = all_remaining;
        const char *label = nullptr;
    };



    enum class Filter : u32 {
        Nearest,
        Linear,
    };

    enum class MipmapMode : u32 {
        Nearest,
        Linear,
    };

    enum class AddressMode : u32 {
        Repeat,
        MirroredRepeat,
        ClampToEdge,
        ClampToBorder,
    };

    enum class BorderColor : u32 {
        TransparentBlack,
        OpaqueBlack,
        OpaqueWhite,
    };

                                                                                                    
                                                                               
    enum class CompareOp : u32 {
        Never,
        Less,
        Equal,
        LessEqual,
        Greater,
        NotEqual,
        GreaterEqual,
        Always,
    };

    struct SamplerDesc {
        Filter min_filter = Filter::Linear;
        Filter mag_filter = Filter::Linear;
        MipmapMode mipmap_mode = MipmapMode::Linear;
        AddressMode address_u = AddressMode::Repeat;
        AddressMode address_v = AddressMode::Repeat;
        AddressMode address_w = AddressMode::Repeat;
        f32 mip_lod_bias = 0.0f;
        f32 min_lod = 0.0f;
        f32 max_lod = 1000.0f;
                                                                                           
        f32 max_anisotropy = 0.0f;
                                                                                     
        bool compare_enable = false;
        CompareOp compare = CompareOp::Never;
        BorderColor border_color = BorderColor::TransparentBlack;
        const char *label = nullptr;
    };

    template <>
    struct enable_flag_ops<BufferUsage> : std::true_type {};
    template <>
    struct enable_flag_ops<TextureUsage> : std::true_type {};

} // namespace SFT::RHI
