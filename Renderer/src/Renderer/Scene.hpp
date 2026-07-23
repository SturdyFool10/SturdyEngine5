#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <span>
#include <string>
#include <vector>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include "Handles.hpp"
#include "Light.hpp"

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
        DirectionalLight sun{};
        std::vector<SpotLight> spot_lights;
        std::vector<PointLight> point_lights;
    };

    // Renderer-facing lowering of Engine's programmable graph recipe. These are semantic choices,
    // never resource descriptions: the renderer still owns formats, target lifetimes, synchronization
    // and the concrete low-level RenderGraph pass callbacks. ACES is deliberately not offered here —
    // see Engine::ToneMappingOperator's doc comment for why.
    enum class ToneMappingOperator : u8 {
        None,
        Reinhard,
        Exponential,
        Agx,
        HermiteSpline,
        PsychoV,
    };

    enum class AgxLook : u8 {
        None,
        Punchy,
        Golden,
    };



    // Where a custom effect sits relative to the built-in bloom/tonemap pipeline. Both stages carry
    // scene-linear HDR; AfterBloomBeforeToneMap additionally has bloom already composited in, so an
    // effect there sees exactly what tonemapping is about to see. Kept to two values deliberately —
    // see Scene.hpp/RendererLifecycle.cpp's staged-HDR-effects docs before adding AfterToneMap: that
    // stage needs display-linear-vs-encoded-output semantics this contract doesn't have yet.
    enum class PostProcessStage : u8 {
        BeforeBloom,
        AfterBloomBeforeToneMap,
    };

    // Consumer-supplied one-input fullscreen HDR effect. The shader contract is intentionally small:
    // vertexMain/fragmentMain, one Texture2D named sourceTexture, one sourceSampler, and optional push
    // constants. Renderer owns compiled GPU objects; consumers own only source code and parameter bytes.
    // Effects run in declaration order within their stage; order across different stages follows the
    // fixed pipeline (every BeforeBloom effect before bloom, every AfterBloomBeforeToneMap effect after).
    struct CustomPostProcessEffect {
        std::string shader_path;
        std::string module_name;
        std::string fragment_entry_point = "fragmentMain";
        std::vector<std::byte> push_constants;
        UString label;
        PostProcessStage stage = PostProcessStage::BeforeBloom;
    };



    struct RenderGraphSettings {
        bool render_scene = true;
        bool shadows = true;
        bool bloom = true;
        bool tone_mapping = true;
        bool debug_overlay = false;
        // Default submission is asynchronous. Explicit completion waits are reserved for tooling,
        // captures, deterministic tests, or other caller-requested synchronization points.
        bool wait_for_completion = false;
        f32 resolution_scale = 1.0f;
        glm::vec4 background_color{0.01f, 0.015f, 0.025f, 1.0f};
        f32 background_intensity = 1.0f;

        // Raster shadow stack. The atlas is divided into 8x8 cells: directional cascades receive
        // 2x2 cells each and local-light faces receive one. Local lights are importance-ranked when
        // demand exceeds the fixed budget, keeping memory and worst-case raster cost deterministic.
        u32 shadow_atlas_size = 4096;
        u32 shadow_cascade_count = 4;
        f32 shadow_max_distance = 250.0f;
        f32 shadow_cascade_split_lambda = 0.65f;
        f32 shadow_cascade_blend = 0.10f;
        f32 shadow_depth_bias = 0.75f;
        f32 shadow_slope_bias = 1.0f;
        f32 shadow_normal_bias = 0.75f; // Receiver offset measured in shadow texels.
        u32 max_shadowed_spot_lights = 8;
        u32 max_shadowed_point_lights = 4;
        bool shadow_contact_hardening = true;
        f32 bloom_threshold = 1.0f;
        f32 bloom_soft_knee = 0.5f;
        f32 bloom_intensity = 0.08f;
        f32 bloom_scatter = 0.7f;
        u32 bloom_max_levels = 6;
        ToneMappingOperator tone_mapping_operator = ToneMappingOperator::Agx;
        f32 tone_mapping_exposure = 1.0f;
        f32 tone_mapping_white_point = 1.0f;
        f32 tone_mapping_saturation = 1.0f;

        // Consumer-requested nits (Engine::ToneMappingSettings). tone_mapping_hdr_output itself is
        // NOT consumer-set here — Renderer derives it per-frame from the surface's actual
        // PresentationSettings::hdr_enabled (see RendererLifecycle.cpp's render_frame_rhi) right
        // before recording the tonemap pass, since only Renderer knows the swapchain's real color
        // space at that point.
        bool tone_mapping_hdr_output = false;
        f32 tone_mapping_hdr_paper_white_nits = 203.0f;
        f32 tone_mapping_hdr_peak_nits = 1000.0f;

        AgxLook agx_look = AgxLook::None;

        f32 hermite_toe_strength = 0.5f;
        f32 hermite_toe_length = 0.5f;
        f32 hermite_shoulder_strength = 2.0f;
        f32 hermite_shoulder_length = 0.5f;
        f32 hermite_shoulder_angle = 1.0f;

        f32 psychov_highlights = 1.0f;
        f32 psychov_shadows = 1.0f;
        f32 psychov_contrast = 1.0f;
        f32 psychov_purity_scale = 1.0f;
        f32 psychov_gamut_compression = 1.0f;
        bool psychov_gamut_compression_use_bt2020 = true;
        f32 psychov_compression = 0.0f;
        glm::vec3 psychov_adapted_gray_bt709{0.18f};
        glm::vec3 psychov_background_gray_bt709{0.18f};
        std::vector<CustomPostProcessEffect> custom_post_processes;
    };

    // Default transient target layout for the deferred path, expressed in RHI formats so the render graph
    // can create everything through dynamic rendering. The first implementation still shades through the
    // simple geometry path, but these defaults establish the G-buffer contract future material variants
    // should target: albedo, world/view normal, material properties, HDR scene color, and depth.
    struct DeferredTargetFormats {
        RHI::Format albedo = RHI::Format::RGBA8Unorm;
        // Octahedral-encoded (Shaders/sturdy_common.slang's encodeOctahedralNormal/
        // decodeOctahedralNormal) — a unit normal needs only 2 components this way, halving
        // bandwidth versus a raw xyz-in-RGBA16F encode for the same per-channel precision.
        RHI::Format normal = RHI::Format::RG16Float;
        RHI::Format material = RHI::Format::RGBA8Unorm;
        RHI::Format scene_color = RHI::Format::RGBA16Float;
        RHI::Format depth = RHI::Format::D32Float;
    };

    // One camera's view of a scene. The immediate goal is a high-level submission seam; future renderer
    // passes can hang culling settings, fog/sky, reflection probes, and post-process volumes off this
    // structure without exposing RHI details to game code.
    struct RenderViewDesc {
        CameraView camera{};
        SceneLighting lighting{};
        span<const SceneRenderable> renderables{};
        // Always-on debug markers (e.g. light-position icospheres) — drawn through a separate,
        // single-color-target forward pass over the HDR background, never visibility-mask-filtered
        // (they're a dev aid, not gameplay-visibility-relevant).
        span<const SceneRenderable> gizmo_renderables{};
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

    // GPU-facing per-view payload for scene rendering. This becomes the stable set-0 view buffer used
    // by geometry and post-processing passes; fields are vec4/mat4 aligned so the same
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
