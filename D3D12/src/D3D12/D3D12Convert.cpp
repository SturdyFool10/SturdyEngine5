#include <D3D12/D3D12Convert.hpp>

namespace SFT::D3D12 {

    using rhi::has_any;


    /// Converts the value to DXGI view format representation.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to DXGI view format representation.
    /// @note This function does not throw exceptions.
    DXGI_FORMAT to_dxgi_view_format(rhi::Format format) noexcept {
        switch (format) {
            case rhi::Format::Undefined:
                return DXGI_FORMAT_UNKNOWN;

            case rhi::Format::R8Unorm:
                return DXGI_FORMAT_R8_UNORM;
            case rhi::Format::R8Snorm:
                return DXGI_FORMAT_R8_SNORM;
            case rhi::Format::R8Uint:
                return DXGI_FORMAT_R8_UINT;
            case rhi::Format::R8Sint:
                return DXGI_FORMAT_R8_SINT;
            case rhi::Format::RG8Unorm:
                return DXGI_FORMAT_R8G8_UNORM;
            case rhi::Format::RG8Snorm:
                return DXGI_FORMAT_R8G8_SNORM;
            case rhi::Format::RG8Uint:
                return DXGI_FORMAT_R8G8_UINT;
            case rhi::Format::RG8Sint:
                return DXGI_FORMAT_R8G8_SINT;
            case rhi::Format::RGBA8Unorm:
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            case rhi::Format::RGBA8UnormSrgb:
                return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            case rhi::Format::RGBA8Snorm:
                return DXGI_FORMAT_R8G8B8A8_SNORM;
            case rhi::Format::RGBA8Uint:
                return DXGI_FORMAT_R8G8B8A8_UINT;
            case rhi::Format::RGBA8Sint:
                return DXGI_FORMAT_R8G8B8A8_SINT;
            case rhi::Format::BGRA8Unorm:
                return DXGI_FORMAT_B8G8R8A8_UNORM;
            case rhi::Format::BGRA8UnormSrgb:
                return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

            case rhi::Format::RGB10A2Unorm:
                return DXGI_FORMAT_R10G10B10A2_UNORM;
            case rhi::Format::RG11B10Float:
                return DXGI_FORMAT_R11G11B10_FLOAT;

            case rhi::Format::R16Uint:
                return DXGI_FORMAT_R16_UINT;
            case rhi::Format::R16Sint:
                return DXGI_FORMAT_R16_SINT;
            case rhi::Format::R16Float:
                return DXGI_FORMAT_R16_FLOAT;
            case rhi::Format::RG16Uint:
                return DXGI_FORMAT_R16G16_UINT;
            case rhi::Format::RG16Sint:
                return DXGI_FORMAT_R16G16_SINT;
            case rhi::Format::RG16Float:
                return DXGI_FORMAT_R16G16_FLOAT;
            case rhi::Format::RGBA16Uint:
                return DXGI_FORMAT_R16G16B16A16_UINT;
            case rhi::Format::RGBA16Sint:
                return DXGI_FORMAT_R16G16B16A16_SINT;
            case rhi::Format::RGBA16Float:
                return DXGI_FORMAT_R16G16B16A16_FLOAT;

            case rhi::Format::R32Uint:
                return DXGI_FORMAT_R32_UINT;
            case rhi::Format::R32Sint:
                return DXGI_FORMAT_R32_SINT;
            case rhi::Format::R32Float:
                return DXGI_FORMAT_R32_FLOAT;
            case rhi::Format::RG32Uint:
                return DXGI_FORMAT_R32G32_UINT;
            case rhi::Format::RG32Sint:
                return DXGI_FORMAT_R32G32_SINT;
            case rhi::Format::RG32Float:
                return DXGI_FORMAT_R32G32_FLOAT;
            case rhi::Format::RGBA32Uint:
                return DXGI_FORMAT_R32G32B32A32_UINT;
            case rhi::Format::RGBA32Sint:
                return DXGI_FORMAT_R32G32B32A32_SINT;
            case rhi::Format::RGBA32Float:
                return DXGI_FORMAT_R32G32B32A32_FLOAT;

            case rhi::Format::D16Unorm:
                return DXGI_FORMAT_D16_UNORM;
            case rhi::Format::D24UnormS8Uint:
                return DXGI_FORMAT_D24_UNORM_S8_UINT;
            case rhi::Format::D32Float:
                return DXGI_FORMAT_D32_FLOAT;
            case rhi::Format::D32FloatS8Uint:
                return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;

            case rhi::Format::BC1Unorm:
                return DXGI_FORMAT_BC1_UNORM;
            case rhi::Format::BC1UnormSrgb:
                return DXGI_FORMAT_BC1_UNORM_SRGB;
            case rhi::Format::BC3Unorm:
                return DXGI_FORMAT_BC3_UNORM;
            case rhi::Format::BC3UnormSrgb:
                return DXGI_FORMAT_BC3_UNORM_SRGB;
            case rhi::Format::BC4Unorm:
                return DXGI_FORMAT_BC4_UNORM;
            case rhi::Format::BC5Unorm:
                return DXGI_FORMAT_BC5_UNORM;
            case rhi::Format::BC7Unorm:
                return DXGI_FORMAT_BC7_UNORM;
            case rhi::Format::BC7UnormSrgb:
                return DXGI_FORMAT_BC7_UNORM_SRGB;
        }
        return DXGI_FORMAT_UNKNOWN;
    }

    /// Converts the value to DXGI typeless format representation.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to DXGI typeless format representation.
    /// @note This function does not throw exceptions.
    DXGI_FORMAT to_dxgi_typeless_format(rhi::Format format) noexcept {
        switch (format) {
            case rhi::Format::D16Unorm:
                return DXGI_FORMAT_R16_TYPELESS;
            case rhi::Format::D24UnormS8Uint:
                return DXGI_FORMAT_R24G8_TYPELESS;
            case rhi::Format::D32Float:
                return DXGI_FORMAT_R32_TYPELESS;
            case rhi::Format::D32FloatS8Uint:
                return DXGI_FORMAT_R32G8X24_TYPELESS;
            default:
                return DXGI_FORMAT_UNKNOWN;
        }
    }

    /// Converts the value to DXGI depth srv format representation.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to DXGI depth srv format representation.
    /// @note This function does not throw exceptions.
    DXGI_FORMAT to_dxgi_depth_srv_format(rhi::Format format) noexcept {
        switch (format) {
            case rhi::Format::D16Unorm:
                return DXGI_FORMAT_R16_UNORM;
            case rhi::Format::D24UnormS8Uint:
                return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            case rhi::Format::D32Float:
                return DXGI_FORMAT_R32_FLOAT;
            case rhi::Format::D32FloatS8Uint:
                return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            default:
                return DXGI_FORMAT_UNKNOWN;
        }
    }

    /// Converts the value to DXGI resource format representation.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    /// @param usage Usage flags or category applied to the resource.
    ///
    /// @return Returns the value converted to DXGI resource format representation.
    /// @note This function does not throw exceptions.
    DXGI_FORMAT to_dxgi_resource_format(rhi::Format format, rhi::TextureUsage usage) noexcept {


        if (rhi::format_is_depth_stencil(format) &&
            has_any(usage, rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage)) {
            const DXGI_FORMAT typeless = to_dxgi_typeless_format(format);
            if (typeless != DXGI_FORMAT_UNKNOWN) {
                return typeless;
            }
        }
        return to_dxgi_view_format(format);
    }

    /// Computes the format element bytes required by the supplied values.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    u32 format_element_bytes(rhi::Format format) noexcept {
        switch (format) {
            case rhi::Format::R8Unorm:
            case rhi::Format::R8Snorm:
            case rhi::Format::R8Uint:
            case rhi::Format::R8Sint:
                return 1;

            case rhi::Format::RG8Unorm:
            case rhi::Format::RG8Snorm:
            case rhi::Format::RG8Uint:
            case rhi::Format::RG8Sint:
            case rhi::Format::R16Uint:
            case rhi::Format::R16Sint:
            case rhi::Format::R16Float:
            case rhi::Format::D16Unorm:
                return 2;

            case rhi::Format::RGBA8Unorm:
            case rhi::Format::RGBA8UnormSrgb:
            case rhi::Format::RGBA8Snorm:
            case rhi::Format::RGBA8Uint:
            case rhi::Format::RGBA8Sint:
            case rhi::Format::BGRA8Unorm:
            case rhi::Format::BGRA8UnormSrgb:
            case rhi::Format::RGB10A2Unorm:
            case rhi::Format::RG11B10Float:
            case rhi::Format::RG16Uint:
            case rhi::Format::RG16Sint:
            case rhi::Format::RG16Float:
            case rhi::Format::R32Uint:
            case rhi::Format::R32Sint:
            case rhi::Format::R32Float:
            case rhi::Format::D24UnormS8Uint:
            case rhi::Format::D32Float:
                return 4;

            case rhi::Format::RGBA16Uint:
            case rhi::Format::RGBA16Sint:
            case rhi::Format::RGBA16Float:
            case rhi::Format::RG32Uint:
            case rhi::Format::RG32Sint:
            case rhi::Format::RG32Float:
            case rhi::Format::D32FloatS8Uint:
                return 8;

            case rhi::Format::RGBA32Uint:
            case rhi::Format::RGBA32Sint:
            case rhi::Format::RGBA32Float:
                return 16;


            case rhi::Format::BC1Unorm:
            case rhi::Format::BC1UnormSrgb:
            case rhi::Format::BC4Unorm:
                return 8;
            case rhi::Format::BC3Unorm:
            case rhi::Format::BC3UnormSrgb:
            case rhi::Format::BC5Unorm:
            case rhi::Format::BC7Unorm:
            case rhi::Format::BC7UnormSrgb:
                return 16;

            case rhi::Format::Undefined:
                return 0;
        }
        return 0;
    }

    /// Formats block extent using the supplied arguments and current state.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    u32 format_block_extent(rhi::Format format) noexcept {
        return rhi::format_is_block_compressed(format) ? 4u : 1u;
    }

    /// Converts the value to DXGI representation.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to DXGI representation.
    /// @note This function does not throw exceptions.
    DXGI_FORMAT to_dxgi(rhi::VertexFormat format) noexcept {
        switch (format) {
            case rhi::VertexFormat::Float32:
                return DXGI_FORMAT_R32_FLOAT;
            case rhi::VertexFormat::Float32x2:
                return DXGI_FORMAT_R32G32_FLOAT;
            case rhi::VertexFormat::Float32x3:
                return DXGI_FORMAT_R32G32B32_FLOAT;
            case rhi::VertexFormat::Float32x4:
                return DXGI_FORMAT_R32G32B32A32_FLOAT;
            case rhi::VertexFormat::Uint32:
                return DXGI_FORMAT_R32_UINT;
            case rhi::VertexFormat::Uint32x2:
                return DXGI_FORMAT_R32G32_UINT;
            case rhi::VertexFormat::Uint32x3:
                return DXGI_FORMAT_R32G32B32_UINT;
            case rhi::VertexFormat::Uint32x4:
                return DXGI_FORMAT_R32G32B32A32_UINT;
            case rhi::VertexFormat::Sint32:
                return DXGI_FORMAT_R32_SINT;
            case rhi::VertexFormat::Sint32x2:
                return DXGI_FORMAT_R32G32_SINT;
            case rhi::VertexFormat::Sint32x3:
                return DXGI_FORMAT_R32G32B32_SINT;
            case rhi::VertexFormat::Sint32x4:
                return DXGI_FORMAT_R32G32B32A32_SINT;
            case rhi::VertexFormat::Uint8x4Unorm:
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            case rhi::VertexFormat::Sint8x4Norm:
                return DXGI_FORMAT_R8G8B8A8_SNORM;
            case rhi::VertexFormat::Uint16x2Unorm:
                return DXGI_FORMAT_R16G16_UNORM;
            case rhi::VertexFormat::Uint16x4Unorm:
                return DXGI_FORMAT_R16G16B16A16_UNORM;
            case rhi::VertexFormat::Float16x2:
                return DXGI_FORMAT_R16G16_FLOAT;
            case rhi::VertexFormat::Float16x4:
                return DXGI_FORMAT_R16G16B16A16_FLOAT;
        }
        return DXGI_FORMAT_UNKNOWN;
    }

    /// Converts the value to DXGI representation.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to DXGI representation.
    /// @note This function does not throw exceptions.
    DXGI_FORMAT to_dxgi(rhi::IndexFormat format) noexcept {
        return format == rhi::IndexFormat::Uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    }

    /// Converts the value to DXGI color space representation.
    ///
    /// @param color_space `color_space` value used by the operation.
    /// @param out `out` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool to_dxgi_color_space(rhi::ColorSpace color_space, DXGI_COLOR_SPACE_TYPE &out) noexcept {
        switch (color_space) {
            case rhi::ColorSpace::SrgbNonlinear:
                out = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
                return true;
            case rhi::ColorSpace::Hdr10St2084:
                out = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                return true;
            case rhi::ColorSpace::ScrgbLinear:
                out = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
                return true;


            case rhi::ColorSpace::Hdr10Hlg:
            case rhi::ColorSpace::DisplayP3Nonlinear:
            case rhi::ColorSpace::Bt2020Linear:
                return false;


            case rhi::ColorSpace::DolbyVision:
            case rhi::ColorSpace::AdobeRgbLinear:
            case rhi::ColorSpace::AdobeRgbNonlinear:
            case rhi::ColorSpace::DisplayP3Linear:
                return false;
        }
        return false;
    }


    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param usage Usage flags or category applied to the resource.
    ///
    /// @return Returns the value converted to D3D12 resource flags representation.
    /// @note This function does not throw exceptions.
    D3D12_RESOURCE_FLAGS to_d3d12_resource_flags(rhi::BufferUsage usage) noexcept {
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;


        if (has_any(usage, rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationStructureScratch)) {
            flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
        return flags;
    }

    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param usage Usage flags or category applied to the resource.
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value converted to D3D12 resource flags representation.
    /// @note This function does not throw exceptions.
    D3D12_RESOURCE_FLAGS to_d3d12_resource_flags(rhi::TextureUsage usage, rhi::Format format) noexcept {
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
        if (has_any(usage, rhi::TextureUsage::ColorAttachment)) {
            flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        }
        if (has_any(usage, rhi::TextureUsage::DepthStencilAttachment) ||
            (rhi::format_is_depth_stencil(format) && has_any(usage, rhi::TextureUsage::TransientAttachment))) {
            flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;


            if (!has_any(usage, rhi::TextureUsage::Sampled)) {
                flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
            }
        }
        if (has_any(usage, rhi::TextureUsage::Storage)) {
            flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
        return flags;
    }

    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param memory `memory` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 heap type representation.
    /// @note This function does not throw exceptions.
    D3D12_HEAP_TYPE to_d3d12_heap_type(rhi::MemoryLocation memory) noexcept {
        switch (memory) {
            case rhi::MemoryLocation::DeviceLocal:
                return D3D12_HEAP_TYPE_DEFAULT;
            case rhi::MemoryLocation::HostUpload:
                return D3D12_HEAP_TYPE_UPLOAD;
            case rhi::MemoryLocation::HostReadback:
                return D3D12_HEAP_TYPE_READBACK;
        }
        return D3D12_HEAP_TYPE_DEFAULT;
    }

    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param dimension `dimension` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    D3D12_RESOURCE_DIMENSION to_d3d12(rhi::TextureDimension dimension) noexcept {
        switch (dimension) {
            case rhi::TextureDimension::Dim1D:
                return D3D12_RESOURCE_DIMENSION_TEXTURE1D;
            case rhi::TextureDimension::Dim2D:
                return D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            case rhi::TextureDimension::Dim3D:
                return D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        }
        return D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    }


    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param op `op` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    D3D12_COMPARISON_FUNC to_d3d12(rhi::CompareOp op) noexcept {
        switch (op) {
            case rhi::CompareOp::Never:
                return D3D12_COMPARISON_FUNC_NEVER;
            case rhi::CompareOp::Less:
                return D3D12_COMPARISON_FUNC_LESS;
            case rhi::CompareOp::Equal:
                return D3D12_COMPARISON_FUNC_EQUAL;
            case rhi::CompareOp::LessEqual:
                return D3D12_COMPARISON_FUNC_LESS_EQUAL;
            case rhi::CompareOp::Greater:
                return D3D12_COMPARISON_FUNC_GREATER;
            case rhi::CompareOp::NotEqual:
                return D3D12_COMPARISON_FUNC_NOT_EQUAL;
            case rhi::CompareOp::GreaterEqual:
                return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            case rhi::CompareOp::Always:
                return D3D12_COMPARISON_FUNC_ALWAYS;
        }
        return D3D12_COMPARISON_FUNC_ALWAYS;
    }

    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param mode Mode controlling how the operation is performed.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    D3D12_TEXTURE_ADDRESS_MODE to_d3d12(rhi::AddressMode mode) noexcept {
        switch (mode) {
            case rhi::AddressMode::Repeat:
                return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            case rhi::AddressMode::MirroredRepeat:
                return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
            case rhi::AddressMode::ClampToEdge:
                return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            case rhi::AddressMode::ClampToBorder:
                return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        }
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }

    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value converted to D3D12 filter representation.
    /// @note This function does not throw exceptions.
    D3D12_FILTER to_d3d12_filter(const rhi::SamplerDesc &desc) noexcept {


        const D3D12_FILTER_REDUCTION_TYPE reduction =
            desc.compare_enable ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON : D3D12_FILTER_REDUCTION_TYPE_STANDARD;
        if (desc.max_anisotropy > 1.0f) {
            return D3D12_ENCODE_ANISOTROPIC_FILTER(reduction);
        }
        const D3D12_FILTER_TYPE min_filter =
            desc.min_filter == rhi::Filter::Linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
        const D3D12_FILTER_TYPE mag_filter =
            desc.mag_filter == rhi::Filter::Linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
        const D3D12_FILTER_TYPE mip_filter =
            desc.mipmap_mode == rhi::MipmapMode::Linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
        return D3D12_ENCODE_BASIC_FILTER(min_filter, mag_filter, mip_filter, reduction);
    }

    /// Fills border color using the supplied arguments and current state.
    ///
    /// @param color `color` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void fill_border_color(rhi::BorderColor color, float out[4]) noexcept {
        switch (color) {
            case rhi::BorderColor::TransparentBlack:
                out[0] = out[1] = out[2] = out[3] = 0.0f;
                return;
            case rhi::BorderColor::OpaqueBlack:
                out[0] = out[1] = out[2] = 0.0f;
                out[3] = 1.0f;
                return;
            case rhi::BorderColor::OpaqueWhite:
                out[0] = out[1] = out[2] = out[3] = 1.0f;
                return;
        }
        out[0] = out[1] = out[2] = out[3] = 0.0f;
    }


    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param mode Mode controlling how the operation is performed.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    D3D12_FILL_MODE to_d3d12(rhi::PolygonMode mode) noexcept {


        return mode == rhi::PolygonMode::Fill ? D3D12_FILL_MODE_SOLID : D3D12_FILL_MODE_WIREFRAME;
    }

    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param mode Mode controlling how the operation is performed.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    D3D12_CULL_MODE to_d3d12(rhi::CullMode mode) noexcept {
        switch (mode) {
            case rhi::CullMode::None:
                return D3D12_CULL_MODE_NONE;
            case rhi::CullMode::Front:
                return D3D12_CULL_MODE_FRONT;
            case rhi::CullMode::Back:
                return D3D12_CULL_MODE_BACK;
        }
        return D3D12_CULL_MODE_NONE;
    }

    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param op `op` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    D3D12_STENCIL_OP to_d3d12(rhi::StencilOp op) noexcept {
        switch (op) {
            case rhi::StencilOp::Keep:
                return D3D12_STENCIL_OP_KEEP;
            case rhi::StencilOp::Zero:
                return D3D12_STENCIL_OP_ZERO;
            case rhi::StencilOp::Replace:
                return D3D12_STENCIL_OP_REPLACE;
            case rhi::StencilOp::IncrementClamp:
                return D3D12_STENCIL_OP_INCR_SAT;
            case rhi::StencilOp::DecrementClamp:
                return D3D12_STENCIL_OP_DECR_SAT;
            case rhi::StencilOp::Invert:
                return D3D12_STENCIL_OP_INVERT;
            case rhi::StencilOp::IncrementWrap:
                return D3D12_STENCIL_OP_INCR;
            case rhi::StencilOp::DecrementWrap:
                return D3D12_STENCIL_OP_DECR;
        }
        return D3D12_STENCIL_OP_KEEP;
    }

    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param factor `factor` value used by the operation.
    /// @param is_alpha `is_alpha` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 blend representation.
    /// @note This function does not throw exceptions.
    D3D12_BLEND to_d3d12_blend(rhi::BlendFactor factor, bool is_alpha) noexcept {


        switch (factor) {
            case rhi::BlendFactor::Zero:
                return D3D12_BLEND_ZERO;
            case rhi::BlendFactor::One:
                return D3D12_BLEND_ONE;
            case rhi::BlendFactor::SrcColor:
                return is_alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
            case rhi::BlendFactor::OneMinusSrcColor:
                return is_alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
            case rhi::BlendFactor::DstColor:
                return is_alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
            case rhi::BlendFactor::OneMinusDstColor:
                return is_alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
            case rhi::BlendFactor::SrcAlpha:
                return D3D12_BLEND_SRC_ALPHA;
            case rhi::BlendFactor::OneMinusSrcAlpha:
                return D3D12_BLEND_INV_SRC_ALPHA;
            case rhi::BlendFactor::DstAlpha:
                return D3D12_BLEND_DEST_ALPHA;
            case rhi::BlendFactor::OneMinusDstAlpha:
                return D3D12_BLEND_INV_DEST_ALPHA;
            case rhi::BlendFactor::ConstantColor:
                return D3D12_BLEND_BLEND_FACTOR;
            case rhi::BlendFactor::OneMinusConstantColor:
                return D3D12_BLEND_INV_BLEND_FACTOR;
            case rhi::BlendFactor::SrcAlphaSaturated:
                return D3D12_BLEND_SRC_ALPHA_SAT;
        }
        return D3D12_BLEND_ONE;
    }

    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param op `op` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    D3D12_BLEND_OP to_d3d12(rhi::BlendOp op) noexcept {
        switch (op) {
            case rhi::BlendOp::Add:
                return D3D12_BLEND_OP_ADD;
            case rhi::BlendOp::Subtract:
                return D3D12_BLEND_OP_SUBTRACT;
            case rhi::BlendOp::ReverseSubtract:
                return D3D12_BLEND_OP_REV_SUBTRACT;
            case rhi::BlendOp::Min:
                return D3D12_BLEND_OP_MIN;
            case rhi::BlendOp::Max:
                return D3D12_BLEND_OP_MAX;
        }
        return D3D12_BLEND_OP_ADD;
    }

    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param mask `mask` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 write mask representation.
    /// @note This function does not throw exceptions.
    u8 to_d3d12_write_mask(rhi::ColorWriteMask mask) noexcept {
        u8 result = 0;
        if (has_any(mask, rhi::ColorWriteMask::Red)) {
            result |= D3D12_COLOR_WRITE_ENABLE_RED;
        }
        if (has_any(mask, rhi::ColorWriteMask::Green)) {
            result |= D3D12_COLOR_WRITE_ENABLE_GREEN;
        }
        if (has_any(mask, rhi::ColorWriteMask::Blue)) {
            result |= D3D12_COLOR_WRITE_ENABLE_BLUE;
        }
        if (has_any(mask, rhi::ColorWriteMask::Alpha)) {
            result |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
        }
        return result;
    }

    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param topology `topology` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 topology type representation.
    /// @note This function does not throw exceptions.
    D3D12_PRIMITIVE_TOPOLOGY_TYPE to_d3d12_topology_type(rhi::PrimitiveTopology topology) noexcept {
        switch (topology) {
            case rhi::PrimitiveTopology::PointList:
                return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            case rhi::PrimitiveTopology::LineList:
            case rhi::PrimitiveTopology::LineStrip:
                return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            case rhi::PrimitiveTopology::TriangleList:
            case rhi::PrimitiveTopology::TriangleStrip:
                return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        }
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }

    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param topology `topology` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 topology representation.
    /// @note This function does not throw exceptions.
    D3D_PRIMITIVE_TOPOLOGY to_d3d12_topology(rhi::PrimitiveTopology topology) noexcept {
        switch (topology) {
            case rhi::PrimitiveTopology::PointList:
                return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            case rhi::PrimitiveTopology::LineList:
                return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            case rhi::PrimitiveTopology::LineStrip:
                return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            case rhi::PrimitiveTopology::TriangleList:
                return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            case rhi::PrimitiveTopology::TriangleStrip:
                return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        }
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }


    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param queue Queue used or affected by the operation.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    D3D12_COMMAND_LIST_TYPE to_d3d12(rhi::QueueClass queue) noexcept {
        switch (queue) {
            case rhi::QueueClass::Graphics:
                return D3D12_COMMAND_LIST_TYPE_DIRECT;
            case rhi::QueueClass::Compute:
                return D3D12_COMMAND_LIST_TYPE_COMPUTE;
            case rhi::QueueClass::Transfer:
                return D3D12_COMMAND_LIST_TYPE_COPY;
            case rhi::QueueClass::VideoDecode:
                return D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE;
            case rhi::QueueClass::VideoEncode:
                return D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE;


            case rhi::QueueClass::Sparse:
                return D3D12_COMMAND_LIST_TYPE_DIRECT;
        }
        return D3D12_COMMAND_LIST_TYPE_DIRECT;
    }

    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param type Type value to inspect, select, or convert.
    ///
    /// @return Returns the value converted to D3D12 query heap type representation.
    /// @note This function does not throw exceptions.
    D3D12_QUERY_HEAP_TYPE to_d3d12_query_heap_type(rhi::QueryType type) noexcept {
        switch (type) {
            case rhi::QueryType::Occlusion:
                return D3D12_QUERY_HEAP_TYPE_OCCLUSION;
            case rhi::QueryType::Timestamp:
                return D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            case rhi::QueryType::PipelineStatistics:
                return D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
        }
        return D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    }

    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param type Type value to inspect, select, or convert.
    /// @param precise_occlusion `precise_occlusion` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 query type representation.
    /// @note This function does not throw exceptions.
    D3D12_QUERY_TYPE to_d3d12_query_type(rhi::QueryType type, bool precise_occlusion) noexcept {
        switch (type) {
            case rhi::QueryType::Occlusion:
                return precise_occlusion ? D3D12_QUERY_TYPE_OCCLUSION : D3D12_QUERY_TYPE_BINARY_OCCLUSION;
            case rhi::QueryType::Timestamp:
                return D3D12_QUERY_TYPE_TIMESTAMP;
            case rhi::QueryType::PipelineStatistics:
                return D3D12_QUERY_TYPE_PIPELINE_STATISTICS;
        }
        return D3D12_QUERY_TYPE_TIMESTAMP;
    }

    /// Computes the query result bytes required by the supplied values.
    ///
    /// @param type Type value to inspect, select, or convert.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note This function does not throw exceptions.
    u64 query_result_bytes(rhi::QueryType type) noexcept {
        switch (type) {
            case rhi::QueryType::Occlusion:
            case rhi::QueryType::Timestamp:
                return sizeof(u64);
            case rhi::QueryType::PipelineStatistics:
                return sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
        }
        return sizeof(u64);
    }


    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param stages `stages` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 sync representation.
    /// @note This function does not throw exceptions.
    D3D12_BARRIER_SYNC to_d3d12_sync(rhi::PipelineStage stages) noexcept {
        if (stages == rhi::PipelineStage::None) {
            return D3D12_BARRIER_SYNC_NONE;
        }
        if (stages == rhi::PipelineStage::AllCommands) {
            return D3D12_BARRIER_SYNC_ALL;
        }

        D3D12_BARRIER_SYNC sync = D3D12_BARRIER_SYNC_NONE;
        const auto add = [&](rhi::PipelineStage bit, D3D12_BARRIER_SYNC mapped) {
            if (has_any(stages, bit)) {
                sync |= mapped;
            }
        };

        add(rhi::PipelineStage::DrawIndirect, D3D12_BARRIER_SYNC_EXECUTE_INDIRECT);


        add(rhi::PipelineStage::VertexInput,
            D3D12_BARRIER_SYNC_INDEX_INPUT | D3D12_BARRIER_SYNC_VERTEX_SHADING);
        add(rhi::PipelineStage::VertexShader, D3D12_BARRIER_SYNC_VERTEX_SHADING);
        add(rhi::PipelineStage::TessControlShader, D3D12_BARRIER_SYNC_VERTEX_SHADING);
        add(rhi::PipelineStage::TessEvalShader, D3D12_BARRIER_SYNC_VERTEX_SHADING);
        add(rhi::PipelineStage::GeometryShader, D3D12_BARRIER_SYNC_VERTEX_SHADING);
        add(rhi::PipelineStage::TaskShader, D3D12_BARRIER_SYNC_VERTEX_SHADING);
        add(rhi::PipelineStage::MeshShader, D3D12_BARRIER_SYNC_VERTEX_SHADING);
        add(rhi::PipelineStage::FragmentShader, D3D12_BARRIER_SYNC_PIXEL_SHADING);


        add(rhi::PipelineStage::EarlyFragmentTests, D3D12_BARRIER_SYNC_DEPTH_STENCIL);
        add(rhi::PipelineStage::LateFragmentTests, D3D12_BARRIER_SYNC_DEPTH_STENCIL);
        add(rhi::PipelineStage::ColorAttachmentOutput, D3D12_BARRIER_SYNC_RENDER_TARGET);
        add(rhi::PipelineStage::ComputeShader, D3D12_BARRIER_SYNC_COMPUTE_SHADING);


        add(rhi::PipelineStage::Transfer, D3D12_BARRIER_SYNC_COPY);
        add(rhi::PipelineStage::RayTracingShader, D3D12_BARRIER_SYNC_RAYTRACING);
        add(rhi::PipelineStage::AccelerationStructureBuild,
            D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE);


        return sync;
    }

    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param access `access` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 access representation.
    /// @note This function does not throw exceptions.
    D3D12_BARRIER_ACCESS to_d3d12_access(rhi::AccessFlags access) noexcept {
        if (access == rhi::AccessFlags::None) {


            return D3D12_BARRIER_ACCESS_COMMON;
        }

        D3D12_BARRIER_ACCESS result = D3D12_BARRIER_ACCESS_COMMON;
        const auto add = [&](rhi::AccessFlags bit, D3D12_BARRIER_ACCESS mapped) {
            if (has_any(access, bit)) {
                result |= mapped;
            }
        };

        add(rhi::AccessFlags::IndirectCommandRead, D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
        add(rhi::AccessFlags::IndexRead, D3D12_BARRIER_ACCESS_INDEX_BUFFER);
        add(rhi::AccessFlags::VertexAttributeRead, D3D12_BARRIER_ACCESS_VERTEX_BUFFER);
        add(rhi::AccessFlags::UniformRead, D3D12_BARRIER_ACCESS_CONSTANT_BUFFER);
        add(rhi::AccessFlags::ShaderRead, D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        add(rhi::AccessFlags::ShaderWrite, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
        add(rhi::AccessFlags::ColorAttachmentRead, D3D12_BARRIER_ACCESS_RENDER_TARGET);
        add(rhi::AccessFlags::ColorAttachmentWrite, D3D12_BARRIER_ACCESS_RENDER_TARGET);
        add(rhi::AccessFlags::DepthStencilAttachmentRead, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);
        add(rhi::AccessFlags::DepthStencilAttachmentWrite, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);
        add(rhi::AccessFlags::TransferRead, D3D12_BARRIER_ACCESS_COPY_SOURCE);
        add(rhi::AccessFlags::TransferWrite, D3D12_BARRIER_ACCESS_COPY_DEST);
        add(rhi::AccessFlags::AccelerationStructureRead, D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);
        add(rhi::AccessFlags::AccelerationStructureWrite, D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE);


        if (has_any(access, rhi::AccessFlags::MemoryRead | rhi::AccessFlags::MemoryWrite)) {
            return D3D12_BARRIER_ACCESS_COMMON;
        }


        return result;
    }

    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param layout `layout` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 layout representation.
    /// @note This function does not throw exceptions.
    D3D12_BARRIER_LAYOUT to_d3d12_layout(rhi::TextureLayout layout) noexcept {
        switch (layout) {
            case rhi::TextureLayout::Undefined:
                return D3D12_BARRIER_LAYOUT_UNDEFINED;
            case rhi::TextureLayout::General:
                return D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
            case rhi::TextureLayout::ColorAttachment:
                return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
            case rhi::TextureLayout::DepthStencilAttachment:
                return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
            case rhi::TextureLayout::DepthStencilReadOnly:
                return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ;
            case rhi::TextureLayout::ShaderReadOnly:
                return D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
            case rhi::TextureLayout::TransferSrc:
                return D3D12_BARRIER_LAYOUT_COPY_SOURCE;
            case rhi::TextureLayout::TransferDst:
                return D3D12_BARRIER_LAYOUT_COPY_DEST;

            case rhi::TextureLayout::Present:
                return D3D12_BARRIER_LAYOUT_PRESENT;
        }
        return D3D12_BARRIER_LAYOUT_COMMON;
    }


    /// Converts the value to legacy texture state representation.
    ///
    /// @param layout `layout` value used by the operation.
    ///
    /// @return Returns the value converted to legacy texture state representation.
    /// @note This function does not throw exceptions.
    D3D12_RESOURCE_STATES to_legacy_texture_state(rhi::TextureLayout layout) noexcept {
        switch (layout) {
            case rhi::TextureLayout::Undefined:
                return D3D12_RESOURCE_STATE_COMMON;
            case rhi::TextureLayout::General:
                return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            case rhi::TextureLayout::ColorAttachment:
                return D3D12_RESOURCE_STATE_RENDER_TARGET;
            case rhi::TextureLayout::DepthStencilAttachment:
                return D3D12_RESOURCE_STATE_DEPTH_WRITE;
            case rhi::TextureLayout::DepthStencilReadOnly:
                return D3D12_RESOURCE_STATE_DEPTH_READ;
            case rhi::TextureLayout::ShaderReadOnly:


                return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            case rhi::TextureLayout::TransferSrc:
                return D3D12_RESOURCE_STATE_COPY_SOURCE;
            case rhi::TextureLayout::TransferDst:
                return D3D12_RESOURCE_STATE_COPY_DEST;
            case rhi::TextureLayout::Present:
                return D3D12_RESOURCE_STATE_PRESENT;
        }
        return D3D12_RESOURCE_STATE_COMMON;
    }

    /// Converts the value to legacy buffer state representation.
    ///
    /// @param access `access` value used by the operation.
    ///
    /// @return Returns the value converted to legacy buffer state representation.
    /// @note This function does not throw exceptions.
    D3D12_RESOURCE_STATES to_legacy_buffer_state(rhi::AccessFlags access) noexcept {


        if (has_any(access, rhi::AccessFlags::ShaderWrite)) {
            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        if (has_any(access, rhi::AccessFlags::AccelerationStructureWrite | rhi::AccessFlags::AccelerationStructureRead)) {
            return D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
        }
        if (has_any(access, rhi::AccessFlags::TransferWrite)) {
            return D3D12_RESOURCE_STATE_COPY_DEST;
        }
        if (has_any(access, rhi::AccessFlags::TransferRead)) {
            return D3D12_RESOURCE_STATE_COPY_SOURCE;
        }
        if (has_any(access, rhi::AccessFlags::IndirectCommandRead)) {
            return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        }
        if (has_any(access, rhi::AccessFlags::IndexRead)) {
            return D3D12_RESOURCE_STATE_INDEX_BUFFER;
        }
        if (has_any(access, rhi::AccessFlags::VertexAttributeRead | rhi::AccessFlags::UniformRead)) {
            return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        }
        if (has_any(access, rhi::AccessFlags::ShaderRead)) {
            return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        return D3D12_RESOURCE_STATE_COMMON;
    }


    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param flags Flags controlling optional behavior.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS to_d3d12(
        rhi::AccelerationStructureBuildFlags flags) noexcept {
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS result =
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
        if (has_any(flags, rhi::AccelerationStructureBuildFlags::AllowUpdate)) {
            result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
        }
        if (has_any(flags, rhi::AccelerationStructureBuildFlags::AllowCompaction)) {
            result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
        }
        if (has_any(flags, rhi::AccelerationStructureBuildFlags::PreferFastTrace)) {
            result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        }
        if (has_any(flags, rhi::AccelerationStructureBuildFlags::PreferFastBuild)) {
            result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        }
        if (has_any(flags, rhi::AccelerationStructureBuildFlags::MinimizeMemory)) {
            result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY;
        }
        return result;
    }

    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param flags Flags controlling optional behavior.
    ///
    /// @return Returns the value converted to D3D12 representation.
    /// @note This function does not throw exceptions.
    D3D12_RAYTRACING_GEOMETRY_FLAGS to_d3d12(rhi::AccelerationStructureGeometryFlags flags) noexcept {
        D3D12_RAYTRACING_GEOMETRY_FLAGS result = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        if (has_any(flags, rhi::AccelerationStructureGeometryFlags::Opaque)) {
            result |= D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        }
        if (has_any(flags, rhi::AccelerationStructureGeometryFlags::NoDuplicateAnyHitInvocation)) {
            result |= D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;
        }
        return result;
    }


    /// Converts the supplied engine/RHI value to its D3D12 representation.
    ///
    /// @param stages `stages` value used by the operation.
    ///
    /// @return Returns the value converted to D3D12 visibility representation.
    /// @note This function does not throw exceptions.
    D3D12_SHADER_VISIBILITY to_d3d12_visibility(rhi::ShaderStage stages) noexcept {
        switch (stages) {
            case rhi::ShaderStage::Vertex:
                return D3D12_SHADER_VISIBILITY_VERTEX;
            case rhi::ShaderStage::Fragment:
                return D3D12_SHADER_VISIBILITY_PIXEL;
            case rhi::ShaderStage::Geometry:
                return D3D12_SHADER_VISIBILITY_GEOMETRY;
            case rhi::ShaderStage::TessControl:
                return D3D12_SHADER_VISIBILITY_HULL;
            case rhi::ShaderStage::TessEval:
                return D3D12_SHADER_VISIBILITY_DOMAIN;
            case rhi::ShaderStage::Task:
                return D3D12_SHADER_VISIBILITY_AMPLIFICATION;
            case rhi::ShaderStage::Mesh:
                return D3D12_SHADER_VISIBILITY_MESH;
            default:
                break;
        }


        return D3D12_SHADER_VISIBILITY_ALL;
    }

} // namespace SFT::D3D12
