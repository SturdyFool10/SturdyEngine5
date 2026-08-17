#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <span>
#include <type_traits>
#pragma endregion

#include "Flags.hpp"
#include "Types.hpp"
#include "Handles.hpp"
#include "Shader.hpp"
#include "Resources.hpp"

using std::span;

namespace SFT::RHI {



    enum class PrimitiveTopology : u32 {
        PointList,
        LineList,
        LineStrip,
        TriangleList,
        TriangleStrip,
    };



    enum class PolygonMode : u32 {
        Fill,
        Line,
        Point,
    };

    enum class CullMode : u32 {
        None,
        Front,
        Back,
    };

                                                                                                   
                                                                                                
                                                                          
       
                                                                                                    
                                                                                                     
                                                                                                      
                                                                                                      
                                                                  
    enum class FrontFace : u32 {
        CounterClockwise,
        Clockwise,
    };

    struct RasterizationState {
        PolygonMode polygon_mode = PolygonMode::Fill;
        CullMode cull_mode = CullMode::Back;
        FrontFace front_face = FrontFace::CounterClockwise;
        bool depth_clamp_enable = false;
                                                                                                   
                          
        f32 depth_bias_constant = 0.0f;
        f32 depth_bias_slope_scale = 0.0f;
        f32 depth_bias_clamp = 0.0f;
        f32 line_width = 1.0f;
    };



    enum class StencilOp : u32 {
        Keep,
        Zero,
        Replace,
        IncrementClamp,
        DecrementClamp,
        Invert,
        IncrementWrap,
        DecrementWrap,
    };

    struct StencilFaceState {
        StencilOp fail_op = StencilOp::Keep;
        StencilOp depth_fail_op = StencilOp::Keep;
        StencilOp pass_op = StencilOp::Keep;
        CompareOp compare = CompareOp::Always;
    };

                                                                                                      
                                                                                            
                                                                     
    struct DepthStencilState {
        Format format = Format::Undefined;
        bool depth_test_enable = false;
        bool depth_write_enable = false;
        CompareOp depth_compare = CompareOp::Less;
        bool stencil_test_enable = false;
        StencilFaceState stencil_front{};
        StencilFaceState stencil_back{};
        u8 stencil_read_mask = 0xFF;
        u8 stencil_write_mask = 0xFF;
    };



    struct MultisampleState {
        SampleCount samples = SampleCount::X1;
        u32 sample_mask = ~0u;
        bool alpha_to_coverage_enable = false;
    };



    enum class BlendFactor : u32 {
        Zero,
        One,
        SrcColor,
        OneMinusSrcColor,
        DstColor,
        OneMinusDstColor,
        SrcAlpha,
        OneMinusSrcAlpha,
        DstAlpha,
        OneMinusDstAlpha,
        ConstantColor,
        OneMinusConstantColor,
        SrcAlphaSaturated,
    };

    enum class BlendOp : u32 {
        Add,
        Subtract,
        ReverseSubtract,
        Min,
        Max,
    };

                                                                            
    enum class ColorWriteMask : u32 {
        None = 0,
        Red = 1u << 0,
        Green = 1u << 1,
        Blue = 1u << 2,
        Alpha = 1u << 3,
        All = Red | Green | Blue | Alpha,
    };

    struct BlendComponent {
        BlendFactor src_factor = BlendFactor::One;
        BlendFactor dst_factor = BlendFactor::Zero;
        BlendOp op = BlendOp::Add;
    };

                                                                                             
                                                                                          
                                                                                               
    struct ColorTargetState {
        Format format = Format::Undefined;
        bool blend_enable = false;
        BlendComponent color{};
        BlendComponent alpha{};
        ColorWriteMask write_mask = ColorWriteMask::All;
    };



                                                                                             
                                                                                                    
                                                                                                         
                                                                                                        
                                               
       
                                                                                                      
                                                                                                         
                                                                                            
    struct RenderPipelineDesc {
        PipelineLayoutHandle layout{};

        ShaderEntry vertex{};
        ShaderEntry task{};
        ShaderEntry mesh{};
        ShaderEntry fragment{};

        span<const VertexBufferLayout> vertex_buffers;

        PrimitiveTopology topology = PrimitiveTopology::TriangleList;
        RasterizationState rasterization{};
        MultisampleState multisample{};
        DepthStencilState depth_stencil{};
        span<const ColorTargetState> color_targets;

                                                                                                     
                                                                                                      
                                                                                                        
                                                                                                               
        u32 view_mask = 0;

        const char *label = nullptr;
    };

    struct ComputePipelineDesc {
        PipelineLayoutHandle layout{};
        ShaderEntry compute{};
        const char *label = nullptr;
    };

    template <>
    struct enable_flag_ops<ColorWriteMask> : std::true_type {};

} // namespace SFT::RHI
