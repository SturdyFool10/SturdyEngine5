#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <string>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/Displacement/DisplacementPlanner.hpp>
#include <Renderer/Displacement/DisplacementTypes.hpp>
#include <Renderer/Handles.hpp>

using std::span;
using std::string;
using std::vector;

// Renderer-facing description of a displaced material (plans/displacement-system.md section 8). The
// Renderer resolves a Displacement::DisplacementPlan from the tier + the live device, compiles the
// matching shader variant of Shaders/gbuffer_geometry_displaced.slang, uploads the height texture (and,
// when the plan needs one, the packed min/max hierarchy), and keeps the per-frame uniforms fresh.
namespace SFT::Renderer {

    struct DisplacedMaterialDesc {
        /// Single-channel height in [0, 1], row-major, width * height values.
        span<const f32> heights;
        u32 width = 0;
        u32 height = 0;
        /// Repeat addressing for the heightfield (shader `displacement_wrap` + hierarchy build).
        bool wrap = true;

        Displacement::HardwareTier tier = Displacement::HardwareTier::High;
        Displacement::DisplacementRequest request{};

        /// World-space amplitude of the heightfield.
        f32 height_scale = 0.1f;
        /// Normalized height the base mesh sits at (1 = depth map, 0 = height map).
        f32 reference_height = 1.0f;
        /// World size one UV tile covers.
        glm::vec2 tile_size{1.0f, 1.0f};
        /// 0 = shade with normal_texture, 1 = shade with the normal derived from the height gradient.
        f32 normal_mix = 1.0f;

        glm::vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};
        f32 metallic = 0.0f;
        f32 roughness = 0.6f;

        const char *label = nullptr;
        /// Shader template; must expose the same entry points/uniforms as gbuffer_geometry_displaced.slang.
        string shader_path = "Shaders/gbuffer_geometry_displaced.slang";
        string module_name = "gbuffer_geometry_displaced";
    };

    /// What create_displaced_material produced. The plan is what actually got built, including every
    /// downgrade against the request (DisplacementPlan::downgrades says why).
    struct DisplacedMaterial {
        MaterialTemplateHandle material_template{};
        MaterialInstanceHandle instance{};
        TextureHandle height_texture{};
        /// Invalid (default) when the plan has no hierarchical block.
        TextureHandle hierarchy_texture{};
        Displacement::DisplacementPlan plan{};
        /// Levels in the hierarchy including the implicit level 0; 1 = none bound.
        u32 hierarchy_levels = 1;
        /// True when the variant writes displaced depth; such a material is kept out of the z prepass and
        /// drawn with a standard (Less, depth-writing) test in the G-buffer pass.
        bool writes_displaced_depth = false;
        /// True when meshes for this material should be pre-tessellated (see create_displaced_mesh).
        bool needs_pre_tessellated_mesh = false;
    };

    /// Internal bookkeeping, one per displaced material, so per-frame uniform refresh and teardown do not
    /// need the caller to hand the resources back.
    struct DisplacedMaterialRecord {
        DisplacedMaterial material{};
        glm::vec3 last_camera_position{0.0f};
        bool camera_written = false;
        u32 steps = 0;
    };

} // namespace SFT::Renderer
