#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#pragma endregion

#include <Async/Async.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

using std::string;
using std::vector;

// Mesh-shader geometry path for displaced materials (Shaders/displacement_mesh.slang; see
// plans/displacement-system.md). Everything here is plain data owned by the Renderer; the logic lives in
// RendererDisplacementMesh.cpp.
namespace SFT::Renderer {

    /// Per-template configuration of the mesh-shader geometry path.
    struct DisplacementMeshSettings {
        /// Target on-screen edge length of a refined triangle, in pixels (TierProfile::target_edge_pixels).
        f32 target_edge_pixels = 16.0f;
        /// Refinement ceiling; clamped to what the device's mesh output limits admit and to the shader's 8.
        u32 max_level = 8;
        /// Same macro list the material template's own shader was compiled with (SFT_HF_ALGORITHM, ...).
        /// The mesh path must be built with SFT_HF_ALGORITHM=0 (the geometry already carries the
        /// displacement); the Renderer forces that regardless of what is passed.
        vector<Core::Slang::ShaderMacro> macros;
        string shader_path = "Shaders/displacement_mesh.slang";
        string module_name = "displacement_mesh";
    };

    /// std140 image of `DisplacementViewConstants` in displacement_mesh.slang (one uniform-buffer slot per view).
    /// Matrices are glm (column-major) exactly like the scene draw push constants, which the shader treats
    /// identically.
    struct DisplacementMeshViewGpuConstants {
        glm::mat4 view_projection{1.0f};
        glm::vec4 camera_position{0.0f};
        glm::vec4 frustum_planes[6]{};
        f32 pixels_per_unit_at_one = 0.0f;
        f32 padding[3]{};
    };
    static_assert(sizeof(DisplacementMeshViewGpuConstants) == 192,
                  "DisplacementMeshViewGpuConstants must match the layout of DisplacementViewConstants");

    /// Push-constant image of `DisplacementDrawConstants` (Task|Mesh stages).
    struct DisplacementMeshGpuDrawConstants {
        glm::mat4 model{1.0f};
        u32 first_index = 0;
        u32 vertex_base = 0;
        u32 triangle_count = 0;
        f32 bounds_scale = 1.0f;
        f32 target_edge_pixels = 16.0f;
        u32 max_level = 8;
        u32 padding[2]{};
    };
    static_assert(sizeof(DisplacementMeshGpuDrawConstants) == 96,
                  "DisplacementMeshGpuDrawConstants must match the layout of DisplacementDrawConstants");

    /// One pipeline built for a (formats, depth, culling, ...) combination.
    struct DisplacementMeshPipelineVariant {
        vector<RHI::Format> color_formats;
        RHI::Format depth_format = RHI::Format::Undefined;
        bool depth_only = false;
        f32 depth_bias = 0.0f;
        f32 slope_bias = 0.0f;
        RHI::CullMode cull_mode = RHI::CullMode::Back;
        RHI::FrontFace front_face = RHI::FrontFace::CounterClockwise;
        RHI::SampleCount samples = RHI::SampleCount::X1;
        RHI::RenderPipelineHandle pipeline{};
    };

    /// GPU objects + settings for one material template. `built == false` after a device loss: the
    /// settings survive, the handles are rebuilt lazily.
    struct DisplacementMeshTemplateState {
        DisplacementMeshSettings settings;
        bool built = false;

        RHI::ShaderModuleHandle task_module{};
        RHI::ShaderModuleHandle mesh_module{};
        RHI::ShaderModuleHandle fragment_module{};
        RHI::ShaderModuleHandle depth_only_fragment_module{};
        RHI::PipelineLayoutHandle pipeline_layout{};
        bool has_depth_only_fragment = false;

        vector<DisplacementMeshPipelineVariant> variants;
    };

    /// Renderer-wide state shared by every template: the set-1 layout and the settings map.
    struct DisplacementMeshResources {
        RHI::BindGroupLayoutHandle draw_layout{};
        std::unordered_map<u64, DisplacementMeshTemplateState> templates;
    };

    /// Per-frame, per-view-submission data the recording side needs (pointed at by RenderItem). Lives in
    /// FrameSubmission, so it is per window and per frame and needs no cross-window synchronisation.
    ///
    /// The Vulkan RHI bridge has no dynamic-offset descriptors, so per-view data gets its own uniform-buffer
    /// slot and its own bind group (created on first use, cached by view-projection matrix), while the small
    /// per-draw part travels as push constants.
    struct DisplacementMeshFrame {
        glm::vec3 lod_camera_position{0.0f};
        /// viewport_height / (2 * tan(fov / 2)): pixels per world unit at unit distance.
        f32 pixels_per_unit_at_one = 0.0f;

        RHI::BufferHandle constants_buffer{};
        u32 constants_stride = 0;
        u32 constants_capacity = 0; // view slots
        std::atomic<bool> overflow_reported{false};

        struct ViewEntry {
            glm::mat4 view_projection{1.0f};
            RHI::BindGroupHandle group{};
        };
        struct ViewCache {
            RHI::BufferHandle vertex_arena{};
            RHI::BufferHandle index_arena{};
            vector<ViewEntry> entries;
        };
        Async::Mutex<ViewCache> views;
        vector<RHI::BindGroupHandle> *transient_bind_groups = nullptr;
    };

} // namespace SFT::Renderer
