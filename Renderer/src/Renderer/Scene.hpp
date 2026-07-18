#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include "Handles.hpp"

using std::span;

namespace SFT::Renderer {

    // Camera data for one rendered view. `view` transforms world -> view space and `projection`
    // transforms view -> clip space; callers own coordinate-system policy, but the Renderer receives the
    // final matrices so gameplay/editor code can use any camera controller it wants.
    struct CameraView {
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};
        glm::vec3 world_position{0.0f, 0.0f, 0.0f};
        f32 near_plane = 0.01f;
        f32 far_plane = 1000.0f;
        f32 vertical_fov_radians = 1.0471975512f; // 60 degrees; informational until projection helpers land.
    };

    // Per-object render submission in world space. This is intentionally resource-handle based: scene/ECS
    // ownership stays above the Renderer, while the Renderer receives a compact drawable packet it can later
    // cull, sort, instance, batch, or feed into GPU-driven visibility.
    struct SceneRenderable {
        MeshHandle mesh{};
        MaterialInstanceHandle material{};
        glm::mat4 world_transform{1.0f};
        u64 stable_id = 0;     // Optional persistent object id for picking/history/GPU feedback.
        u32 visibility_mask = ~0u;
        u32 sort_key = 0;      // Optional caller hint; Renderer-owned sort keys can replace/augment this later.
    };

    struct SceneLighting {
        glm::vec3 ambient_radiance{0.02f, 0.02f, 0.02f};
        f32 exposure = 1.0f;
    };

    // Renderer-facing lowering of Engine's programmable graph recipe. These are semantic choices,
    // never resource descriptions: the renderer still owns formats, target lifetimes, synchronization
    // and the concrete low-level RenderGraph pass callbacks.
    enum class ToneMappingOperator : u8 {
        None,
        Reinhard,
        Aces,
        Exponential,
    };

    struct RenderGraphSettings {
        bool render_scene = true;
        bool deferred_lighting = true;
        bool tone_mapping = true;
        bool debug_overlay = false;
        f32 resolution_scale = 1.0f;
        glm::vec4 background_color{0.01f, 0.015f, 0.025f, 1.0f};
        f32 background_intensity = 1.0f;
        ToneMappingOperator tone_mapping_operator = ToneMappingOperator::Aces;
        f32 tone_mapping_exposure = 1.0f;
        f32 tone_mapping_white_point = 1.0f;
        f32 tone_mapping_saturation = 1.0f;
    };

    // Default transient target layout for the deferred path, expressed in RHI formats so the render graph
    // can create everything through dynamic rendering. The first implementation still shades through the
    // simple geometry path, but these defaults establish the G-buffer contract future material variants
    // should target: albedo, world/view normal, material properties, HDR lighting, and depth.
    struct DeferredTargetFormats {
        RHI::Format albedo = RHI::Format::RGBA8Unorm;
        // Octahedral-encoded (Shaders/sturdy_common.slang's encodeOctahedralNormal/
        // decodeOctahedralNormal) — a unit normal needs only 2 components this way, halving
        // bandwidth versus a raw xyz-in-RGBA16F encode for the same per-channel precision.
        RHI::Format normal = RHI::Format::RG16Float;
        RHI::Format material = RHI::Format::RGBA8Unorm;
        RHI::Format lighting = RHI::Format::RGBA16Float;
        RHI::Format depth = RHI::Format::D32Float;
    };

    // One camera's view of a scene. The immediate goal is a high-level submission seam; future renderer
    // passes can hang culling settings, shadow views, fog/sky, reflection probes, and post-process volumes
    // off this structure without exposing RHI details to game code.
    struct RenderViewDesc {
        CameraView camera{};
        SceneLighting lighting{};
        span<const SceneRenderable> renderables{};
        u32 visibility_mask = ~0u;
        DeferredTargetFormats deferred_formats{};
        RenderGraphSettings render_graph{};
        UString debug_label;
    };

    // High-level per-frame renderer entry point: one surface, timing, and the scene view to render into it.
    struct RenderFrameDesc {
        Core::RenderSurfaceHandle surface{};
        Core::FrameInput frame{};
        RenderViewDesc view{};
    };

    // Per-draw scene constants consumed by baseline geometry materials. Kept to 128 bytes so it fits
    // Vulkan's guaranteed minimum push-constant budget; larger high-fidelity payloads should move to
    // view/object buffers once culling, skinning, and material variants need more data.
    struct SceneDrawConstants {
        glm::mat4 view_projection{1.0f};
        glm::mat4 model{1.0f};
    };

    // GPU-facing per-view payload for deferred rendering. This becomes the stable set-0 view buffer used
    // by G-buffer, lighting, shadow, and post-processing passes; fields are vec4/mat4 aligned so the same
    // layout maps cleanly to GLSL/HLSL/Slang constant-buffer rules.
    struct SceneViewGpuData {
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};
        glm::mat4 view_projection{1.0f};
        glm::vec4 camera_world_position_near{0.0f, 0.0f, 0.0f, 0.01f};
        glm::vec4 ambient_radiance_exposure{0.02f, 0.02f, 0.02f, 1.0f};
        glm::vec4 far_fov_object_count_time{1000.0f, 1.0471975512f, 0.0f, 0.0f};
    };

    // GPU-facing per-object table entry. Deferred geometry passes can index this by draw/instance id, and
    // GPU culling can compact/reorder these entries before indirect submission without changing material
    // instances or mesh resources.
    struct SceneObjectGpuData {
        glm::mat4 model{1.0f};
        glm::mat4 previous_model{1.0f};
        glm::vec4 id_sort_visibility_flags{0.0f, 0.0f, 0.0f, 0.0f};
    };

} // namespace SFT::Renderer
