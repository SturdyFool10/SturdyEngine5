#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include "SpectralPathTracing.hpp"
#include "Handles.hpp"
#include "RenderTarget.hpp"
#include "Light.hpp"
#include "TextAtlas.hpp"

using std::span;

namespace SFT::Renderer {


    struct CameraView {
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};
        glm::vec3 world_position{0.0f, 0.0f, 0.0f};
        f32 near_plane = 0.01f;
        f32 far_plane = 1000.0f;
        f32 vertical_fov_radians = 1.0471975512f;


        glm::mat4 previous_view_projection{1.0f};
    };


    struct SceneRenderable {
        MeshHandle mesh{};
        MaterialInstanceHandle material{};
        glm::mat4 world_transform{1.0f};
        u64 stable_id = 0;
        u32 visibility_mask = ~0u;
        u32 sort_key = 0;
    };

    struct SceneLighting {
        glm::vec3 ambient_radiance{0.02f, 0.02f, 0.02f};
        f32 exposure = 1.0f;
        DirectionalLight sun{};
        std::vector<SpotLight> spot_lights;
        std::vector<PointLight> point_lights;
    };


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


    enum class PostProcessStage : u8 {
        BeforeBloom,
        AfterBloomBeforeToneMap,
    };


    struct CustomPostProcessEffect {
        std::string shader_path;
        std::string module_name;
        std::string fragment_entry_point = "fragmentMain";
        std::vector<std::byte> push_constants;
        UString label;
        PostProcessStage stage = PostProcessStage::BeforeBloom;
    };

    struct LogicalRenderGraphTexture {
        u32 index = ~0u;
        /// Converts the `LogicalRenderGraphTexture` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] explicit constexpr operator bool() const noexcept { return index != ~0u; }
        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(LogicalRenderGraphTexture, LogicalRenderGraphTexture) noexcept = default;
    };

    enum class CustomGraphPassKind : u8 {
        RasterEffect,
        ComputeEffect,
        Copy,
    };

    struct CustomComputeEffect {
        std::string shader_path;
        std::string module_name;
        std::string compute_entry_point = "computeMain";
        std::vector<std::byte> push_constants;
        UString label;
    };

    struct CustomGraphPass {
        CustomGraphPassKind kind = CustomGraphPassKind::RasterEffect;
        PostProcessStage stage = PostProcessStage::BeforeBloom;
        LogicalRenderGraphTexture input{};
        LogicalRenderGraphTexture output{};
        CustomPostProcessEffect raster{};
        CustomComputeEffect compute{};
        UString label;
    };


    struct CustomGraphProgram {
        u32 texture_count = 0;
        std::vector<CustomGraphPass> passes;
        std::vector<LogicalRenderGraphTexture> outputs;
        LogicalRenderGraphTexture deferred_scene_output{};
        LogicalRenderGraphTexture anti_aliasing_output{};
        LogicalRenderGraphTexture bloom_output{};
        LogicalRenderGraphTexture before_bloom_presentation_output{};
        LogicalRenderGraphTexture after_bloom_presentation_output{};
    };


    using UiOverlayPrepareFn = std::function<Core::RendererResult(
        RHI::RhiDevice &, RHI::CommandEncoder &, glm::vec2, Core::RenderSurfaceHandle, u32,
        std::vector<RHI::BufferHandle> &, TextAtlasRetiredResources &)>;
    using UiOverlayDrawFn = std::function<Core::RendererResult(
        RHI::RenderPassEncoder &, glm::vec2, Core::RenderSurfaceHandle, u32)>;

    struct UiOverlayHooks {
        UiOverlayPrepareFn prepare;
        UiOverlayDrawFn draw;


        f32 hdr_reference_white_scale = 1.0f;
        /// Converts the `UiOverlayHooks` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] explicit operator bool() const noexcept;
    };


    struct FrameTimingSnapshot {


        bool has_data = false;
        std::vector<std::pair<std::string, f64>> gpu_pass_timings_ms;
        std::vector<std::pair<std::string, f64>> cpu_pass_timings_ms;
        std::vector<std::pair<std::string, f64>> cpu_stage_timings_ms;
    };

    struct RenderGraphSettings {
        bool render_scene = true;
        SpectralPathTracingSettings spectral_path_tracing{};
        bool shadows = true;
        bool ambient_occlusion = true;
        bool bloom = true;
        bool tone_mapping = true;
        bool debug_overlay = false;


        bool draw_overlay_text = true;


        bool wait_for_completion = false;
        f32 resolution_scale = 1.0f;
        glm::vec4 background_color{0.01f, 0.015f, 0.025f, 1.0f};
        f32 background_intensity = 1.0f;


        u32 shadow_atlas_size = 4096;
        u32 shadow_cascade_count = 4;
        f32 shadow_max_distance = 250.0f;
        f32 shadow_cascade_split_lambda = 0.65f;
        f32 shadow_cascade_blend = 0.10f;
        f32 shadow_depth_bias = 0.75f;
        f32 shadow_slope_bias = 1.0f;
        f32 shadow_normal_bias = 0.75f;
        u32 max_shadowed_spot_lights = 8;
        u32 max_shadowed_point_lights = 4;
        bool shadow_contact_hardening = true;


        bool contact_shadows = true;
        f32 contact_shadow_distance = 0.5f;
        f32 contact_shadow_thickness = 0.05f;
        u32 contact_shadow_steps = 8;
        f32 gtao_radius = 1.0f;
        f32 gtao_falloff = 0.8f;
        f32 gtao_thickness = 0.15f;
        f32 gtao_intensity = 1.0f;
        u32 gtao_quality = 2;
        u32 msaa_samples = 1;
        u32 post_process_aa = 1;
        f32 aa_subpixel_quality = 0.75f;
        f32 aa_edge_threshold = 0.125f;
        f32 bloom_threshold = 0.0f;
        f32 bloom_soft_knee = 0.5f;
        f32 bloom_intensity = 0.04f;
        f32 bloom_scatter = 0.7f;
        f32 bloom_downsample_ratio = 1.61803398875f;
        u32 bloom_max_levels = 12;
        ToneMappingOperator tone_mapping_operator = ToneMappingOperator::Agx;
        f32 tone_mapping_exposure = 1.0f;
        f32 tone_mapping_white_point = 1.0f;
        f32 tone_mapping_saturation = 1.0f;


        bool tone_mapping_hdr_output = false;


        Core::HdrColorSpaceMode tone_mapping_hdr_color_space = Core::HdrColorSpaceMode::Hdr10St2084;
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
        CustomGraphProgram custom_graph;
        UiOverlayHooks ui_overlay;
    };


    struct DeferredTargetFormats {
        RHI::Format albedo = RHI::Format::RGBA8Unorm;


        RHI::Format normal = RHI::Format::RG16Float;
        RHI::Format material = RHI::Format::RGBA8Unorm;

        RHI::Format emissive = RHI::Format::RGBA16Float;
        RHI::Format scene_color = RHI::Format::RGBA16Float;


        RHI::Format motion = RHI::Format::RG16Float;
        RHI::Format depth = RHI::Format::D32Float;
    };


    struct RenderViewDesc {
        CameraView camera{};
        SceneLighting lighting{};
        span<const SceneRenderable> renderables{};


        span<const SceneRenderable> gizmo_renderables{};
        u32 visibility_mask = ~0u;
        DeferredTargetFormats deferred_formats{};
        RenderGraphSettings render_graph{};
        UString debug_label;
    };


    struct RenderFrameDesc {
        Core::RenderSurfaceHandle surface{};
        OffscreenRenderTargetHandle offscreen_target{};
        Core::FrameInput frame{};
        RenderViewDesc view{};
    };


    struct SceneDrawConstants {
        glm::mat4 view_projection{1.0f};
        glm::mat4 model{1.0f};
    };


    struct ObjectHistoryDrawConstants {
        u32 object_index = 0;
    };


    struct SceneViewGpuData {
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};
        glm::mat4 view_projection{1.0f};


        glm::mat4 previous_view_projection{1.0f};
        glm::vec4 camera_world_position_near{0.0f, 0.0f, 0.0f, 0.01f};
        glm::vec4 ambient_radiance_exposure{0.02f, 0.02f, 0.02f, 1.0f};
        glm::vec4 far_fov_object_count_time{1000.0f, 1.0471975512f, 0.0f, 0.0f};
    };


    struct SceneObjectGpuData {
        glm::mat4 model{1.0f};
        glm::mat4 previous_model{1.0f};
        glm::vec4 id_sort_visibility_flags{0.0f, 0.0f, 0.0f, 0.0f};
    };

} // namespace SFT::Renderer
