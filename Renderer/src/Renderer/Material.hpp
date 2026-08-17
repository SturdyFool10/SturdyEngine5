#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <string>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include "Handles.hpp"
#include "ReflectionBinding.hpp"

using std::string;
using std::vector;

namespace SFT::Renderer {


    namespace slang = Core::Slang;


    enum class MaterialParameterType : u32 {
        Unknown,
        Float,
        Vec2,
        Vec3,
        Vec4,
        Mat3,
        Mat4,
        Int,
        UInt,
    };


    /// Performs the material parameter type of operation using the supplied arguments.
    ///
    /// @param uniform `uniform` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] MaterialParameterType material_parameter_type_of(const ReflectedUniform &uniform) noexcept;


    struct MaterialParameter {
        string name;
        u32 offset = 0;
        u32 size = 0;
        MaterialParameterType type = MaterialParameterType::Unknown;
        vector<byte> default_bytes;
    };


    struct MaterialTextureSlot {
        string name;
        u32 set = 0;
        u32 binding = 0;
        RHI::BindingType type = RHI::BindingType::SampledTexture;
    };


    struct MaterialPipelineVariant {
        vector<RHI::Format> color_formats;
        RHI::Format depth_format = RHI::Format::Undefined;


        bool standard_depth_test = false;
        RHI::SampleCount samples = RHI::SampleCount::X1;
        RHI::RenderPipelineHandle pipeline{};
    };


    struct DepthOnlyPipelineVariant {
        RHI::Format depth_format = RHI::Format::Undefined;
        bool shadow_map = false;
        f32 depth_bias = 0.0f;
        f32 slope_bias = 0.0f;
        RHI::SampleCount samples = RHI::SampleCount::X1;
        RHI::RenderPipelineHandle pipeline{};
    };


    struct MaterialTemplateResource {
        MaterialTemplateHandle handle{};
        UString label;


        Core::Slang::Shader shader;


        Core::Slang::ShaderVariantCache variant_cache;
        bool hot_reloadable = false;

        RHI::ShaderModuleHandle vertex_module{};
        RHI::ShaderModuleHandle fragment_module{};
        string vertex_entry_point;
        string fragment_entry_point;
        bool has_fragment = false;


        RHI::ShaderModuleHandle depth_only_fragment_module{};
        string depth_only_fragment_entry_point;
        bool has_depth_only_fragment = false;
        vector<RHI::BindGroupLayoutHandle> bind_group_layouts;


        vector<u32> bind_group_layout_sets;
        RHI::PipelineLayoutHandle pipeline_layout{};


        u32 uniform_block_size = 0;

        u32 uniform_set = 0;
        u32 uniform_binding = 0;
        bool has_uniform_block = false;

        vector<MaterialParameter> parameters;
        vector<MaterialTextureSlot> texture_slots;


        bool alive = false;
    };


    struct MaterialTextureBinding {
        u32 binding = 0;
        TextureHandle texture{};
    };


    struct MaterialInstanceFrame {
        RHI::BufferHandle uniform_buffer{};
        vector<RHI::BindGroupHandle> bind_groups;


        bool uniform_dirty = true;
        bool bind_groups_dirty = true;
    };


    struct MaterialInstanceResource {
        MaterialInstanceHandle handle{};
        MaterialTemplateHandle material_template{};
        UString label;

        vector<byte> uniform_values;
        vector<MaterialTextureBinding> textures;


        u64 content_revision = 1;

        vector<MaterialInstanceFrame> frames;
        bool alive = false;
    };

} // namespace SFT::Renderer
