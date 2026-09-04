#include <Core/WebGPU/RHI/WebGpuConvert.hpp>

namespace SFT::Core::WebGpu {

    /// Converts an RHI texture format to its WebGPU equivalent.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUTextureFormat to_wgpu(RHI::Format format) noexcept {
        switch (format) {
            case RHI::Format::Undefined: return WGPUTextureFormat_Undefined;
            case RHI::Format::R8Unorm: return WGPUTextureFormat_R8Unorm;
            case RHI::Format::R8Snorm: return WGPUTextureFormat_R8Snorm;
            case RHI::Format::R8Uint: return WGPUTextureFormat_R8Uint;
            case RHI::Format::R8Sint: return WGPUTextureFormat_R8Sint;
            case RHI::Format::RG8Unorm: return WGPUTextureFormat_RG8Unorm;
            case RHI::Format::RG8Snorm: return WGPUTextureFormat_RG8Snorm;
            case RHI::Format::RG8Uint: return WGPUTextureFormat_RG8Uint;
            case RHI::Format::RG8Sint: return WGPUTextureFormat_RG8Sint;
            case RHI::Format::RGBA8Unorm: return WGPUTextureFormat_RGBA8Unorm;
            case RHI::Format::RGBA8UnormSrgb: return WGPUTextureFormat_RGBA8UnormSrgb;
            case RHI::Format::RGBA8Snorm: return WGPUTextureFormat_RGBA8Snorm;
            case RHI::Format::RGBA8Uint: return WGPUTextureFormat_RGBA8Uint;
            case RHI::Format::RGBA8Sint: return WGPUTextureFormat_RGBA8Sint;
            case RHI::Format::BGRA8Unorm: return WGPUTextureFormat_BGRA8Unorm;
            case RHI::Format::BGRA8UnormSrgb: return WGPUTextureFormat_BGRA8UnormSrgb;
            case RHI::Format::RGB10A2Unorm: return WGPUTextureFormat_RGB10A2Unorm;
            case RHI::Format::RG11B10Float: return WGPUTextureFormat_RG11B10Ufloat;
            case RHI::Format::R16Uint: return WGPUTextureFormat_R16Uint;
            case RHI::Format::R16Sint: return WGPUTextureFormat_R16Sint;
            case RHI::Format::R16Float: return WGPUTextureFormat_R16Float;
            case RHI::Format::RG16Uint: return WGPUTextureFormat_RG16Uint;
            case RHI::Format::RG16Sint: return WGPUTextureFormat_RG16Sint;
            case RHI::Format::RG16Float: return WGPUTextureFormat_RG16Float;
            case RHI::Format::RGBA16Uint: return WGPUTextureFormat_RGBA16Uint;
            case RHI::Format::RGBA16Sint: return WGPUTextureFormat_RGBA16Sint;
            case RHI::Format::RGBA16Float: return WGPUTextureFormat_RGBA16Float;
            case RHI::Format::R32Uint: return WGPUTextureFormat_R32Uint;
            case RHI::Format::R32Sint: return WGPUTextureFormat_R32Sint;
            case RHI::Format::R32Float: return WGPUTextureFormat_R32Float;
            case RHI::Format::RG32Uint: return WGPUTextureFormat_RG32Uint;
            case RHI::Format::RG32Sint: return WGPUTextureFormat_RG32Sint;
            case RHI::Format::RG32Float: return WGPUTextureFormat_RG32Float;
            case RHI::Format::RGBA32Uint: return WGPUTextureFormat_RGBA32Uint;
            case RHI::Format::RGBA32Sint: return WGPUTextureFormat_RGBA32Sint;
            case RHI::Format::RGBA32Float: return WGPUTextureFormat_RGBA32Float;
            case RHI::Format::D16Unorm: return WGPUTextureFormat_Depth16Unorm;
            case RHI::Format::D24UnormS8Uint: return WGPUTextureFormat_Depth24PlusStencil8;
            case RHI::Format::D32Float: return WGPUTextureFormat_Depth32Float;
            case RHI::Format::D32FloatS8Uint: return WGPUTextureFormat_Depth32FloatStencil8;
            // BC formats are an optional WebGPU feature (texture-compression-bc), requested at
            // device creation; the format enum itself is always defined.
            case RHI::Format::BC1Unorm: return WGPUTextureFormat_BC1RGBAUnorm;
            case RHI::Format::BC1UnormSrgb: return WGPUTextureFormat_BC1RGBAUnormSrgb;
            case RHI::Format::BC3Unorm: return WGPUTextureFormat_BC3RGBAUnorm;
            case RHI::Format::BC3UnormSrgb: return WGPUTextureFormat_BC3RGBAUnormSrgb;
            case RHI::Format::BC4Unorm: return WGPUTextureFormat_BC4RUnorm;
            case RHI::Format::BC5Unorm: return WGPUTextureFormat_BC5RGUnorm;
            case RHI::Format::BC7Unorm: return WGPUTextureFormat_BC7RGBAUnorm;
            case RHI::Format::BC7UnormSrgb: return WGPUTextureFormat_BC7RGBAUnormSrgb;
        }
        return WGPUTextureFormat_Undefined;
    }

    /// Converts a WebGPU texture format back to the RHI enum.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to the RHI representation.
    /// @note This function does not throw exceptions.
    RHI::Format from_wgpu(WGPUTextureFormat format) noexcept {
        switch (format) {
            case WGPUTextureFormat_R8Unorm: return RHI::Format::R8Unorm;
            case WGPUTextureFormat_RGBA8Unorm: return RHI::Format::RGBA8Unorm;
            case WGPUTextureFormat_RGBA8UnormSrgb: return RHI::Format::RGBA8UnormSrgb;
            case WGPUTextureFormat_BGRA8Unorm: return RHI::Format::BGRA8Unorm;
            case WGPUTextureFormat_BGRA8UnormSrgb: return RHI::Format::BGRA8UnormSrgb;
            case WGPUTextureFormat_RGB10A2Unorm: return RHI::Format::RGB10A2Unorm;
            case WGPUTextureFormat_RG11B10Ufloat: return RHI::Format::RG11B10Float;
            case WGPUTextureFormat_RGBA16Float: return RHI::Format::RGBA16Float;
            case WGPUTextureFormat_RGBA32Float: return RHI::Format::RGBA32Float;
            case WGPUTextureFormat_Depth32Float: return RHI::Format::D32Float;
            case WGPUTextureFormat_Depth24PlusStencil8: return RHI::Format::D24UnormS8Uint;
            default:
                // Only the formats a surface can realistically report are mapped back; anything
                // else is a format this engine never asked for in the first place.
                return RHI::Format::Undefined;
        }
    }

    /// Returns the size in bytes of one texel of `format`, or of one 4x4 block for a
    /// block-compressed format.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value produced by the operation, or 0 for a format with no known size.
    /// @note This function does not throw exceptions.
    u32 format_element_bytes(RHI::Format format) noexcept {
        switch (format) {
            case RHI::Format::R8Unorm:
            case RHI::Format::R8Snorm:
            case RHI::Format::R8Uint:
            case RHI::Format::R8Sint:
                return 1;

            case RHI::Format::RG8Unorm:
            case RHI::Format::RG8Snorm:
            case RHI::Format::RG8Uint:
            case RHI::Format::RG8Sint:
            case RHI::Format::R16Uint:
            case RHI::Format::R16Sint:
            case RHI::Format::R16Float:
            case RHI::Format::D16Unorm:
                return 2;

            case RHI::Format::RGBA8Unorm:
            case RHI::Format::RGBA8UnormSrgb:
            case RHI::Format::RGBA8Snorm:
            case RHI::Format::RGBA8Uint:
            case RHI::Format::RGBA8Sint:
            case RHI::Format::BGRA8Unorm:
            case RHI::Format::BGRA8UnormSrgb:
            case RHI::Format::RGB10A2Unorm:
            case RHI::Format::RG11B10Float:
            case RHI::Format::RG16Uint:
            case RHI::Format::RG16Sint:
            case RHI::Format::RG16Float:
            case RHI::Format::R32Uint:
            case RHI::Format::R32Sint:
            case RHI::Format::R32Float:
            case RHI::Format::D24UnormS8Uint:
            case RHI::Format::D32Float:
                return 4;

            case RHI::Format::RGBA16Uint:
            case RHI::Format::RGBA16Sint:
            case RHI::Format::RGBA16Float:
            case RHI::Format::RG32Uint:
            case RHI::Format::RG32Sint:
            case RHI::Format::RG32Float:
            case RHI::Format::D32FloatS8Uint:
                return 8;

            case RHI::Format::RGBA32Uint:
            case RHI::Format::RGBA32Sint:
            case RHI::Format::RGBA32Float:
                return 16;

            case RHI::Format::BC1Unorm:
            case RHI::Format::BC1UnormSrgb:
            case RHI::Format::BC4Unorm:
                return 8;
            case RHI::Format::BC3Unorm:
            case RHI::Format::BC3UnormSrgb:
            case RHI::Format::BC5Unorm:
            case RHI::Format::BC7Unorm:
            case RHI::Format::BC7UnormSrgb:
                return 16;

            case RHI::Format::Undefined:
                return 0;
        }
        return 0;
    }

    /// Returns the edge length in texels of one addressable element of `format`: 4 for a
    /// block-compressed format and 1 for every other.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    u32 format_block_extent(RHI::Format format) noexcept {
        return RHI::format_is_block_compressed(format) ? 4u : 1u;
    }

    /// Reports whether WebGPU allows a texture of `format` to carry `TextureUsage::Storage`.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool format_supports_storage_binding(RHI::Format format) noexcept {
        // WebGPU's storage-capable set, from the "plain color formats" table in the spec. It is
        // fixed by the API rather than queried from the driver, so this list is exhaustive and
        // needs no capability probe. BGRA8Unorm is deliberately absent: it is storage-capable only
        // when the optional bgra8unorm-storage feature is enabled, which this backend does not
        // request.
        switch (format) {
            case RHI::Format::R8Unorm:
            case RHI::Format::R8Snorm:
            case RHI::Format::R8Uint:
            case RHI::Format::R8Sint:
            case RHI::Format::RG8Unorm:
            case RHI::Format::RG8Snorm:
            case RHI::Format::RG8Uint:
            case RHI::Format::RG8Sint:
            case RHI::Format::RGBA8Unorm:
            case RHI::Format::RGBA8Snorm:
            case RHI::Format::RGBA8Uint:
            case RHI::Format::RGBA8Sint:
            case RHI::Format::R32Uint:
            case RHI::Format::R32Sint:
            case RHI::Format::R32Float:
            case RHI::Format::RG32Uint:
            case RHI::Format::RG32Sint:
            case RHI::Format::RG32Float:
            case RHI::Format::RGBA16Uint:
            case RHI::Format::RGBA16Sint:
            case RHI::Format::RGBA16Float:
            case RHI::Format::RGBA32Uint:
            case RHI::Format::RGBA32Sint:
            case RHI::Format::RGBA32Float:
                return true;
            default:
                return false;
        }
    }

    /// Converts RHI buffer usage flags to WebGPU's.
    ///
    /// @param usage `usage` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUBufferUsage to_wgpu(RHI::BufferUsage usage) noexcept {
        WGPUBufferUsage result = WGPUBufferUsage_None;
        const auto has = [usage](RHI::BufferUsage bit) {
            return (static_cast<u32>(usage) & static_cast<u32>(bit)) != 0;
        };
        if (has(RHI::BufferUsage::TransferSrc)) result |= WGPUBufferUsage_CopySrc;
        if (has(RHI::BufferUsage::TransferDst)) result |= WGPUBufferUsage_CopyDst;
        if (has(RHI::BufferUsage::Vertex)) result |= WGPUBufferUsage_Vertex;
        if (has(RHI::BufferUsage::Index)) result |= WGPUBufferUsage_Index;
        if (has(RHI::BufferUsage::Uniform)) result |= WGPUBufferUsage_Uniform;
        if (has(RHI::BufferUsage::Storage)) result |= WGPUBufferUsage_Storage;
        if (has(RHI::BufferUsage::Indirect)) result |= WGPUBufferUsage_Indirect;
        // ShaderBindingTable and the three AccelerationStructure* usages have no WebGPU
        // counterpart at all -- WebGPU has no ray tracing. A buffer requesting one is rejected
        // upstream in create_buffer rather than being silently created without it.
        return result;
    }

    /// Converts RHI texture usage flags to WebGPU's.
    ///
    /// @param usage `usage` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUTextureUsage to_wgpu(RHI::TextureUsage usage) noexcept {
        WGPUTextureUsage result = WGPUTextureUsage_None;
        const auto has = [usage](RHI::TextureUsage bit) {
            return (static_cast<u32>(usage) & static_cast<u32>(bit)) != 0;
        };
        if (has(RHI::TextureUsage::TransferSrc)) result |= WGPUTextureUsage_CopySrc;
        if (has(RHI::TextureUsage::TransferDst)) result |= WGPUTextureUsage_CopyDst;
        if (has(RHI::TextureUsage::Sampled)) result |= WGPUTextureUsage_TextureBinding;
        if (has(RHI::TextureUsage::Storage)) result |= WGPUTextureUsage_StorageBinding;
        if (has(RHI::TextureUsage::ColorAttachment) || has(RHI::TextureUsage::DepthStencilAttachment)) {
            result |= WGPUTextureUsage_RenderAttachment;
        }
        // TransientAttachment is a memory-residency hint (Vulkan's LAZILY_ALLOCATED); WebGPU has no
        // way to express it, and ignoring it only costs memory, never correctness.
        return result;
    }

    /// Converts an RHI texture dimension to WebGPU's.
    ///
    /// @param dimension `dimension` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUTextureDimension to_wgpu(RHI::TextureDimension dimension) noexcept {
        switch (dimension) {
            case RHI::TextureDimension::Dim1D: return WGPUTextureDimension_1D;
            case RHI::TextureDimension::Dim2D: return WGPUTextureDimension_2D;
            case RHI::TextureDimension::Dim3D: return WGPUTextureDimension_3D;
        }
        return WGPUTextureDimension_2D;
    }

    /// Converts an RHI texture view type to WebGPU's.
    ///
    /// @param view_type `view_type` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUTextureViewDimension to_wgpu(RHI::TextureViewType view_type) noexcept {
        switch (view_type) {
            case RHI::TextureViewType::View1D: return WGPUTextureViewDimension_1D;
            case RHI::TextureViewType::View2D: return WGPUTextureViewDimension_2D;
            case RHI::TextureViewType::View2DArray: return WGPUTextureViewDimension_2DArray;
            case RHI::TextureViewType::ViewCube: return WGPUTextureViewDimension_Cube;
            case RHI::TextureViewType::ViewCubeArray: return WGPUTextureViewDimension_CubeArray;
            case RHI::TextureViewType::View3D: return WGPUTextureViewDimension_3D;
        }
        return WGPUTextureViewDimension_2D;
    }

    /// Converts an RHI address mode to WebGPU's.
    ///
    /// @param mode `mode` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUAddressMode to_wgpu(RHI::AddressMode mode) noexcept {
        switch (mode) {
            case RHI::AddressMode::Repeat: return WGPUAddressMode_Repeat;
            case RHI::AddressMode::MirroredRepeat: return WGPUAddressMode_MirrorRepeat;
            case RHI::AddressMode::ClampToEdge: return WGPUAddressMode_ClampToEdge;
            // WebGPU has no border-colour addressing at all. Clamping to edge is the closest
            // behaviour and the substitution every WebGPU port makes; it differs only outside
            // [0, 1], where a border sampler would have returned the border colour.
            case RHI::AddressMode::ClampToBorder: return WGPUAddressMode_ClampToEdge;
        }
        return WGPUAddressMode_Repeat;
    }

    /// Converts an RHI filter to WebGPU's.
    ///
    /// @param filter `filter` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUFilterMode to_wgpu(RHI::Filter filter) noexcept {
        return filter == RHI::Filter::Linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    }

    /// Converts an RHI mipmap mode to WebGPU's.
    ///
    /// @param mode `mode` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUMipmapFilterMode to_wgpu(RHI::MipmapMode mode) noexcept {
        return mode == RHI::MipmapMode::Linear ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
    }

    /// Converts an RHI comparison to WebGPU's.
    ///
    /// @param op `op` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUCompareFunction to_wgpu(RHI::CompareOp op) noexcept {
        switch (op) {
            case RHI::CompareOp::Never: return WGPUCompareFunction_Never;
            case RHI::CompareOp::Less: return WGPUCompareFunction_Less;
            case RHI::CompareOp::Equal: return WGPUCompareFunction_Equal;
            case RHI::CompareOp::LessEqual: return WGPUCompareFunction_LessEqual;
            case RHI::CompareOp::Greater: return WGPUCompareFunction_Greater;
            case RHI::CompareOp::NotEqual: return WGPUCompareFunction_NotEqual;
            case RHI::CompareOp::GreaterEqual: return WGPUCompareFunction_GreaterEqual;
            case RHI::CompareOp::Always: return WGPUCompareFunction_Always;
        }
        return WGPUCompareFunction_Always;
    }

    /// Converts an RHI index format to WebGPU's.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUIndexFormat to_wgpu(RHI::IndexFormat format) noexcept {
        return format == RHI::IndexFormat::Uint16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32;
    }

    /// Converts an RHI primitive topology to WebGPU's.
    ///
    /// @param topology `topology` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUPrimitiveTopology to_wgpu(RHI::PrimitiveTopology topology) noexcept {
        switch (topology) {
            case RHI::PrimitiveTopology::PointList: return WGPUPrimitiveTopology_PointList;
            case RHI::PrimitiveTopology::LineList: return WGPUPrimitiveTopology_LineList;
            case RHI::PrimitiveTopology::LineStrip: return WGPUPrimitiveTopology_LineStrip;
            case RHI::PrimitiveTopology::TriangleList: return WGPUPrimitiveTopology_TriangleList;
            case RHI::PrimitiveTopology::TriangleStrip: return WGPUPrimitiveTopology_TriangleStrip;
        }
        return WGPUPrimitiveTopology_TriangleList;
    }

    /// Converts an RHI cull mode to WebGPU's.
    ///
    /// @param mode `mode` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUCullMode to_wgpu(RHI::CullMode mode) noexcept {
        switch (mode) {
            case RHI::CullMode::None: return WGPUCullMode_None;
            case RHI::CullMode::Front: return WGPUCullMode_Front;
            case RHI::CullMode::Back: return WGPUCullMode_Back;
        }
        return WGPUCullMode_None;
    }

    /// Converts an RHI winding order to WebGPU's front-face enum.
    ///
    /// @param face `face` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUFrontFace to_wgpu(RHI::FrontFace face) noexcept {
        return face == RHI::FrontFace::Clockwise ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
    }

    /// Converts an RHI blend factor to WebGPU's.
    ///
    /// @param factor `factor` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUBlendFactor to_wgpu(RHI::BlendFactor factor) noexcept {
        switch (factor) {
            case RHI::BlendFactor::Zero: return WGPUBlendFactor_Zero;
            case RHI::BlendFactor::One: return WGPUBlendFactor_One;
            case RHI::BlendFactor::SrcColor: return WGPUBlendFactor_Src;
            case RHI::BlendFactor::OneMinusSrcColor: return WGPUBlendFactor_OneMinusSrc;
            case RHI::BlendFactor::DstColor: return WGPUBlendFactor_Dst;
            case RHI::BlendFactor::OneMinusDstColor: return WGPUBlendFactor_OneMinusDst;
            case RHI::BlendFactor::SrcAlpha: return WGPUBlendFactor_SrcAlpha;
            case RHI::BlendFactor::OneMinusSrcAlpha: return WGPUBlendFactor_OneMinusSrcAlpha;
            case RHI::BlendFactor::DstAlpha: return WGPUBlendFactor_DstAlpha;
            case RHI::BlendFactor::OneMinusDstAlpha: return WGPUBlendFactor_OneMinusDstAlpha;
            case RHI::BlendFactor::ConstantColor: return WGPUBlendFactor_Constant;
            case RHI::BlendFactor::OneMinusConstantColor: return WGPUBlendFactor_OneMinusConstant;
            case RHI::BlendFactor::SrcAlphaSaturated: return WGPUBlendFactor_SrcAlphaSaturated;
        }
        return WGPUBlendFactor_One;
    }

    /// Converts an RHI blend operation to WebGPU's.
    ///
    /// @param op `op` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUBlendOperation to_wgpu(RHI::BlendOp op) noexcept {
        switch (op) {
            case RHI::BlendOp::Add: return WGPUBlendOperation_Add;
            case RHI::BlendOp::Subtract: return WGPUBlendOperation_Subtract;
            case RHI::BlendOp::ReverseSubtract: return WGPUBlendOperation_ReverseSubtract;
            case RHI::BlendOp::Min: return WGPUBlendOperation_Min;
            case RHI::BlendOp::Max: return WGPUBlendOperation_Max;
        }
        return WGPUBlendOperation_Add;
    }

    /// Converts an RHI stencil operation to WebGPU's.
    ///
    /// @param op `op` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUStencilOperation to_wgpu(RHI::StencilOp op) noexcept {
        switch (op) {
            case RHI::StencilOp::Keep: return WGPUStencilOperation_Keep;
            case RHI::StencilOp::Zero: return WGPUStencilOperation_Zero;
            case RHI::StencilOp::Replace: return WGPUStencilOperation_Replace;
            case RHI::StencilOp::IncrementClamp: return WGPUStencilOperation_IncrementClamp;
            case RHI::StencilOp::DecrementClamp: return WGPUStencilOperation_DecrementClamp;
            case RHI::StencilOp::Invert: return WGPUStencilOperation_Invert;
            case RHI::StencilOp::IncrementWrap: return WGPUStencilOperation_IncrementWrap;
            case RHI::StencilOp::DecrementWrap: return WGPUStencilOperation_DecrementWrap;
        }
        return WGPUStencilOperation_Keep;
    }

    /// Converts an RHI vertex attribute format to WebGPU's.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUVertexFormat to_wgpu(RHI::VertexFormat format) noexcept {
        switch (format) {
            case RHI::VertexFormat::Float32: return WGPUVertexFormat_Float32;
            case RHI::VertexFormat::Float32x2: return WGPUVertexFormat_Float32x2;
            case RHI::VertexFormat::Float32x3: return WGPUVertexFormat_Float32x3;
            case RHI::VertexFormat::Float32x4: return WGPUVertexFormat_Float32x4;
            case RHI::VertexFormat::Uint32: return WGPUVertexFormat_Uint32;
            case RHI::VertexFormat::Uint32x2: return WGPUVertexFormat_Uint32x2;
            case RHI::VertexFormat::Uint32x3: return WGPUVertexFormat_Uint32x3;
            case RHI::VertexFormat::Uint32x4: return WGPUVertexFormat_Uint32x4;
            case RHI::VertexFormat::Sint32: return WGPUVertexFormat_Sint32;
            case RHI::VertexFormat::Sint32x2: return WGPUVertexFormat_Sint32x2;
            case RHI::VertexFormat::Sint32x3: return WGPUVertexFormat_Sint32x3;
            case RHI::VertexFormat::Sint32x4: return WGPUVertexFormat_Sint32x4;
            case RHI::VertexFormat::Uint8x4Unorm: return WGPUVertexFormat_Unorm8x4;
            case RHI::VertexFormat::Sint8x4Norm: return WGPUVertexFormat_Snorm8x4;
            case RHI::VertexFormat::Uint16x2Unorm: return WGPUVertexFormat_Unorm16x2;
            case RHI::VertexFormat::Uint16x4Unorm: return WGPUVertexFormat_Unorm16x4;
            case RHI::VertexFormat::Float16x2: return WGPUVertexFormat_Float16x2;
            case RHI::VertexFormat::Float16x4: return WGPUVertexFormat_Float16x4;
        }
        return WGPUVertexFormat_Float32;
    }

    /// Converts an RHI vertex step mode to WebGPU's.
    ///
    /// @param rate `rate` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUVertexStepMode to_wgpu(RHI::VertexStepMode rate) noexcept {
        return rate == RHI::VertexStepMode::Instance ? WGPUVertexStepMode_Instance
                                                     : WGPUVertexStepMode_Vertex;
    }

    /// Converts an RHI load action to WebGPU's.
    ///
    /// @param op `op` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPULoadOp to_wgpu(RHI::LoadOp op) noexcept {
        switch (op) {
            case RHI::LoadOp::Load: return WGPULoadOp_Load;
            case RHI::LoadOp::Clear: return WGPULoadOp_Clear;
            // WebGPU has no "don't care" load. Clear is the safe substitution: Load would read
            // memory the caller just told us is undefined, which on a tiler is both slower and a
            // real source of garbage-in-the-first-frame bugs.
            case RHI::LoadOp::DontCare: return WGPULoadOp_Clear;
        }
        return WGPULoadOp_Clear;
    }

    /// Converts an RHI store action to WebGPU's.
    ///
    /// @param op `op` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUStoreOp to_wgpu(RHI::StoreOp op) noexcept {
        return op == RHI::StoreOp::Store ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
    }

    /// Converts RHI shader stage flags to WebGPU's.
    ///
    /// @param stages `stages` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUShaderStage to_wgpu(RHI::ShaderStage stages) noexcept {
        WGPUShaderStage result = WGPUShaderStage_None;
        const auto has = [stages](RHI::ShaderStage bit) {
            return (static_cast<u32>(stages) & static_cast<u32>(bit)) != 0;
        };
        if (has(RHI::ShaderStage::Vertex)) result |= WGPUShaderStage_Vertex;
        if (has(RHI::ShaderStage::Fragment)) result |= WGPUShaderStage_Fragment;
        if (has(RHI::ShaderStage::Compute)) result |= WGPUShaderStage_Compute;
        // Geometry, tessellation, task/mesh and every ray-tracing stage have no WebGPU equivalent;
        // a pipeline asking for one fails at creation rather than losing a stage quietly.
        return result;
    }

    /// Converts an RHI present mode to a WebGPU surface present mode.
    ///
    /// @param mode `mode` value used by the operation.
    ///
    /// @return Returns the value converted to the WebGPU representation.
    /// @note This function does not throw exceptions.
    WGPUPresentMode to_wgpu(RHI::PresentMode mode) noexcept {
        switch (mode) {
            case RHI::PresentMode::Fifo: return WGPUPresentMode_Fifo;
            // WebGPU has no relaxed-FIFO. Plain FIFO is the conservative substitution: it never
            // tears, it only fails to catch up after a late frame the way relaxed would.
            case RHI::PresentMode::FifoRelaxed: return WGPUPresentMode_Fifo;
            case RHI::PresentMode::Mailbox: return WGPUPresentMode_Mailbox;
            case RHI::PresentMode::Immediate: return WGPUPresentMode_Immediate;
            case RHI::PresentMode::FifoLatestReady: return WGPUPresentMode_Mailbox;
        }
        return WGPUPresentMode_Fifo;
    }

    /// Returns the number of bytes one texel of `format` occupies.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the requested count or size.
    /// @note This function does not throw exceptions.
    u32 format_texel_bytes(RHI::Format format) noexcept {
        switch (format) {
            case RHI::Format::R8Unorm:
            case RHI::Format::R8Snorm:
            case RHI::Format::R8Uint:
            case RHI::Format::R8Sint: return 1;
            case RHI::Format::RG8Unorm:
            case RHI::Format::RG8Snorm:
            case RHI::Format::RG8Uint:
            case RHI::Format::RG8Sint:
            case RHI::Format::R16Uint:
            case RHI::Format::R16Sint:
            case RHI::Format::R16Float:
            case RHI::Format::D16Unorm: return 2;
            case RHI::Format::RGBA8Unorm:
            case RHI::Format::RGBA8UnormSrgb:
            case RHI::Format::RGBA8Snorm:
            case RHI::Format::RGBA8Uint:
            case RHI::Format::RGBA8Sint:
            case RHI::Format::BGRA8Unorm:
            case RHI::Format::BGRA8UnormSrgb:
            case RHI::Format::RGB10A2Unorm:
            case RHI::Format::RG11B10Float:
            case RHI::Format::RG16Uint:
            case RHI::Format::RG16Sint:
            case RHI::Format::RG16Float:
            case RHI::Format::R32Uint:
            case RHI::Format::R32Sint:
            case RHI::Format::R32Float:
            case RHI::Format::D24UnormS8Uint:
            case RHI::Format::D32Float: return 4;
            case RHI::Format::RGBA16Uint:
            case RHI::Format::RGBA16Sint:
            case RHI::Format::RGBA16Float:
            case RHI::Format::RG32Uint:
            case RHI::Format::RG32Sint:
            case RHI::Format::RG32Float:
            case RHI::Format::D32FloatS8Uint: return 8;
            case RHI::Format::RGBA32Uint:
            case RHI::Format::RGBA32Sint:
            case RHI::Format::RGBA32Float: return 16;
            default:
                // Block-compressed and undefined formats have no per-texel size.
                return 0;
        }
    }

} // namespace SFT::Core::WebGpu
