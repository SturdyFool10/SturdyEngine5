#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <string>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include "Handles.hpp"
#include "ReflectionBinding.hpp"

using std::byte;
using std::string;
using std::vector;

namespace SFT::Renderer {

    // ─── Material system ─────────────────────────────────────────────────────────────────────────
    //
    // A MaterialTemplate is built once per (compiled shader) from its Slang reflection: the RHI bind-
    // group/pipeline layouts, the byte layout of its uniform block, and its named texture slots all come
    // from reflection (see :ReflectionBinding), never hand-written. A MaterialInstance is a template plus
    // a CPU-side value block and texture bindings — the thing you actually set parameters on and draw.
    // See plans/material-system.md. Renderer owns creation/upkeep/teardown (RendererMaterial.cpp);
    // these are the data records it stores, mirroring MeshResource/TextureResource.

    namespace slang = Core::Slang;

    // The typed shape of one material parameter, derived from its reflected scalar type + dimensions.
    // Only the float-based shapes materials realistically expose are enumerated; anything else is
    // Unknown and settable only as raw bytes.
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

    // Classifies a reflected uniform leaf into a MaterialParameterType from its scalar type + shape.
    [[nodiscard]] inline MaterialParameterType material_parameter_type_of(const ReflectedUniform &uniform) noexcept {
        const bool is_float = uniform.scalar == slang::ShaderScalarType::Float32 ||
                              uniform.scalar == slang::ShaderScalarType::Float16 ||
                              uniform.scalar == slang::ShaderScalarType::Float64;
        if (uniform.rows > 1) {
            if (uniform.rows == 4 && uniform.columns == 4) return MaterialParameterType::Mat4;
            if (uniform.rows == 3 && uniform.columns == 3) return MaterialParameterType::Mat3;
            return MaterialParameterType::Unknown;
        }
        const u32 components = uniform.columns == 0 ? 1u : uniform.columns;
        if (is_float) {
            switch (components) {
                case 1: return MaterialParameterType::Float;
                case 2: return MaterialParameterType::Vec2;
                case 3: return MaterialParameterType::Vec3;
                case 4: return MaterialParameterType::Vec4;
                default: return MaterialParameterType::Unknown;
            }
        }
        if (components == 1) {
            if (uniform.scalar == slang::ShaderScalarType::Int32) return MaterialParameterType::Int;
            if (uniform.scalar == slang::ShaderScalarType::UInt32) return MaterialParameterType::UInt;
        }
        return MaterialParameterType::Unknown;
    }

    // One named, settable parameter in a material's uniform block: where it lives (`offset`/`size` into
    // the UBO) and its typed shape. `default_bytes` seeds a fresh instance's value block.
    struct MaterialParameter {
        string name;
        u32 offset = 0;
        u32 size = 0;
        MaterialParameterType type = MaterialParameterType::Unknown;
        vector<byte> default_bytes;
    };

    // One named texture slot a material can bind a texture into, and where it binds (`set`/`binding`).
    // `sampler_binding`/`has_sampler` capture a companion separate-sampler slot when the shader uses
    // the split texture+sampler model rather than a combined image sampler.
    struct MaterialTextureSlot {
        string name;
        u32 set = 0;
        u32 binding = 0;
        RHI::BindingType type = RHI::BindingType::SampledTexture;
    };

    // A render pipeline built for one attachment configuration. Materials cache these lazily because a
    // dynamic-rendering pipeline bakes in its color/depth formats (see :Material's pipeline_for()).
    struct MaterialPipelineVariant {
        vector<RHI::Format> color_formats;
        RHI::Format depth_format = RHI::Format::Undefined;
        RHI::RenderPipelineHandle pipeline{};
    };

    // The shared, reflection-derived half of a material: compiled shader modules, the RHI layouts its
    // instances bind against, the byte size + parameter map of its uniform block, and its texture slots.
    // One template backs many instances. Owned + created by Renderer::create_material_template().
    struct MaterialTemplateResource {
        MaterialTemplateHandle handle{};
        string label;

        // Kept so pipelines/reflection stay available for hot-reload and lazy pipeline creation.
        Core::Slang::Shader shader;

        // Populated only for templates created from a `.slang` source (create_material_template_from_source):
        // owns the source + a per-permutation compiled-shader cache (see :ShaderVariant), so a variant
        // (SKINNED, ALPHA_TEST, ...) is compiled at most once and a hot-reload can recompile from source.
        // `hot_reloadable` is true exactly when `variant_cache` has a file-backed source to watch.
        Core::Slang::ShaderVariantCache variant_cache;
        bool hot_reloadable = false;

        RHI::ShaderModuleHandle vertex_module{};
        RHI::ShaderModuleHandle fragment_module{};
        string vertex_entry_point;
        string fragment_entry_point;
        bool has_fragment = false;
        vector<RHI::BindGroupLayoutHandle> bind_group_layouts;
        // The register-space / set index each entry of bind_group_layouts targets, in the same order —
        // so an instance can build one bind group per set and bind it at the right index.
        vector<u32> bind_group_layout_sets;
        RHI::PipelineLayoutHandle pipeline_layout{};

        // The material's uniform block (default constant buffer). Instances allocate a UBO of this size.
        u32 uniform_block_size = 0;
        // The set/binding the uniform block occupies (materials assume a single material UBO).
        u32 uniform_set = 0;
        u32 uniform_binding = 0;
        bool has_uniform_block = false;

        vector<MaterialParameter> parameters;
        vector<MaterialTextureSlot> texture_slots;

        // Lazily-built pipelines, one per attachment configuration seen at draw time.
        vector<MaterialPipelineVariant> pipeline_variants;

        bool alive = false;
    };

    // One texture bound to a material instance's slot, resolved to the RHI view + sampler at bind time.
    struct MaterialTextureBinding {
        u32 binding = 0;
        TextureHandle texture{};
    };

    // Per-frame-in-flight GPU state for a material instance: the UBO its parameters upload into and one
    // bind group per template descriptor set (referencing that UBO + the instance's textures).
    // N-buffered so writing this frame's parameters never races a previous, still-in-flight frame's copy
    // (see plans/async-submission-model.md).
    struct MaterialInstanceFrame {
        RHI::BufferHandle uniform_buffer{};
        vector<RHI::BindGroupHandle> bind_groups;   // one per template bind_group_layout, in set order
        // Per-frame because each frame slot owns its own UBO/bind groups: a parameter or texture change
        // must re-upload/rebuild for every slot, once each, not just the slot in use this frame.
        bool uniform_dirty = true;
        bool bind_groups_dirty = true;
    };

    // A drawable material: a template reference, a CPU-side copy of the uniform block (staged into the
    // per-frame UBO when dirty), the textures bound to each slot, and the N-buffered GPU state. Owned +
    // created by Renderer::create_material_instance().
    struct MaterialInstanceResource {
        MaterialInstanceHandle handle{};
        MaterialTemplateHandle material_template{};
        string label;

        vector<byte> uniform_values;               // mirror of the UBO, seeded from parameter defaults
        vector<MaterialTextureBinding> textures;    // one entry per template texture slot

        vector<MaterialInstanceFrame> frames;       // size == max_frames_in_flight
        bool alive = false;
    };

} // namespace SFT::Renderer
