#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-extension"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <vk_mem_alloc.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#pragma endregion

#include <RHI/RHI.hpp>

// Translation between the API-agnostic Sturdy.RHI vocabulary and concrete Vulkan enums/flags. Kept in
// one place so the RHI backend (:VulkanRhiBackend) reads as straight descriptor mapping. Every function
// is a pure `to_vk(...)`; there is no state here.
namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    [[nodiscard]] constexpr VkFormat to_vk(rhi::Format format) noexcept {
        switch (format) {
            case rhi::Format::Undefined: return VK_FORMAT_UNDEFINED;
            case rhi::Format::R8Unorm: return VK_FORMAT_R8_UNORM;
            case rhi::Format::R8Snorm: return VK_FORMAT_R8_SNORM;
            case rhi::Format::R8Uint: return VK_FORMAT_R8_UINT;
            case rhi::Format::R8Sint: return VK_FORMAT_R8_SINT;
            case rhi::Format::RG8Unorm: return VK_FORMAT_R8G8_UNORM;
            case rhi::Format::RG8Snorm: return VK_FORMAT_R8G8_SNORM;
            case rhi::Format::RG8Uint: return VK_FORMAT_R8G8_UINT;
            case rhi::Format::RG8Sint: return VK_FORMAT_R8G8_SINT;
            case rhi::Format::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
            case rhi::Format::RGBA8UnormSrgb: return VK_FORMAT_R8G8B8A8_SRGB;
            case rhi::Format::RGBA8Snorm: return VK_FORMAT_R8G8B8A8_SNORM;
            case rhi::Format::RGBA8Uint: return VK_FORMAT_R8G8B8A8_UINT;
            case rhi::Format::RGBA8Sint: return VK_FORMAT_R8G8B8A8_SINT;
            case rhi::Format::BGRA8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
            case rhi::Format::BGRA8UnormSrgb: return VK_FORMAT_B8G8R8A8_SRGB;
            case rhi::Format::RGB10A2Unorm: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            case rhi::Format::RG11B10Float: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
            case rhi::Format::R16Uint: return VK_FORMAT_R16_UINT;
            case rhi::Format::R16Sint: return VK_FORMAT_R16_SINT;
            case rhi::Format::R16Float: return VK_FORMAT_R16_SFLOAT;
            case rhi::Format::RG16Uint: return VK_FORMAT_R16G16_UINT;
            case rhi::Format::RG16Sint: return VK_FORMAT_R16G16_SINT;
            case rhi::Format::RG16Float: return VK_FORMAT_R16G16_SFLOAT;
            case rhi::Format::RGBA16Uint: return VK_FORMAT_R16G16B16A16_UINT;
            case rhi::Format::RGBA16Sint: return VK_FORMAT_R16G16B16A16_SINT;
            case rhi::Format::RGBA16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case rhi::Format::R32Uint: return VK_FORMAT_R32_UINT;
            case rhi::Format::R32Sint: return VK_FORMAT_R32_SINT;
            case rhi::Format::R32Float: return VK_FORMAT_R32_SFLOAT;
            case rhi::Format::RG32Uint: return VK_FORMAT_R32G32_UINT;
            case rhi::Format::RG32Sint: return VK_FORMAT_R32G32_SINT;
            case rhi::Format::RG32Float: return VK_FORMAT_R32G32_SFLOAT;
            case rhi::Format::RGBA32Uint: return VK_FORMAT_R32G32B32A32_UINT;
            case rhi::Format::RGBA32Sint: return VK_FORMAT_R32G32B32A32_SINT;
            case rhi::Format::RGBA32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
            case rhi::Format::D16Unorm: return VK_FORMAT_D16_UNORM;
            case rhi::Format::D24UnormS8Uint: return VK_FORMAT_D24_UNORM_S8_UINT;
            case rhi::Format::D32Float: return VK_FORMAT_D32_SFLOAT;
            case rhi::Format::D32FloatS8Uint: return VK_FORMAT_D32_SFLOAT_S8_UINT;
        }
        return VK_FORMAT_UNDEFINED;
    }

    [[nodiscard]] constexpr VkFormat to_vk(rhi::VertexFormat format) noexcept {
        switch (format) {
            case rhi::VertexFormat::Float32: return VK_FORMAT_R32_SFLOAT;
            case rhi::VertexFormat::Float32x2: return VK_FORMAT_R32G32_SFLOAT;
            case rhi::VertexFormat::Float32x3: return VK_FORMAT_R32G32B32_SFLOAT;
            case rhi::VertexFormat::Float32x4: return VK_FORMAT_R32G32B32A32_SFLOAT;
            case rhi::VertexFormat::Uint32: return VK_FORMAT_R32_UINT;
            case rhi::VertexFormat::Uint32x2: return VK_FORMAT_R32G32_UINT;
            case rhi::VertexFormat::Uint32x3: return VK_FORMAT_R32G32B32_UINT;
            case rhi::VertexFormat::Uint32x4: return VK_FORMAT_R32G32B32A32_UINT;
            case rhi::VertexFormat::Sint32: return VK_FORMAT_R32_SINT;
            case rhi::VertexFormat::Sint32x2: return VK_FORMAT_R32G32_SINT;
            case rhi::VertexFormat::Sint32x3: return VK_FORMAT_R32G32B32_SINT;
            case rhi::VertexFormat::Sint32x4: return VK_FORMAT_R32G32B32A32_SINT;
            case rhi::VertexFormat::Uint8x4Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
            case rhi::VertexFormat::Sint8x4Norm: return VK_FORMAT_R8G8B8A8_SNORM;
            case rhi::VertexFormat::Uint16x2Unorm: return VK_FORMAT_R16G16_UNORM;
            case rhi::VertexFormat::Uint16x4Unorm: return VK_FORMAT_R16G16B16A16_UNORM;
            case rhi::VertexFormat::Float16x2: return VK_FORMAT_R16G16_SFLOAT;
            case rhi::VertexFormat::Float16x4: return VK_FORMAT_R16G16B16A16_SFLOAT;
        }
        return VK_FORMAT_UNDEFINED;
    }

    [[nodiscard]] constexpr VkSampleCountFlagBits to_vk(rhi::SampleCount samples) noexcept {
        switch (samples) {
            case rhi::SampleCount::X1: return VK_SAMPLE_COUNT_1_BIT;
            case rhi::SampleCount::X2: return VK_SAMPLE_COUNT_2_BIT;
            case rhi::SampleCount::X4: return VK_SAMPLE_COUNT_4_BIT;
            case rhi::SampleCount::X8: return VK_SAMPLE_COUNT_8_BIT;
            case rhi::SampleCount::X16: return VK_SAMPLE_COUNT_16_BIT;
        }
        return VK_SAMPLE_COUNT_1_BIT;
    }

    [[nodiscard]] constexpr VkIndexType to_vk(rhi::IndexFormat format) noexcept {
        return format == rhi::IndexFormat::Uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    }

    [[nodiscard]] constexpr VkBufferUsageFlags to_vk(rhi::BufferUsage usage) noexcept {
        VkBufferUsageFlags out = 0;
        if (rhi::has_any(usage, rhi::BufferUsage::TransferSrc)) out |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (rhi::has_any(usage, rhi::BufferUsage::TransferDst)) out |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (rhi::has_any(usage, rhi::BufferUsage::Vertex)) out |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (rhi::has_any(usage, rhi::BufferUsage::Index)) out |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (rhi::has_any(usage, rhi::BufferUsage::Uniform)) out |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (rhi::has_any(usage, rhi::BufferUsage::Storage)) out |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (rhi::has_any(usage, rhi::BufferUsage::Indirect)) out |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        if (rhi::has_any(usage, rhi::BufferUsage::ShaderBindingTable))
            out |= VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        if (rhi::has_any(usage, rhi::BufferUsage::AccelerationStructure))
            out |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        if (rhi::has_any(usage, rhi::BufferUsage::AccelerationStructureInput))
            out |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        return out;
    }

    [[nodiscard]] constexpr VkImageUsageFlags to_vk(rhi::TextureUsage usage) noexcept {
        VkImageUsageFlags out = 0;
        if (rhi::has_any(usage, rhi::TextureUsage::TransferSrc)) out |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (rhi::has_any(usage, rhi::TextureUsage::TransferDst)) out |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (rhi::has_any(usage, rhi::TextureUsage::Sampled)) out |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (rhi::has_any(usage, rhi::TextureUsage::Storage)) out |= VK_IMAGE_USAGE_STORAGE_BIT;
        if (rhi::has_any(usage, rhi::TextureUsage::ColorAttachment)) out |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (rhi::has_any(usage, rhi::TextureUsage::DepthStencilAttachment))
            out |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        return out;
    }

    [[nodiscard]] constexpr VkImageType to_vk(rhi::TextureDimension dimension) noexcept {
        switch (dimension) {
            case rhi::TextureDimension::Dim1D: return VK_IMAGE_TYPE_1D;
            case rhi::TextureDimension::Dim2D: return VK_IMAGE_TYPE_2D;
            case rhi::TextureDimension::Dim3D: return VK_IMAGE_TYPE_3D;
        }
        return VK_IMAGE_TYPE_2D;
    }

    [[nodiscard]] constexpr VkImageViewType to_vk(rhi::TextureViewType type) noexcept {
        switch (type) {
            case rhi::TextureViewType::View1D: return VK_IMAGE_VIEW_TYPE_1D;
            case rhi::TextureViewType::View2D: return VK_IMAGE_VIEW_TYPE_2D;
            case rhi::TextureViewType::View2DArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            case rhi::TextureViewType::ViewCube: return VK_IMAGE_VIEW_TYPE_CUBE;
            case rhi::TextureViewType::ViewCubeArray: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
            case rhi::TextureViewType::View3D: return VK_IMAGE_VIEW_TYPE_3D;
        }
        return VK_IMAGE_VIEW_TYPE_2D;
    }

    [[nodiscard]] constexpr VkImageAspectFlags aspect_for_format(rhi::Format format) noexcept {
        if (rhi::format_has_depth(format) && rhi::format_has_stencil(format))
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        if (rhi::format_has_depth(format)) return VK_IMAGE_ASPECT_DEPTH_BIT;
        if (rhi::format_has_stencil(format)) return VK_IMAGE_ASPECT_STENCIL_BIT;
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }

    [[nodiscard]] constexpr VkFilter to_vk(rhi::Filter filter) noexcept {
        return filter == rhi::Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    }

    [[nodiscard]] constexpr VkSamplerMipmapMode to_vk(rhi::MipmapMode mode) noexcept {
        return mode == rhi::MipmapMode::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    }

    [[nodiscard]] constexpr VkSamplerAddressMode to_vk(rhi::AddressMode mode) noexcept {
        switch (mode) {
            case rhi::AddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case rhi::AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case rhi::AddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case rhi::AddressMode::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        }
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }

    [[nodiscard]] constexpr VkBorderColor to_vk(rhi::BorderColor color) noexcept {
        switch (color) {
            case rhi::BorderColor::TransparentBlack: return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            case rhi::BorderColor::OpaqueBlack: return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
            case rhi::BorderColor::OpaqueWhite: return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        }
        return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }

    [[nodiscard]] constexpr VkCompareOp to_vk(rhi::CompareOp op) noexcept {
        switch (op) {
            case rhi::CompareOp::Never: return VK_COMPARE_OP_NEVER;
            case rhi::CompareOp::Less: return VK_COMPARE_OP_LESS;
            case rhi::CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
            case rhi::CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
            case rhi::CompareOp::Greater: return VK_COMPARE_OP_GREATER;
            case rhi::CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
            case rhi::CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case rhi::CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
        }
        return VK_COMPARE_OP_ALWAYS;
    }

    [[nodiscard]] constexpr VkShaderStageFlags to_vk(rhi::ShaderStage stages) noexcept {
        VkShaderStageFlags out = 0;
        if (rhi::has_any(stages, rhi::ShaderStage::Vertex)) out |= VK_SHADER_STAGE_VERTEX_BIT;
        if (rhi::has_any(stages, rhi::ShaderStage::Fragment)) out |= VK_SHADER_STAGE_FRAGMENT_BIT;
        if (rhi::has_any(stages, rhi::ShaderStage::Compute)) out |= VK_SHADER_STAGE_COMPUTE_BIT;
        if (rhi::has_any(stages, rhi::ShaderStage::Geometry)) out |= VK_SHADER_STAGE_GEOMETRY_BIT;
        if (rhi::has_any(stages, rhi::ShaderStage::TessControl)) out |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        if (rhi::has_any(stages, rhi::ShaderStage::TessEval)) out |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        if (rhi::has_any(stages, rhi::ShaderStage::Task)) out |= VK_SHADER_STAGE_TASK_BIT_EXT;
        if (rhi::has_any(stages, rhi::ShaderStage::Mesh)) out |= VK_SHADER_STAGE_MESH_BIT_EXT;
        if (rhi::has_any(stages, rhi::ShaderStage::RayGeneration)) out |= VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        if (rhi::has_any(stages, rhi::ShaderStage::AnyHit)) out |= VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
        if (rhi::has_any(stages, rhi::ShaderStage::ClosestHit)) out |= VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
        if (rhi::has_any(stages, rhi::ShaderStage::Miss)) out |= VK_SHADER_STAGE_MISS_BIT_KHR;
        if (rhi::has_any(stages, rhi::ShaderStage::Intersection)) out |= VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
        if (rhi::has_any(stages, rhi::ShaderStage::Callable)) out |= VK_SHADER_STAGE_CALLABLE_BIT_KHR;
        return out;
    }

    [[nodiscard]] constexpr VkShaderStageFlagBits to_vk_single_stage(rhi::ShaderStage stage) noexcept {
        return static_cast<VkShaderStageFlagBits>(to_vk(stage));
    }

    [[nodiscard]] constexpr VkDescriptorType to_vk(rhi::BindingType type) noexcept {
        switch (type) {
            case rhi::BindingType::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            case rhi::BindingType::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            case rhi::BindingType::ReadOnlyStorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            case rhi::BindingType::SampledTexture: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            case rhi::BindingType::StorageTexture: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            case rhi::BindingType::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
            case rhi::BindingType::CombinedImageSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            case rhi::BindingType::AccelerationStructure: return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            case rhi::BindingType::InputAttachment: return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        }
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    }

    [[nodiscard]] constexpr VkDescriptorBindingFlags to_vk(rhi::BindingFlags flags) noexcept {
        VkDescriptorBindingFlags out = 0;
        if (rhi::has_any(flags, rhi::BindingFlags::PartiallyBound)) out |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        if (rhi::has_any(flags, rhi::BindingFlags::UpdateAfterBind)) out |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        if (rhi::has_any(flags, rhi::BindingFlags::VariableDescriptorCount)) out |= VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
        return out;
    }

    [[nodiscard]] constexpr VkPrimitiveTopology to_vk(rhi::PrimitiveTopology topology) noexcept {
        switch (topology) {
            case rhi::PrimitiveTopology::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            case rhi::PrimitiveTopology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            case rhi::PrimitiveTopology::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            case rhi::PrimitiveTopology::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            case rhi::PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        }
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }

    [[nodiscard]] constexpr VkPolygonMode to_vk(rhi::PolygonMode mode) noexcept {
        switch (mode) {
            case rhi::PolygonMode::Fill: return VK_POLYGON_MODE_FILL;
            case rhi::PolygonMode::Line: return VK_POLYGON_MODE_LINE;
            case rhi::PolygonMode::Point: return VK_POLYGON_MODE_POINT;
        }
        return VK_POLYGON_MODE_FILL;
    }

    [[nodiscard]] constexpr VkCullModeFlags to_vk(rhi::CullMode mode) noexcept {
        switch (mode) {
            case rhi::CullMode::None: return VK_CULL_MODE_NONE;
            case rhi::CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
            case rhi::CullMode::Back: return VK_CULL_MODE_BACK_BIT;
        }
        return VK_CULL_MODE_NONE;
    }

    [[nodiscard]] constexpr VkFrontFace to_vk(rhi::FrontFace face) noexcept {
        return face == rhi::FrontFace::Clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    }

    [[nodiscard]] constexpr VkBlendFactor to_vk(rhi::BlendFactor factor) noexcept {
        switch (factor) {
            case rhi::BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
            case rhi::BlendFactor::One: return VK_BLEND_FACTOR_ONE;
            case rhi::BlendFactor::SrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
            case rhi::BlendFactor::OneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case rhi::BlendFactor::DstColor: return VK_BLEND_FACTOR_DST_COLOR;
            case rhi::BlendFactor::OneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case rhi::BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
            case rhi::BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case rhi::BlendFactor::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
            case rhi::BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case rhi::BlendFactor::ConstantColor: return VK_BLEND_FACTOR_CONSTANT_COLOR;
            case rhi::BlendFactor::OneMinusConstantColor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
            case rhi::BlendFactor::SrcAlphaSaturated: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        }
        return VK_BLEND_FACTOR_ONE;
    }

    [[nodiscard]] constexpr VkBlendOp to_vk(rhi::BlendOp op) noexcept {
        switch (op) {
            case rhi::BlendOp::Add: return VK_BLEND_OP_ADD;
            case rhi::BlendOp::Subtract: return VK_BLEND_OP_SUBTRACT;
            case rhi::BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
            case rhi::BlendOp::Min: return VK_BLEND_OP_MIN;
            case rhi::BlendOp::Max: return VK_BLEND_OP_MAX;
        }
        return VK_BLEND_OP_ADD;
    }

    [[nodiscard]] constexpr VkColorComponentFlags to_vk(rhi::ColorWriteMask mask) noexcept {
        VkColorComponentFlags out = 0;
        if (rhi::has_any(mask, rhi::ColorWriteMask::Red)) out |= VK_COLOR_COMPONENT_R_BIT;
        if (rhi::has_any(mask, rhi::ColorWriteMask::Green)) out |= VK_COLOR_COMPONENT_G_BIT;
        if (rhi::has_any(mask, rhi::ColorWriteMask::Blue)) out |= VK_COLOR_COMPONENT_B_BIT;
        if (rhi::has_any(mask, rhi::ColorWriteMask::Alpha)) out |= VK_COLOR_COMPONENT_A_BIT;
        return out;
    }

    [[nodiscard]] constexpr VkStencilOp to_vk(rhi::StencilOp op) noexcept {
        switch (op) {
            case rhi::StencilOp::Keep: return VK_STENCIL_OP_KEEP;
            case rhi::StencilOp::Zero: return VK_STENCIL_OP_ZERO;
            case rhi::StencilOp::Replace: return VK_STENCIL_OP_REPLACE;
            case rhi::StencilOp::IncrementClamp: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
            case rhi::StencilOp::DecrementClamp: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
            case rhi::StencilOp::Invert: return VK_STENCIL_OP_INVERT;
            case rhi::StencilOp::IncrementWrap: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
            case rhi::StencilOp::DecrementWrap: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        }
        return VK_STENCIL_OP_KEEP;
    }

    [[nodiscard]] constexpr VkVertexInputRate to_vk(rhi::VertexStepMode mode) noexcept {
        return mode == rhi::VertexStepMode::Instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
    }

    [[nodiscard]] constexpr VkAttachmentLoadOp to_vk(rhi::LoadOp op) noexcept {
        switch (op) {
            case rhi::LoadOp::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
            case rhi::LoadOp::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
            case rhi::LoadOp::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        }
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }

    [[nodiscard]] constexpr VkAttachmentStoreOp to_vk(rhi::StoreOp op) noexcept {
        return op == rhi::StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }

    [[nodiscard]] constexpr VkImageLayout to_vk(rhi::TextureLayout layout) noexcept {
        switch (layout) {
            case rhi::TextureLayout::Undefined: return VK_IMAGE_LAYOUT_UNDEFINED;
            case rhi::TextureLayout::General: return VK_IMAGE_LAYOUT_GENERAL;
            case rhi::TextureLayout::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            case rhi::TextureLayout::DepthStencilAttachment: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            case rhi::TextureLayout::DepthStencilReadOnly: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            case rhi::TextureLayout::ShaderReadOnly: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            case rhi::TextureLayout::TransferSrc: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            case rhi::TextureLayout::TransferDst: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            case rhi::TextureLayout::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        }
        return VK_IMAGE_LAYOUT_UNDEFINED;
    }

    [[nodiscard]] constexpr VkPipelineStageFlags2 to_vk(rhi::PipelineStage stages) noexcept {
        // The RHI PipelineStage bits are laid out to mirror VkPipelineStage2, but map explicitly rather
        // than bit-cast so the contract survives either enum being reordered.
        VkPipelineStageFlags2 out = 0;
        using S = rhi::PipelineStage;
        if (rhi::has_any(stages, S::DrawIndirect)) out |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        if (rhi::has_any(stages, S::VertexInput)) out |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
        if (rhi::has_any(stages, S::VertexShader)) out |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        if (rhi::has_any(stages, S::TessControlShader)) out |= VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT;
        if (rhi::has_any(stages, S::TessEvalShader)) out |= VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT;
        if (rhi::has_any(stages, S::GeometryShader)) out |= VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT;
        if (rhi::has_any(stages, S::FragmentShader)) out |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        if (rhi::has_any(stages, S::EarlyFragmentTests)) out |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
        if (rhi::has_any(stages, S::LateFragmentTests)) out |= VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        if (rhi::has_any(stages, S::ColorAttachmentOutput)) out |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        if (rhi::has_any(stages, S::ComputeShader)) out |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        if (rhi::has_any(stages, S::Transfer)) out |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        if (rhi::has_any(stages, S::Host)) out |= VK_PIPELINE_STAGE_2_HOST_BIT;
        if (rhi::has_any(stages, S::TaskShader)) out |= VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT;
        if (rhi::has_any(stages, S::MeshShader)) out |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
        if (rhi::has_any(stages, S::RayTracingShader)) out |= VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        if (rhi::has_any(stages, S::AccelerationStructureBuild))
            out |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        if (stages == S::AllCommands) out = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        return out;
    }

    [[nodiscard]] constexpr VkAccessFlags2 to_vk(rhi::AccessFlags access) noexcept {
        VkAccessFlags2 out = 0;
        using A = rhi::AccessFlags;
        if (rhi::has_any(access, A::IndirectCommandRead)) out |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        if (rhi::has_any(access, A::IndexRead)) out |= VK_ACCESS_2_INDEX_READ_BIT;
        if (rhi::has_any(access, A::VertexAttributeRead)) out |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        if (rhi::has_any(access, A::UniformRead)) out |= VK_ACCESS_2_UNIFORM_READ_BIT;
        if (rhi::has_any(access, A::ShaderRead)) out |= VK_ACCESS_2_SHADER_READ_BIT;
        if (rhi::has_any(access, A::ShaderWrite)) out |= VK_ACCESS_2_SHADER_WRITE_BIT;
        if (rhi::has_any(access, A::ColorAttachmentRead)) out |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        if (rhi::has_any(access, A::ColorAttachmentWrite)) out |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        if (rhi::has_any(access, A::DepthStencilAttachmentRead)) out |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        if (rhi::has_any(access, A::DepthStencilAttachmentWrite)) out |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        if (rhi::has_any(access, A::TransferRead)) out |= VK_ACCESS_2_TRANSFER_READ_BIT;
        if (rhi::has_any(access, A::TransferWrite)) out |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
        if (rhi::has_any(access, A::HostRead)) out |= VK_ACCESS_2_HOST_READ_BIT;
        if (rhi::has_any(access, A::HostWrite)) out |= VK_ACCESS_2_HOST_WRITE_BIT;
        if (rhi::has_any(access, A::AccelerationStructureRead)) out |= VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        if (rhi::has_any(access, A::AccelerationStructureWrite)) out |= VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        if (rhi::has_any(access, A::MemoryRead)) out |= VK_ACCESS_2_MEMORY_READ_BIT;
        if (rhi::has_any(access, A::MemoryWrite)) out |= VK_ACCESS_2_MEMORY_WRITE_BIT;
        return out;
    }

    [[nodiscard]] constexpr VkQueryType to_vk(rhi::QueryType type) noexcept {
        switch (type) {
            case rhi::QueryType::Occlusion: return VK_QUERY_TYPE_OCCLUSION;
            case rhi::QueryType::Timestamp: return VK_QUERY_TYPE_TIMESTAMP;
            case rhi::QueryType::PipelineStatistics: return VK_QUERY_TYPE_PIPELINE_STATISTICS;
        }
        return VK_QUERY_TYPE_TIMESTAMP;
    }

    [[nodiscard]] constexpr VkQueryPipelineStatisticFlags to_vk(rhi::PipelineStatistic stats) noexcept {
        VkQueryPipelineStatisticFlags out = 0;
        using P = rhi::PipelineStatistic;
        if (rhi::has_any(stats, P::InputAssemblyVertices)) out |= VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT;
        if (rhi::has_any(stats, P::InputAssemblyPrimitives)) out |= VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT;
        if (rhi::has_any(stats, P::VertexShaderInvocations)) out |= VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT;
        if (rhi::has_any(stats, P::GeometryShaderInvocations)) out |= VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT;
        if (rhi::has_any(stats, P::GeometryShaderPrimitives)) out |= VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT;
        if (rhi::has_any(stats, P::ClippingInvocations)) out |= VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT;
        if (rhi::has_any(stats, P::ClippingPrimitives)) out |= VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT;
        if (rhi::has_any(stats, P::FragmentShaderInvocations)) out |= VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        if (rhi::has_any(stats, P::TessControlShaderPatches)) out |= VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_CONTROL_SHADER_PATCHES_BIT;
        if (rhi::has_any(stats, P::TessEvaluationShaderInvocations)) out |= VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT;
        if (rhi::has_any(stats, P::ComputeShaderInvocations)) out |= VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
        if (rhi::has_any(stats, P::TaskShaderInvocations)) out |= VK_QUERY_PIPELINE_STATISTIC_TASK_SHADER_INVOCATIONS_BIT_EXT;
        if (rhi::has_any(stats, P::MeshShaderInvocations)) out |= VK_QUERY_PIPELINE_STATISTIC_MESH_SHADER_INVOCATIONS_BIT_EXT;
        return out;
    }

    [[nodiscard]] constexpr VkQueryResultFlags to_vk(rhi::QueryResultFlags flags) noexcept {
        VkQueryResultFlags out = 0;
        using Q = rhi::QueryResultFlags;
        if (rhi::has_any(flags, Q::Result64Bit)) out |= VK_QUERY_RESULT_64_BIT;
        if (rhi::has_any(flags, Q::Wait)) out |= VK_QUERY_RESULT_WAIT_BIT;
        if (rhi::has_any(flags, Q::WithAvailability)) out |= VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
        if (rhi::has_any(flags, Q::Partial)) out |= VK_QUERY_RESULT_PARTIAL_BIT;
        return out;
    }

    // How a MemoryLocation maps onto a VMA allocation request.
    struct VmaMapping {
        VmaMemoryUsage usage = VMA_MEMORY_USAGE_AUTO;
        VmaAllocationCreateFlags flags = 0;
    };

    [[nodiscard]] constexpr VmaMapping to_vma(rhi::MemoryLocation location) noexcept {
        switch (location) {
            case rhi::MemoryLocation::DeviceLocal:
                return {VMA_MEMORY_USAGE_AUTO, 0};
            case rhi::MemoryLocation::HostUpload:
                return {VMA_MEMORY_USAGE_AUTO,
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
            case rhi::MemoryLocation::HostReadback:
                return {VMA_MEMORY_USAGE_AUTO,
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        }
        return {VMA_MEMORY_USAGE_AUTO, 0};
    }

    [[nodiscard]] constexpr rhi::DeviceType to_rhi_device_type(VkPhysicalDeviceType type) noexcept {
        switch (type) {
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return rhi::DeviceType::IntegratedGpu;
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return rhi::DeviceType::DiscreteGpu;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return rhi::DeviceType::VirtualGpu;
            case VK_PHYSICAL_DEVICE_TYPE_CPU: return rhi::DeviceType::Cpu;
            default: return rhi::DeviceType::Other;
        }
    }

} // namespace SFT::Core::Vulkan
