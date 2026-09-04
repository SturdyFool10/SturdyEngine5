#pragma once

#include <Foundation/Foundation.hpp>

#include <RHI/Types.hpp>
#include <RHI/Resources.hpp>
#include <RHI/Pipeline.hpp>
#include <RHI/Binding.hpp>
#include <RHI/Command.hpp>
#include <RHI/Swapchain.hpp>

#include <webgpu/webgpu.h>

namespace SFT::Core::WebGpu {

    /// Converts an RHI texture format to its WebGPU equivalent.
    ///
    /// Returns `WGPUTextureFormat_Undefined` for a format WebGPU has no equivalent of, which every
    /// caller treats as "reject this resource" rather than substituting something close — silently
    /// swapping a format changes what shaders read.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUTextureFormat to_wgpu(RHI::Format format) noexcept;

    /// Converts a WebGPU texture format back to the RHI enum, for reporting what a surface chose.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to the RHI representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] RHI::Format from_wgpu(WGPUTextureFormat format) noexcept;

    /// Returns the size in bytes of one texel of `format`, or of one 4x4 block for a
    /// block-compressed format.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value produced by the operation, or 0 for a format with no known size.
    /// @note This function does not throw exceptions.
    [[nodiscard]] u32 format_element_bytes(RHI::Format format) noexcept;

    /// Returns the edge length in texels of one addressable element of `format`: 4 for a
    /// block-compressed format and 1 for every other.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] u32 format_block_extent(RHI::Format format) noexcept;

    /// Reports whether WebGPU allows a texture of `format` to carry `TextureUsage::Storage`.
    ///
    /// WebGPU permits storage bindings on a fixed list of formats that is much shorter than
    /// Vulkan's, and is not driver-dependent: the two-channel 16-bit formats this engine uses for
    /// the G-buffer normal and motion targets are excluded outright, as are every sRGB, packed, and
    /// depth format. A texture asking for a storage binding on any other format is rejected by
    /// Dawn at creation, so `create_texture` consults this and drops the usage instead.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool format_supports_storage_binding(RHI::Format format) noexcept;

    /// Converts RHI buffer usage flags to WebGPU's.
    ///
    /// @param usage `usage` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUBufferUsage to_wgpu(RHI::BufferUsage usage) noexcept;

    /// Converts RHI texture usage flags to WebGPU's.
    ///
    /// @param usage `usage` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUTextureUsage to_wgpu(RHI::TextureUsage usage) noexcept;

    /// Converts an RHI texture dimension to WebGPU's.
    ///
    /// @param dimension `dimension` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUTextureDimension to_wgpu(RHI::TextureDimension dimension) noexcept;

    /// Converts an RHI texture view type to WebGPU's.
    ///
    /// @param view_type `view_type` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUTextureViewDimension to_wgpu(RHI::TextureViewType view_type) noexcept;

    /// Converts an RHI address mode to WebGPU's.
    ///
    /// @param mode `mode` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUAddressMode to_wgpu(RHI::AddressMode mode) noexcept;

    /// Converts an RHI filter to WebGPU's.
    ///
    /// @param filter `filter` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUFilterMode to_wgpu(RHI::Filter filter) noexcept;

    /// Converts an RHI mipmap mode to WebGPU's.
    ///
    /// @param mode `mode` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUMipmapFilterMode to_wgpu(RHI::MipmapMode mode) noexcept;

    /// Converts an RHI comparison to WebGPU's.
    ///
    /// @param op `op` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUCompareFunction to_wgpu(RHI::CompareOp op) noexcept;

    /// Converts an RHI index format to WebGPU's.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUIndexFormat to_wgpu(RHI::IndexFormat format) noexcept;

    /// Converts an RHI primitive topology to WebGPU's.
    ///
    /// @param topology `topology` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUPrimitiveTopology to_wgpu(RHI::PrimitiveTopology topology) noexcept;

    /// Converts an RHI cull mode to WebGPU's.
    ///
    /// @param mode `mode` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUCullMode to_wgpu(RHI::CullMode mode) noexcept;

    /// Converts an RHI winding order to WebGPU's front-face enum.
    ///
    /// @param face `face` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUFrontFace to_wgpu(RHI::FrontFace face) noexcept;

    /// Converts an RHI blend factor to WebGPU's.
    ///
    /// @param factor `factor` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUBlendFactor to_wgpu(RHI::BlendFactor factor) noexcept;

    /// Converts an RHI blend operation to WebGPU's.
    ///
    /// @param op `op` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUBlendOperation to_wgpu(RHI::BlendOp op) noexcept;

    /// Converts an RHI stencil operation to WebGPU's.
    ///
    /// @param op `op` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUStencilOperation to_wgpu(RHI::StencilOp op) noexcept;

    /// Converts an RHI vertex attribute format to WebGPU's.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUVertexFormat to_wgpu(RHI::VertexFormat format) noexcept;

    /// Converts an RHI vertex step mode to WebGPU's.
    ///
    /// @param rate `rate` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUVertexStepMode to_wgpu(RHI::VertexStepMode rate) noexcept;

    /// Converts an RHI load action to WebGPU's.
    ///
    /// @param op `op` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPULoadOp to_wgpu(RHI::LoadOp op) noexcept;

    /// Converts an RHI store action to WebGPU's.
    ///
    /// @param op `op` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUStoreOp to_wgpu(RHI::StoreOp op) noexcept;

    /// Converts RHI shader stage flags to WebGPU's.
    ///
    /// @param stages `stages` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUShaderStage to_wgpu(RHI::ShaderStage stages) noexcept;

    /// Converts an RHI present mode to a WebGPU surface present mode.
    ///
    /// @param mode `mode` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WGPUPresentMode to_wgpu(RHI::PresentMode mode) noexcept;

    /// Returns the number of bytes one texel of `format` occupies, or 0 for a block-compressed or
    /// unsupported format.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the requested count or size.
    /// @note This function does not throw exceptions.
    [[nodiscard]] u32 format_texel_bytes(RHI::Format format) noexcept;

} // namespace SFT::Core::WebGpu
