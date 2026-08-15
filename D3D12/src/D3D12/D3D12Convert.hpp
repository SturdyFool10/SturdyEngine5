#pragma once

// The complete RHI -> Direct3D 12 / DXGI vocabulary mapping. Every enum the RHI defines is
// translated here and nowhere else, so a translation is stated once and every call site agrees.
//
// Three mappings deserve their own note up front, because they are where D3D12 genuinely differs
// from the Vulkan model the RHI's shapes were drawn from:
//
//  1. Depth formats are *typeless* at the resource level whenever a depth texture is also sampled.
//     Vulkan lets one VkFormat serve as both a depth attachment and a sampled image; D3D12 does not
//     — a DSV needs DXGI_FORMAT_D32_FLOAT while an SRV over the same memory needs
//     DXGI_FORMAT_R32_FLOAT, and the only resource format that can produce both views is
//     DXGI_FORMAT_R32_TYPELESS. Hence the three-function split below (resource / view / depth-SRV).
//
//  2. Barriers map onto *enhanced barriers* (D3D12_BARRIER_SYNC/ACCESS/LAYOUT), not legacy resource
//     states. That is the correct target: enhanced barriers are D3D12's port of exactly the
//     synchronization2 model the RHI's :Barrier is written against, so the mapping is
//     structure-preserving instead of lossy. `to_legacy_*` exists only for the pre-enhanced-barrier
//     fallback path (see D3D12Barriers.hpp), where the (stage, access, layout) triple has to be
//     collapsed into a single D3D12_RESOURCE_STATES value.
//
//  3. Blend factors that name a *color* channel set (SrcColor, DstColor, ...) have both a color and
//     an alpha spelling in D3D12 (D3D12_BLEND_SRC_COLOR vs D3D12_BLEND_SRC_ALPHA); using the color
//     one in an alpha blend equation is a debug-layer error. `to_d3d12_blend` therefore takes an
//     `is_alpha` flag and folds color factors onto their alpha equivalents, which is what D3D11/12
//     have always required and what Vulkan silently allows.

#include <D3D12/D3D12Common.hpp>

namespace SFT::D3D12 {

    // ─── Formats ─────────────────────────────────────────────────────────────────

    // The format a *resource* is created with. Identical to to_dxgi_view_format() except for depth
    // formats that must be typeless to also support a shader-resource view — pass the texture's usage
    // so the decision is made from the descriptor rather than guessed.
    [[nodiscard]] DXGI_FORMAT to_dxgi_resource_format(rhi::Format format, rhi::TextureUsage usage) noexcept;

    // The format a view (SRV/UAV/RTV/vertex/index/swapchain) is created with. Depth formats map to
    // their D*_ spelling here; use to_dxgi_depth_srv_format() for the shader-read view of one.
    [[nodiscard]] DXGI_FORMAT to_dxgi_view_format(rhi::Format format) noexcept;

    // The SRV format for reading a depth texture's depth aspect (D32Float -> R32_FLOAT,
    // D24UnormS8Uint -> R24_UNORM_X8_TYPELESS, ...). Returns DXGI_FORMAT_UNKNOWN for a non-depth
    // format — callers use to_dxgi_view_format() for those.
    [[nodiscard]] DXGI_FORMAT to_dxgi_depth_srv_format(rhi::Format format) noexcept;

    // The typeless resource format a depth format has to be created as when it will also be sampled.
    // DXGI_FORMAT_UNKNOWN for anything without one.
    [[nodiscard]] DXGI_FORMAT to_dxgi_typeless_format(rhi::Format format) noexcept;

    // Bytes per texel for an uncompressed format, or bytes per 4x4 block for a block-compressed one.
    // Copy-region row-pitch math needs both cases and needs to know which it got — see
    // format_block_extent() below.
    [[nodiscard]] u32 format_element_bytes(rhi::Format format) noexcept;

    // Texels per block edge: 4 for the BC formats, 1 for everything else.
    [[nodiscard]] u32 format_block_extent(rhi::Format format) noexcept;

    [[nodiscard]] DXGI_FORMAT to_dxgi(rhi::VertexFormat format) noexcept;
    [[nodiscard]] DXGI_FORMAT to_dxgi(rhi::IndexFormat format) noexcept;

    // The DXGI color space a swapchain is tagged with via IDXGISwapChain3::SetColorSpace1.
    // Returns false when the RHI color space has no DXGI equivalent (the linear wide-gamut spaces
    // Vulkan exposes but DXGI does not name), leaving `out` untouched.
    [[nodiscard]] bool to_dxgi_color_space(rhi::ColorSpace color_space, DXGI_COLOR_SPACE_TYPE &out) noexcept;

    // ─── Resources ───────────────────────────────────────────────────────────────

    [[nodiscard]] D3D12_RESOURCE_FLAGS to_d3d12_resource_flags(rhi::BufferUsage usage) noexcept;
    [[nodiscard]] D3D12_RESOURCE_FLAGS to_d3d12_resource_flags(rhi::TextureUsage usage, rhi::Format format) noexcept;
    [[nodiscard]] D3D12_HEAP_TYPE to_d3d12_heap_type(rhi::MemoryLocation memory) noexcept;
    [[nodiscard]] D3D12_RESOURCE_DIMENSION to_d3d12(rhi::TextureDimension dimension) noexcept;

    // ─── Samplers / comparison ───────────────────────────────────────────────────

    [[nodiscard]] D3D12_COMPARISON_FUNC to_d3d12(rhi::CompareOp op) noexcept;
    [[nodiscard]] D3D12_TEXTURE_ADDRESS_MODE to_d3d12(rhi::AddressMode mode) noexcept;
    // D3D12 encodes min/mag/mip filtering, anisotropy, and the comparison/reduction mode into one
    // packed D3D12_FILTER value rather than the separate fields Vulkan (and SamplerDesc) use.
    [[nodiscard]] D3D12_FILTER to_d3d12_filter(const rhi::SamplerDesc &desc) noexcept;
    void fill_border_color(rhi::BorderColor color, float out[4]) noexcept;

    // ─── Pipeline state ──────────────────────────────────────────────────────────

    [[nodiscard]] D3D12_FILL_MODE to_d3d12(rhi::PolygonMode mode) noexcept;
    [[nodiscard]] D3D12_CULL_MODE to_d3d12(rhi::CullMode mode) noexcept;
    [[nodiscard]] D3D12_STENCIL_OP to_d3d12(rhi::StencilOp op) noexcept;
    [[nodiscard]] D3D12_BLEND to_d3d12_blend(rhi::BlendFactor factor, bool is_alpha) noexcept;
    [[nodiscard]] D3D12_BLEND_OP to_d3d12(rhi::BlendOp op) noexcept;
    [[nodiscard]] u8 to_d3d12_write_mask(rhi::ColorWriteMask mask) noexcept;
    [[nodiscard]] D3D12_PRIMITIVE_TOPOLOGY_TYPE to_d3d12_topology_type(rhi::PrimitiveTopology topology) noexcept;
    [[nodiscard]] D3D_PRIMITIVE_TOPOLOGY to_d3d12_topology(rhi::PrimitiveTopology topology) noexcept;

    // ─── Queues / queries ────────────────────────────────────────────────────────

    [[nodiscard]] D3D12_COMMAND_LIST_TYPE to_d3d12(rhi::QueueClass queue) noexcept;
    [[nodiscard]] D3D12_QUERY_HEAP_TYPE to_d3d12_query_heap_type(rhi::QueryType type) noexcept;
    [[nodiscard]] D3D12_QUERY_TYPE to_d3d12_query_type(rhi::QueryType type, bool precise_occlusion) noexcept;
    // Bytes one resolved result of `type` occupies in a destination buffer. D3D12's resolve stride is
    // fixed by the query type (8 for occlusion/timestamp, sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)
    // for statistics) and is *not* caller-selectable the way Vulkan's is.
    [[nodiscard]] u64 query_result_bytes(rhi::QueryType type) noexcept;

    // ─── Enhanced barriers ───────────────────────────────────────────────────────

    [[nodiscard]] D3D12_BARRIER_SYNC to_d3d12_sync(rhi::PipelineStage stages) noexcept;
    [[nodiscard]] D3D12_BARRIER_ACCESS to_d3d12_access(rhi::AccessFlags access) noexcept;
    [[nodiscard]] D3D12_BARRIER_LAYOUT to_d3d12_layout(rhi::TextureLayout layout) noexcept;

    // ─── Legacy resource states (pre-enhanced-barrier fallback) ──────────────────

    // The single D3D12_RESOURCE_STATES value that best represents a texture in `layout`. Lossy by
    // construction — that is the whole reason enhanced barriers exist — so this is only reached on
    // devices reporting EnhancedBarriersSupported == FALSE.
    [[nodiscard]] D3D12_RESOURCE_STATES to_legacy_texture_state(rhi::TextureLayout layout) noexcept;
    // The same collapse for a buffer, which has no layout: derived from the access mask instead.
    [[nodiscard]] D3D12_RESOURCE_STATES to_legacy_buffer_state(rhi::AccessFlags access) noexcept;

    // ─── Ray tracing ─────────────────────────────────────────────────────────────

    [[nodiscard]] D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS to_d3d12(
        rhi::AccelerationStructureBuildFlags flags) noexcept;
    [[nodiscard]] D3D12_RAYTRACING_GEOMETRY_FLAGS to_d3d12(rhi::AccelerationStructureGeometryFlags flags) noexcept;

    // ─── Shader stage visibility ─────────────────────────────────────────────────

    // The root-signature visibility for a binding's stage mask. D3D12_SHADER_VISIBILITY names exactly
    // one stage or ALL — there is no bitmask — so anything visible to more than one stage collapses to
    // ALL. That is a legality-preserving widening (a binding visible to more stages than it needs is
    // valid, just marginally less optimal), which is the correct direction to err.
    [[nodiscard]] D3D12_SHADER_VISIBILITY to_d3d12_visibility(rhi::ShaderStage stages) noexcept;

} // namespace SFT::D3D12
