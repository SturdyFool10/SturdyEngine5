#include <D3D12/D3D12Convert.hpp>

namespace SFT::D3D12 {

    using rhi::has_any;

    // ─── Formats ─────────────────────────────────────────────────────────────────

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

    DXGI_FORMAT to_dxgi_resource_format(rhi::Format format, rhi::TextureUsage usage) noexcept {
        // A depth texture that is *also* read in a shader cannot be created as DXGI_FORMAT_D32_FLOAT:
        // a D* format admits only a DSV, never an SRV. Creating it typeless is the only arrangement
        // that supports both views, and is why this takes the usage rather than the format alone.
        if (rhi::format_is_depth_stencil(format) &&
            has_any(usage, rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage)) {
            const DXGI_FORMAT typeless = to_dxgi_typeless_format(format);
            if (typeless != DXGI_FORMAT_UNKNOWN) {
                return typeless;
            }
        }
        return to_dxgi_view_format(format);
    }

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

            // Block-compressed: bytes per 4x4 block, not per texel.
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

    u32 format_block_extent(rhi::Format format) noexcept {
        return rhi::format_is_block_compressed(format) ? 4u : 1u;
    }

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

    DXGI_FORMAT to_dxgi(rhi::IndexFormat format) noexcept {
        return format == rhi::IndexFormat::Uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    }

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
            // DXGI exposes HLG only for YCbCr video surfaces, not RGB swapchains. It also has no
            // RGB linear-BT.2020 or Display-P3 color-space tag; presenting one as a nearby DXGI
            // space would misidentify the content's transfer function or primaries.
            case rhi::ColorSpace::Hdr10Hlg:
            case rhi::ColorSpace::DisplayP3Nonlinear:
            case rhi::ColorSpace::Bt2020Linear:
                return false;
            // DolbyVision has no DXGI swapchain color space at all (it is delivered through a
            // certified driver/OS path this engine has no access to — see ColorSpace::DolbyVision's
            // own doc comment), and DXGI names no linear/nonlinear AdobeRGB or linear Display-P3
            // space. Reported as "no equivalent" rather than silently mistagged.
            case rhi::ColorSpace::DolbyVision:
            case rhi::ColorSpace::AdobeRgbLinear:
            case rhi::ColorSpace::AdobeRgbNonlinear:
            case rhi::ColorSpace::DisplayP3Linear:
                return false;
        }
        return false;
    }

    // ─── Resources ───────────────────────────────────────────────────────────────

    D3D12_RESOURCE_FLAGS to_d3d12_resource_flags(rhi::BufferUsage usage) noexcept {
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
        // Only Storage (UAV) and acceleration-structure scratch actually need a resource flag; D3D12
        // buffers carry no vertex/index/uniform/indirect creation bits the way Vulkan's usage mask
        // does — those are expressed by the view/binding at use time, which is why this mapping looks
        // so much thinner than its Vulkan counterpart.
        if (has_any(usage, rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationStructureScratch)) {
            flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
        return flags;
    }

    D3D12_RESOURCE_FLAGS to_d3d12_resource_flags(rhi::TextureUsage usage, rhi::Format format) noexcept {
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
        if (has_any(usage, rhi::TextureUsage::ColorAttachment)) {
            flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        }
        if (has_any(usage, rhi::TextureUsage::DepthStencilAttachment) ||
            (rhi::format_is_depth_stencil(format) && has_any(usage, rhi::TextureUsage::TransientAttachment))) {
            flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            // A depth resource that is never sampled should also deny shader resource views, which
            // lets the driver keep it in its most compressed layout.
            if (!has_any(usage, rhi::TextureUsage::Sampled)) {
                flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
            }
        }
        if (has_any(usage, rhi::TextureUsage::Storage)) {
            flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
        return flags;
    }

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

    // ─── Samplers / comparison ───────────────────────────────────────────────────

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

    D3D12_FILTER to_d3d12_filter(const rhi::SamplerDesc &desc) noexcept {
        // Anisotropy subsumes the min/mag/mip selection entirely in D3D12 — there is one ANISOTROPIC
        // filter value (plus its comparison variant), not a per-axis combination.
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

    // ─── Pipeline state ──────────────────────────────────────────────────────────

    D3D12_FILL_MODE to_d3d12(rhi::PolygonMode mode) noexcept {
        // D3D12's rasterizer has no point fill mode; PolygonMode::Point is gated behind
        // Feature::PointPolygonMode, which this backend never reports supported, so reaching it here
        // means a caller ignored the guard. Wireframe is the least-wrong answer (it at least still
        // renders something identifiable) and the pipeline is still valid.
        return mode == rhi::PolygonMode::Fill ? D3D12_FILL_MODE_SOLID : D3D12_FILL_MODE_WIREFRAME;
    }

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

    D3D12_BLEND to_d3d12_blend(rhi::BlendFactor factor, bool is_alpha) noexcept {
        // See this file's header comment: a color-channel factor used in the alpha equation is a
        // debug-layer error in D3D12, so each one folds onto its alpha spelling when `is_alpha`.
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

    // ─── Queues / queries ────────────────────────────────────────────────────────

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
            // D3D12 has no sparse/tiled-binding queue: UpdateTileMappings is a method on a normal
            // command queue rather than work submitted to a dedicated engine. Aliasing onto DIRECT is
            // therefore the accurate mapping, not a fallback.
            case rhi::QueueClass::Sparse:
                return D3D12_COMMAND_LIST_TYPE_DIRECT;
        }
        return D3D12_COMMAND_LIST_TYPE_DIRECT;
    }

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

    // ─── Enhanced barriers ───────────────────────────────────────────────────────

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
        // D3D12 has no separate input-assembler sync scope; index/vertex fetch is covered by the
        // VERTEX_SHADING scope, which is where the fetched data is consumed.
        add(rhi::PipelineStage::VertexInput, D3D12_BARRIER_SYNC_INDEX_INPUT);
        add(rhi::PipelineStage::VertexShader, D3D12_BARRIER_SYNC_VERTEX_SHADING);
        add(rhi::PipelineStage::TessControlShader, D3D12_BARRIER_SYNC_VERTEX_SHADING);
        add(rhi::PipelineStage::TessEvalShader, D3D12_BARRIER_SYNC_VERTEX_SHADING);
        add(rhi::PipelineStage::GeometryShader, D3D12_BARRIER_SYNC_VERTEX_SHADING);
        add(rhi::PipelineStage::TaskShader, D3D12_BARRIER_SYNC_VERTEX_SHADING);
        add(rhi::PipelineStage::MeshShader, D3D12_BARRIER_SYNC_VERTEX_SHADING);
        add(rhi::PipelineStage::FragmentShader, D3D12_BARRIER_SYNC_PIXEL_SHADING);
        // Depth/stencil testing is one sync scope in D3D12; both early and late fragment tests map to
        // it (there is no early/late split to preserve).
        add(rhi::PipelineStage::EarlyFragmentTests, D3D12_BARRIER_SYNC_DEPTH_STENCIL);
        add(rhi::PipelineStage::LateFragmentTests, D3D12_BARRIER_SYNC_DEPTH_STENCIL);
        add(rhi::PipelineStage::ColorAttachmentOutput, D3D12_BARRIER_SYNC_RENDER_TARGET);
        add(rhi::PipelineStage::ComputeShader, D3D12_BARRIER_SYNC_COMPUTE_SHADING);
        // The RHI Transfer stage accompanies TransferRead/TransferWrite, which map to COPY_SOURCE/
        // COPY_DEST. RESOLVE and CLEAR_UAV are distinct D3D12 scopes with incompatible access masks;
        // including them here makes an otherwise ordinary buffer-to-texture copy barrier invalid.
        add(rhi::PipelineStage::Transfer, D3D12_BARRIER_SYNC_COPY);
        add(rhi::PipelineStage::RayTracingShader, D3D12_BARRIER_SYNC_RAYTRACING);
        add(rhi::PipelineStage::AccelerationStructureBuild,
            D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE);

        // PipelineStage::Host has no D3D12 sync scope: CPU visibility of mapped memory is governed by
        // the heap type and fence completion, not by a barrier. A host-only stage mask therefore
        // legitimately produces SYNC_NONE.
        return sync;
    }

    D3D12_BARRIER_ACCESS to_d3d12_access(rhi::AccessFlags access) noexcept {
        if (access == rhi::AccessFlags::None) {
            // COMMON (== 0) means "compatible with everything", which is the right reading of an
            // unspecified access mask. NO_ACCESS is a different, stricter statement and is applied by
            // the barrier encoder only where D3D12 actually requires it (see D3D12Barriers.cpp).
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

        // MemoryRead/MemoryWrite are the RHI's deliberate "any access" catch-alls, and D3D12's
        // equivalent of "any access" is COMMON — a zero mask, not a union of every bit (a union would
        // be rejected, since several ACCESS bits are mutually exclusive with each other).
        if (has_any(access, rhi::AccessFlags::MemoryRead | rhi::AccessFlags::MemoryWrite)) {
            return D3D12_BARRIER_ACCESS_COMMON;
        }
        // HostRead/HostWrite likewise have no D3D12 barrier access bit; they contribute nothing and a
        // host-only mask correctly lands on COMMON.
        return result;
    }

    D3D12_BARRIER_LAYOUT to_d3d12_layout(rhi::TextureLayout layout) noexcept {
        switch (layout) {
            case rhi::TextureLayout::Undefined:
                return D3D12_BARRIER_LAYOUT_UNDEFINED;
            case rhi::TextureLayout::General:
                return D3D12_BARRIER_LAYOUT_COMMON;
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
            // A DXGI back buffer must be in PRESENT (== COMMON) when Present() is called.
            case rhi::TextureLayout::Present:
                return D3D12_BARRIER_LAYOUT_PRESENT;
        }
        return D3D12_BARRIER_LAYOUT_COMMON;
    }

    // ─── Legacy resource states ──────────────────────────────────────────────────

    D3D12_RESOURCE_STATES to_legacy_texture_state(rhi::TextureLayout layout) noexcept {
        switch (layout) {
            case rhi::TextureLayout::Undefined:
            case rhi::TextureLayout::General:
                return D3D12_RESOURCE_STATE_COMMON;
            case rhi::TextureLayout::ColorAttachment:
                return D3D12_RESOURCE_STATE_RENDER_TARGET;
            case rhi::TextureLayout::DepthStencilAttachment:
                return D3D12_RESOURCE_STATE_DEPTH_WRITE;
            case rhi::TextureLayout::DepthStencilReadOnly:
                return D3D12_RESOURCE_STATE_DEPTH_READ;
            case rhi::TextureLayout::ShaderReadOnly:
                // Both shader-resource states, because the RHI's layout does not distinguish which
                // stage reads it and a legacy barrier has to name every state the resource may be
                // read from next.
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

    D3D12_RESOURCE_STATES to_legacy_buffer_state(rhi::AccessFlags access) noexcept {
        // Ordered most-specific first: a legacy state is a single value, so where an access mask names
        // several uses the write states have to win (a resource left in a read state while being
        // written is the corruption case; the reverse merely costs a redundant transition).
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

    // ─── Ray tracing ─────────────────────────────────────────────────────────────

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

    // ─── Shader stage visibility ─────────────────────────────────────────────────

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
        // Compute and ray tracing have no visibility enumerant of their own — a compute or DXR root
        // signature's parameters are always ALL — and any multi-stage mask widens to ALL per this
        // function's header comment.
        return D3D12_SHADER_VISIBILITY_ALL;
    }

} // namespace SFT::D3D12
