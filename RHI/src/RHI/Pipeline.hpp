#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <type_traits>
#pragma endregion

#include <RHI/Flags.hpp>
#include <RHI/Types.hpp>
#include <RHI/Handles.hpp>
#include <RHI/Shader.hpp>
#include <RHI/Resources.hpp> // CompareOp

using std::span;

namespace SFT::RHI {

    // ─── Input assembly ──────────────────────────────────────────────────────────

    enum class PrimitiveTopology : u32 {
        PointList,
        LineList,
        LineStrip,
        TriangleList,
        TriangleStrip,
    };

    // ─── Rasterization ───────────────────────────────────────────────────────────

    enum class PolygonMode : u32 {
        Fill,
        Line,  // wireframe (a device feature on some backends)
        Point,
    };

    enum class CullMode : u32 {
        None,
        Front,
        Back,
    };

    // Winding considered front-facing by the RHI rasterizer. Backends map this state 1:1; any
    // coordinate-system reconciliation belongs to viewport/projection plumbing rather than being
    // applied a second time to the raster state.
    enum class FrontFace : u32 {
        CounterClockwise,
        Clockwise,
    };

    struct RasterizationState {
        PolygonMode polygon_mode = PolygonMode::Fill;
        CullMode cull_mode = CullMode::Back;
        FrontFace front_face = FrontFace::CounterClockwise;
        bool depth_clamp_enable = false;
        // Constant/slope-scaled depth bias — the usual shadow-map peter-panning knobs. All zero
        // disables bias.
        f32 depth_bias_constant = 0.0f;
        f32 depth_bias_slope_scale = 0.0f;
        f32 depth_bias_clamp = 0.0f;
        f32 line_width = 1.0f;

        // Requires Feature::ConservativeRasterization. Overestimation mode only (any primitive
        // coverage of a pixel's area rasterizes that pixel) -- the only mode D3D12 exposes
        // (D3D12_CONSERVATIVE_RASTERIZATION_MODE has no underestimate option), so Vulkan's
        // VK_EXT_conservative_rasterization underestimate mode is deliberately not surfaced here to
        // keep this one flag meaningful on both backends.
        bool conservative_rasterization_enable = false;
    };

    // ─── Depth / stencil ─────────────────────────────────────────────────────────

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

    // Depth/stencil attachment behavior. `format` names the attachment this state targets (needed up
    // front for dynamic-rendering pipelines, which have no VkRenderPass to infer it from);
    // Undefined means the pipeline has no depth/stencil attachment.
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

        // Requires Feature::DepthBoundsTest. Both backends require this enabled at pipeline-creation
        // time before RenderPassEncoder::set_depth_bounds() may be called against a draw using this
        // pipeline (D3D12: D3D12_DEPTH_STENCIL_DESC1::DepthBoundsTestEnable; Vulkan:
        // VkPipelineDepthStencilStateCreateInfo::depthBoundsTestEnable) -- the bounds themselves are
        // dynamic per-draw state on both, set separately.
        bool depth_bounds_test_enable = false;
    };

    // ─── Multisample ─────────────────────────────────────────────────────────────

    struct MultisampleState {
        SampleCount samples = SampleCount::X1;
        u32 sample_mask = ~0u;
        bool alpha_to_coverage_enable = false;

        // Requires Feature::SampleLocations. Opts this pipeline into
        // RenderPassEncoder::set_sample_locations() being called against draws that use it. Vulkan
        // requires this at pipeline-creation time (VkPipelineSampleLocationsStateCreateInfoEXT);
        // D3D12's SetSamplePositions has no such pipeline-time precondition and ignores this field --
        // it's carried here anyway so callers write one RHI-level flag instead of branching per backend.
        bool sample_locations_enable = false;
    };

    // ─── Color blend ─────────────────────────────────────────────────────────────

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

    // Which channels a color target write touches. A bitmask (see :Flags).
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

    // One color attachment's format + blend/write behavior. `blend_enable` false = straight
    // overwrite (the color/alpha components are then ignored). `format` is named here so
    // dynamic-rendering pipelines know their attachment formats without a render-pass object.
    struct ColorTargetState {
        Format format = Format::Undefined;
        bool blend_enable = false;
        BlendComponent color{};
        BlendComponent alpha{};
        ColorWriteMask write_mask = ColorWriteMask::All;
    };

    // ─── Pipelines ───────────────────────────────────────────────────────────────

    // Everything needed to build a raster pipeline, described against dynamic rendering (no
    // render-pass object): the shader stages, vertex/mesh front-end, fixed-function state, and the
    // attachment formats it will render into. Viewport/scissor are always dynamic (set per-draw via the
    // render-pass encoder), so they aren't baked in here. All spans are non-owning — consumed during
    // create_render_pipeline() (see :Device).
    //
    // Front-end rule: set `mesh` (and optionally `task`) for a mesh-shader pipeline, or set `vertex`
    // for a traditional vertex-input pipeline. A mesh pipeline requires Feature::MeshShader and ignores
    // `vertex_buffers`/`topology`; a task stage additionally requires Feature::TaskShader.
    struct RenderPipelineDesc {
        PipelineLayoutHandle layout{};

        ShaderEntry vertex{};
        ShaderEntry task{};
        ShaderEntry mesh{};
        ShaderEntry fragment{}; // module may be null for a depth-only pipeline

        span<const VertexBufferLayout> vertex_buffers;

        PrimitiveTopology topology = PrimitiveTopology::TriangleList;
        RasterizationState rasterization{};
        MultisampleState multisample{};
        DepthStencilState depth_stencil{}; // format Undefined => no depth/stencil attachment
        span<const ColorTargetState> color_targets;

        // Multiview view mask (requires Feature::Multiview): each set bit is a view rendered in one
        // pass, broadcasting to that array layer (single-pass cascaded shadow maps, cubemap shadows,
        // stereo VR). 0 disables multiview. Must match the RenderPassDesc/RenderBundleDesc it executes
        // in — dynamic-rendering pipelines bake the view mask in, having no render-pass object to carry it.
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
