#pragma once


#include <D3D12/D3D12Common.hpp>

namespace SFT::D3D12 {


    /// Converts the value to DXGI resource format representation.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    /// @param usage Usage flags or category applied to the resource.
    ///
    /// @return Returns the value converted to DXGI resource format representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] DXGI_FORMAT to_dxgi_resource_format(rhi::Format format, rhi::TextureUsage usage) noexcept;


    /// Converts the value to DXGI view format representation.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to DXGI view format representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] DXGI_FORMAT to_dxgi_view_format(rhi::Format format) noexcept;


    /// Converts the value to DXGI depth srv format representation.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to DXGI depth srv format representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] DXGI_FORMAT to_dxgi_depth_srv_format(rhi::Format format) noexcept;


    /// Converts the value to DXGI typeless format representation.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to DXGI typeless format representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] DXGI_FORMAT to_dxgi_typeless_format(rhi::Format format) noexcept;


    /// Computes the format element bytes required by the supplied values.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] u32 format_element_bytes(rhi::Format format) noexcept;


    /// Formats block extent using the supplied arguments and current state.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] u32 format_block_extent(rhi::Format format) noexcept;

    /// Converts the value to DXGI representation.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to DXGI representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] DXGI_FORMAT to_dxgi(rhi::VertexFormat format) noexcept;
    /// Converts the value to DXGI representation.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to DXGI representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] DXGI_FORMAT to_dxgi(rhi::IndexFormat format) noexcept;


    /// Converts the value to DXGI color space representation.
    ///
    /// @param color_space `color_space` value used by the operation.
    /// @param out `out` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool to_dxgi_color_space(rhi::ColorSpace color_space, DXGI_COLOR_SPACE_TYPE &out) noexcept;


    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param usage Usage flags or category applied to the resource.
    ///
    /// @return Returns the value converted to D3D12 resource flags representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_RESOURCE_FLAGS to_d3d12_resource_flags(rhi::BufferUsage usage) noexcept;
    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param usage Usage flags or category applied to the resource.
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to D3D12 resource flags representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_RESOURCE_FLAGS to_d3d12_resource_flags(rhi::TextureUsage usage, rhi::Format format) noexcept;
    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param memory `memory` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 heap type representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_HEAP_TYPE to_d3d12_heap_type(rhi::MemoryLocation memory) noexcept;
    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param dimension `dimension` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_RESOURCE_DIMENSION to_d3d12(rhi::TextureDimension dimension) noexcept;


    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param op `op` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_COMPARISON_FUNC to_d3d12(rhi::CompareOp op) noexcept;
    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param mode Mode controlling how the operation is performed.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_TEXTURE_ADDRESS_MODE to_d3d12(rhi::AddressMode mode) noexcept;


    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value converted to D3D12 filter representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_FILTER to_d3d12_filter(const rhi::SamplerDesc &desc) noexcept;
    /// Fills border color using the supplied arguments and current state.
    ///
    /// @param color `color` value used by the operation.
    /// @param out `out` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void fill_border_color(rhi::BorderColor color, float out[4]) noexcept;


    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param mode Mode controlling how the operation is performed.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_FILL_MODE to_d3d12(rhi::PolygonMode mode) noexcept;
    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param mode Mode controlling how the operation is performed.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_CULL_MODE to_d3d12(rhi::CullMode mode) noexcept;
    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param op `op` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_STENCIL_OP to_d3d12(rhi::StencilOp op) noexcept;
    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param factor `factor` value used by the operation.
    /// @param is_alpha `is_alpha` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 blend representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_BLEND to_d3d12_blend(rhi::BlendFactor factor, bool is_alpha) noexcept;
    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param op `op` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_BLEND_OP to_d3d12(rhi::BlendOp op) noexcept;
    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param mask `mask` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 write mask representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] u8 to_d3d12_write_mask(rhi::ColorWriteMask mask) noexcept;
    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param topology `topology` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 topology type representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_PRIMITIVE_TOPOLOGY_TYPE to_d3d12_topology_type(rhi::PrimitiveTopology topology) noexcept;
    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param topology `topology` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 topology representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D_PRIMITIVE_TOPOLOGY to_d3d12_topology(rhi::PrimitiveTopology topology) noexcept;


    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param queue Queue used or affected by the operation.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_COMMAND_LIST_TYPE to_d3d12(rhi::QueueClass queue) noexcept;
    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param type Type value to inspect, select, or convert.
    ///
    /// @return Returns the value converted to D3D12 query heap type representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_QUERY_HEAP_TYPE to_d3d12_query_heap_type(rhi::QueryType type) noexcept;
    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param type Type value to inspect, select, or convert.
    /// @param precise_occlusion `precise_occlusion` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 query type representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_QUERY_TYPE to_d3d12_query_type(rhi::QueryType type, bool precise_occlusion) noexcept;


    /// Computes the query result bytes required by the supplied values.
    ///
    /// @param type Type value to inspect, select, or convert.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note This function does not throw exceptions.
    [[nodiscard]] u64 query_result_bytes(rhi::QueryType type) noexcept;


    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param stages `stages` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 sync representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_BARRIER_SYNC to_d3d12_sync(rhi::PipelineStage stages) noexcept;
    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param access `access` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 access representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_BARRIER_ACCESS to_d3d12_access(rhi::AccessFlags access) noexcept;
    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param layout `layout` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 layout representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_BARRIER_LAYOUT to_d3d12_layout(rhi::TextureLayout layout) noexcept;


    /// Converts the value to legacy texture state representation.
    ///
    /// @param layout `layout` value used by the operation.
    ///
    /// @return Returns the value converted to legacy texture state representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_RESOURCE_STATES to_legacy_texture_state(rhi::TextureLayout layout) noexcept;

    /// Converts the value to legacy buffer state representation.
    ///
    /// @param access `access` value used by the operation.
    ///
    /// @return Returns the value converted to legacy buffer state representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_RESOURCE_STATES to_legacy_buffer_state(rhi::AccessFlags access) noexcept;


    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param flags Flags controlling optional behavior.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS to_d3d12(
        rhi::AccelerationStructureBuildFlags flags) noexcept;
    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param flags Flags controlling optional behavior.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_RAYTRACING_GEOMETRY_FLAGS to_d3d12(rhi::AccelerationStructureGeometryFlags flags) noexcept;


    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param stages `stages` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 visibility representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] D3D12_SHADER_VISIBILITY to_d3d12_visibility(rhi::ShaderStage stages) noexcept;

} // namespace SFT::D3D12
