#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <array>
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
#include <Renderer/SpectralPathTracing.hpp>
#include <Renderer/Handles.hpp>
#include <Renderer/RenderGraph.hpp>
#include <Renderer/RenderTarget.hpp>
#include <Renderer/Light.hpp>
#include <Renderer/TextAtlas.hpp>

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
        bool casts_shadows = true;
        RHI::CullMode cull_mode = RHI::CullMode::Back;
        RHI::FrontFace front_face = RHI::FrontFace::CounterClockwise;
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


    // `prepare` receives the current frame's RenderGraph (nodes may be appended to it, e.g. to bloom a
    // flagged UI element — see UI::UiRenderer::prepare()'s own doc comment) plus two extra transient-
    // resource out-params (bind groups, glow-bloom output textures) beyond the buffer/text-atlas ones
    // it already had, all following the same "caller owns/destroys transient resources this returns"
    // convention the existing out-params use.
    using UiOverlayPrepareFn = std::function<Core::RendererResult(
        RHI::RhiDevice &, RHI::CommandEncoder &, RenderGraph &, glm::vec2, Core::RenderSurfaceHandle, u32,
        std::vector<RHI::BufferHandle> &, TextAtlasRetiredResources &,
        std::vector<RHI::BindGroupHandle> &, std::vector<RenderGraphTextureHandle> &)>;
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

    struct RestirGiSettings {
        bool enabled = false;
        u32 quality = 1; // 0=Low, 1=Medium, 2=High
        u32 spatial_reuse_samples = 4;
        f32 spatial_reuse_radius_px = 24.0f;
        u32 temporal_history_max = 20;
        f32 max_ray_distance = 60.0f;
        f32 multi_bounce_feedback = 0.5f;
        f32 intensity = 1.0f;
        u32 denoiser = 1; // 0=None, 1=Svgf, 2=DlssRayReconstruction, 3=FsrRedstone
        u32 svgf_atrous_iterations = 5;
        f32 svgf_temporal_alpha = 0.2f;
        f32 svgf_phi_normal = 128.0f;
        f32 svgf_phi_depth = 1.0f;
        f32 svgf_phi_luminance = 4.0f;
        bool show_debug_reservoirs = false;
    };

    struct MotionBlurSettings {
        bool enabled = false;
        f32 intensity = 1.0f;
        f32 shutter_angle_degrees = 180.0f;
        u32 tile_size_px = 20;
        u32 sample_count = 8;
        f32 max_blur_radius_px = 32.0f;
        f32 background_foreground_weight_bias = 0.5f;
        bool camera_motion_only = false;
    };

    struct RenderGraphSettings {
        bool render_scene = true;
        SpectralPathTracingSettings spectral_path_tracing{};
        RestirGiSettings restir_gi{};
        MotionBlurSettings motion_blur{};
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


        /// Per-cascade directional shadow-map edge resolution, near cascade first.
        std::array<u32, 4> shadow_cascade_resolutions{2048u, 1024u, 1024u, 1024u};


        /// PCF filter radius in shadow texels of the sampled cascade.
        f32 shadow_filter_radius_texels = 2.0f;


        /// Shadow debug visualization selector; see `Engine::ShadowDebugView`.
        u32 shadow_debug_view = 0;
        u32 max_shadowed_spot_lights = 8;
        u32 max_shadowed_point_lights = 4;


        /// Optional PCSS-style contact hardening. Kept opt-in so the production default is the
        /// stable single-frame PCF path; PCSS is more sensitive to grazing-angle depth disagreement.
        bool shadow_contact_hardening = false;


        bool contact_shadows = true;
        f32 contact_shadow_distance = 0.5f;
        f32 contact_shadow_thickness = 0.05f;
        u32 contact_shadow_steps = 8;
        f32 contact_shadow_intensity = 0.85f;
        f32 contact_shadow_fade_distance = 40.0f;
        /// World-space radius of the GTAO horizon search. This is a near-field/contact radius, not
        /// a stand-in for long-range indirect occlusion.
        f32 ambient_occlusion_radius = 1.0f;


        /// 0=Low (1 slice x 3 steps), 1=Medium (2 x 4), 2=High (3 x 6, the paper's practical
        /// configuration), 3=Ultra (4 x 8).
        u32 ambient_occlusion_quality = 2;


        /// Blend of the AO term toward fully unoccluded. 1 = full strength.
        f32 ambient_occlusion_intensity = 1.0f;


        /// Fraction of the radius over which a tap fades out, so occluders lose influence smoothly
        /// instead of switching off at the radius edge.
        f32 ambient_occlusion_falloff_range = 0.615f;


        /// Thin-occluder compensation. Screen-space depth is a height field and cannot know how
        /// thick a surface is; raising this discards taps behind the shaded point sooner. Defaults
        /// to 0 because over-thick assumptions produce black halos around thin geometry, and
        /// missing some occlusion reads far better than occlusion that is visibly wrong.
        f32 ambient_occlusion_thin_occluder_compensation = 0.0f;


        /// Contrast curve applied to the visibility term (XeGTAO's FinalValuePower).
        f32 ambient_occlusion_final_value_power = 2.2f;


        /// Exponent of the normalized sample-distance distribution. 2 concentrates taps near the
        /// shaded pixel, where small crevices and contact occlusion live.
        f32 ambient_occlusion_sample_distribution_power = 2.0f;


        /// Runs the 5x5 edge-aware spatial denoiser. The denoiser is part of the algorithm, not a
        /// polish step; disabling it exposes the raw horizon-search result for A/B validation.
        bool ambient_occlusion_denoise = true;
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
