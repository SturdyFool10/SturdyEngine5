module;

#pragma region Imports
#include <span>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#pragma endregion

export module Sturdy.Renderer:Scene;

import Sturdy.Foundation;
import Sturdy.Core;
import :Handles;

using std::span;

export namespace SFT::Renderer {

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

    // One camera's view of a scene. The immediate goal is a high-level submission seam; future renderer
    // passes can hang culling settings, shadow views, fog/sky, reflection probes, and post-process volumes
    // off this structure without exposing RHI details to game code.
    struct RenderViewDesc {
        CameraView camera{};
        SceneLighting lighting{};
        span<const SceneRenderable> renderables{};
        u32 visibility_mask = ~0u;
        const char *debug_label = nullptr;
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

    inline constexpr u32 scene_draw_push_constant_size = sizeof(SceneDrawConstants);

} // namespace SFT::Renderer
