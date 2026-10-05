#pragma once

#include <Foundation/Foundation.hpp>
#include <RenderSettings/RenderSettings.hpp>

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


    using ToneMappingOperator = RenderSettings::ToneMappingOperator;
    using AgxLook = RenderSettings::AgxLook;
    using FisheyeMode = RenderSettings::FisheyeMode;
    using RestirGiSettings = RenderSettings::RestirGiSettings;
    using MotionBlurSettings = RenderSettings::MotionBlurSettings;
    using CameraEmulationSettings = RenderSettings::CameraEmulationSettings;
    using AutoExposureSettings = RenderSettings::AutoExposureSettings;
    using TemporalUpscalerSettings = RenderSettings::TemporalUpscalerSettings;
    using ScreenSpaceGiSettings = RenderSettings::ScreenSpaceGiSettings;
    using LightingSettings = RenderSettings::LightingSettings;
    using VolumetricFogSettings = RenderSettings::VolumetricFogSettings;
    using ReflectionSettings = RenderSettings::ReflectionSettings;

    enum class PostProcessStage : u8 {
        BeforeBloom,
        AfterBloomBeforeToneMap,
        /// After tone mapping, on the display-encoded image. Raster effects only; the last one writes
        /// the presentation target.
        AfterToneMap,
    };


    enum class FullscreenBlend : u8 {
        None,
        ConstantMix,
        /// The effect outputs premultiplied colour and alpha; draws with `src + dst * (1 - src.a)` over what is in the
        /// target (use a load-op that keeps it). This is how the display-encoded overlay layer is put over a frame.
        PremultipliedOver,
    };

    struct CustomPostProcessEffect {
        std::string shader_path;
        std::string module_name;
        std::string fragment_entry_point = "fragmentMain";
        std::vector<std::byte> push_constants;
        UString label;
        PostProcessStage stage = PostProcessStage::BeforeBloom;
        /// How many `extraTexture<N>` inputs the caller supplies (see `CustomGraphPass::extra_inputs`).
        u32 extra_input_count = 0;
        /// How the effect's output combines with what is already in the target. `None` overwrites it.
        /// `ConstantMix` draws with blend `src * c + dst * (1 - c)` where `c` is `blend_constant`
        /// (bloom's upsample accumulates this way); use a load-op that keeps the target's contents.
        FullscreenBlend blend = FullscreenBlend::None;
        f32 blend_constant = 1.0f;
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
        /// Extra sampled textures for a raster effect, bound as `extraTexture0..N`.
        std::vector<LogicalRenderGraphTexture> extra_inputs;
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


    /// What an overlay's optional `prepare` step gets, before its pass is drawn. Use it to upload data (text
    /// atlases, vertex buffers) with `encoder`, create frame-transient GPU objects, and even add render-graph
    /// nodes of your own (a glow, a blur) ahead of the overlay. Anything created here must be handed back through
    /// the out-params so the renderer frees it once the frame has finished on the GPU.
    struct OverlayPrepareContext {
        RHI::RhiDevice &device;
        RHI::CommandEncoder &encoder;
        RenderGraph &graph;
        /// The presentation extent in pixels.
        glm::vec2 viewport{0.0f};
        Core::RenderSurfaceHandle surface{};
        u32 frame_slot_index = 0;
        std::vector<RHI::BufferHandle> &transient_buffers;
        TextAtlasRetiredResources &retired_text_atlas_resources;
        std::vector<RHI::BindGroupHandle> &transient_bind_groups;
        /// Graph textures the overlay's draw samples. Declared as reads of the overlay pass so the graph keeps
        /// their memory alive (and out of aliasing) until it has run.
        std::vector<RenderGraphTextureHandle> &sampled_textures;
    };

    /// What an overlay pass draws into: a render pass already targeting the frame (see `OverlayPass`), with
    /// viewport and scissor set to the whole presentation extent.
    struct OverlayPassContext {
        RHI::RenderPassEncoder &pass;
        Core::Extent2D extent{};
        /// Format of the target being drawn into (pipelines you create must match it).
        RHI::Format format = RHI::Format::Undefined;
        bool hdr_output = false;
        Core::RenderSurfaceHandle surface{};
        u32 frame_slot_index = 0;
    };

    using OverlayPrepareFn = std::function<Core::RendererResult(OverlayPrepareContext &)>;
    using OverlayDrawFn = std::function<Core::RendererResult(OverlayPassContext &)>;

    /// A named piece of application drawing composited on top of the finished frame: a HUD, on-screen UI, gizmos,
    /// diagnostics. The engine ships none of them; this is the one hook an application (or a UI library) uses.
    ///
    /// Overlays run after tone mapping, in order, so they are never tone mapped, bloomed or anti-aliased. They
    /// draw straight onto the display-encoded frame. On an HDR display (and for an overlay-only frame on a transparent
    /// surface) the renderer instead gives them a linear RGBA16F layer, encodes it to the display at the reference
    /// white level and composites it over the frame, so an overlay authored in sRGB looks the same in SDR and HDR;
    /// `OverlayPassContext::format` always tells you what you are drawing into.
    struct OverlayPass {
        std::string name;
        /// Optional; runs once per frame before the frame's passes are declared.
        OverlayPrepareFn prepare;
        OverlayDrawFn draw;
        /// HDR only: scales the reference-white level this overlay is composed at (UI brightness). The first overlay
        /// in the list decides for the frame.
        f32 hdr_reference_white_scale = 1.0f;
    };


    struct FrameTimingSnapshot {


        bool has_data = false;
        std::vector<std::pair<std::string, f64>> gpu_pass_timings_ms;
        std::vector<std::pair<std::string, f64>> cpu_pass_timings_ms;
        std::vector<std::pair<std::string, f64>> cpu_stage_timings_ms;
    };

    /// What the renderer is asked to do this frame: the shared `RenderSettings::FrameSettings` (as the caller set them, with
    /// the engine's per-frame adjustments applied: features without a scene switched off, values the engine computes such
    /// as jitter and overscan filled in) plus what only exists at run time.
    struct RenderGraphSettings {
        RenderSettings::FrameSettings frame{};
        bool render_scene = true;
        SpectralPathTracingSettings spectral_path_tracing{};
        /// The frame is presented to an HDR display, in this encoding.
        bool hdr_output = false;
        Core::HdrColorSpaceMode hdr_color_space = Core::HdrColorSpaceMode::Hdr10St2084;
        std::vector<CustomPostProcessEffect> custom_post_processes;
        CustomGraphProgram custom_graph;
        /// Application overlays, drawn in order on the finished frame (see `OverlayPass`).
        std::vector<OverlayPass> overlay_passes;
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
        /// Non-zero for a GPU-skinned mesh: its previous-frame positions come from the skinning pass's
        /// previous-position buffer instead of being the current ones moved by the previous model matrix.
        u32 skinned = 0;
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
