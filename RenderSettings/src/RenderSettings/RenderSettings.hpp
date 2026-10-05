#pragma once

// The one definition of every per-frame rendering setting.
//
// The renderer reads these structs, `Engine::RenderGraph` stores them, the C ABI copies to and from them in one place, and
// the CxxApi binds them directly. There is no second copy anywhere, so a new setting is one field here plus the code that
// reads it.
//
// The header obeys the CxxApi plain-data rules (plans/cxx-api.md) so `bindgen` can parse it on its own:
//   * it includes only <cstdint>;
//   * fields are `bool`, fixed-width integers, `float`, fixed C arrays, other structs from this header, or enums;
//   * every enum is `enum class Name : std::uint32_t` with explicit values that are never reused;
//   * structs are standard-layout and trivially copyable (checked in RenderSettingsTest);
//   * default member initializers carry the engine defaults (`*_defaults()` functions in the CxxApi hand them to Rust).
//
// Colours are linear RGB unless a comment says otherwise. Distances are world units (metres by convention).

#include <cstdint>

namespace SFT::RenderSettings {

    // ---- scene ------------------------------------------------------------------------------------------------------

    /// What produces the scene image.
    enum class SceneIntegrator : std::uint32_t {
        RasterDeferred = 0,
        ShadowOnly = 1,
        ReflectionOnly = 2,
        AmbientOcclusionOnly = 3,
        ShadowAndTransmission = 4,
        FullPathTracing = 5,
    };

    struct SceneSettings {
        bool enabled = true;
        SceneIntegrator integrator = SceneIntegrator::RasterDeferred;
        std::uint32_t path_samples_per_pixel = 1;
        std::uint32_t path_max_bounces = 8;
        std::uint32_t path_russian_roulette_start_bounce = 3;
        std::uint32_t caustic_photon_count = 262144;
        float caustic_gather_radius = 0.075f;
        float wavelength_min_nm = 380.0f;
        float wavelength_max_nm = 780.0f;
        /// When false the camera's own clear colour is used behind the scene; when true, `background_color` (RGBA).
        bool use_background_color = false;
        float background_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float background_intensity = 1.0f;
    };

    // ---- shadows ----------------------------------------------------------------------------------------------------

    /// Single-frame shadow and G-buffer debug visualizations. They replace the shaded result so cascade allocation,
    /// transitions and shadow-map footprint can be judged directly; bloom and tone mapping are bypassed while one is on.
    enum class ShadowDebugView : std::uint32_t {
        None = 0,
        /// Tints each pixel by the cascade it samples.
        CascadeIndex = 1,
        /// Tints by cascade and highlights the cross-fade band between two cascades.
        CascadeFade = 2,
        /// The shadow-map texel grid of the selected cascade in world space.
        ShadowTexelGrid = 3,
        /// The shadow-atlas UV the pixel samples.
        ShadowUv = 4,
        /// The normal-offset receiver depth in the selected cascade.
        ReceiverDepth = 5,
        /// The depth stored in the directional atlas at the receiver's unfiltered UV.
        AtlasDepth = 6,
        /// Signed receiver-minus-atlas depth at the unfiltered UV (grey = equal).
        DepthDelta = 7,
        /// The receiver's normal-offset displacement in shadow texels.
        NormalBias = 8,
        /// The receiver-plane d(depth)/d(shadow UV) correction.
        ReceiverPlaneGradient = 9,
        /// The unfiltered hardware depth-comparison result.
        HardComparison = 10,
        /// Fixed-radius PCF before cascade blending or contact shadows.
        Pcf = 11,
        /// The complete rasterized directional CSM result, including cascade blending.
        DirectionalCsm = 12,
        /// The screen-space contact-shadow term on its own.
        ContactShadow = 13,
        /// The directional CSM and contact terms multiplied together.
        CombinedSunVisibility = 14,
        // The views below stay available when shadows are off: they separate a CSM artifact from material, G-buffer,
        // AO or direct-lighting bugs.
        /// Raw hardware depth from the G-buffer.
        GbufferDepth = 15,
        /// World position as repeating 10 m bands.
        WorldPosition = 16,
        /// Decoded G-buffer normal, remapped to [0, 1].
        GbufferNormal = 17,
        GbufferAlbedo = 18,
        GbufferRoughness = 19,
        GbufferMetallic = 20,
        /// Material-authored ambient occlusion.
        MaterialAmbientOcclusion = 21,
        /// Ambient/indirect lighting after ambient occlusion is applied.
        AmbientLighting = 22,
        /// Sun N dot L before any shadow visibility.
        SunNdotL = 23,
        /// Sun BRDF contribution before any shadow visibility.
        UnshadowedSunLighting = 24,
        /// The screen-space ambient-occlusion buffer exactly as lighting consumes it (raw search when denoise is off).
        ScreenSpaceAmbientOcclusion = 25,
        /// How many local lights each pixel's light cluster holds (blue = none, red = `max_lights_per_cluster`).
        LightClusterOccupancy = 26,
        /// The indirect specular term (environment and screen-space reflections) on its own, before exposure.
        IndirectSpecular = 27,
        /// Screen-space reflection confidence: green where a reflection was found, black where the environment is used.
        ReflectionConfidence = 28,
    };

    struct ShadowSettings {
        bool enabled = true;
        /// Edge size of the shared spot/point shadow atlas. Directional cascades are sized by `cascade_resolutions`.
        std::uint32_t atlas_size = 4096;
        std::uint32_t cascade_count = 4;
        float max_distance = 250.0f;
        float cascade_split_lambda = 0.65f;
        /// Fraction of a cascade's view-space depth range spent cross-fading into the next cascade.
        float cascade_blend = 0.10f;
        float depth_bias = 0.75f;
        float slope_bias = 1.0f;
        /// Per-cascade shadow-map edge resolution, near cascade first. Far cascades at half the near one keep world texel
        /// size within ~2x of cascade 0 for a quarter of the memory each. Clamped to powers of two, forced non-increasing,
        /// halved uniformly if the packed atlas would exceed the device limit.
        std::uint32_t cascade_resolutions[4] = {2048u, 1024u, 1024u, 1024u};
        /// PCF filter radius in shadow texels of the sampled cascade (texel-relative, so softness is resolution-independent).
        float filter_radius_texels = 2.0f;
        /// Receiver normal-offset magnitude in shadow texels at normal incidence.
        float normal_bias = 0.75f;
        ShadowDebugView debug_view = ShadowDebugView::None;
        std::uint32_t max_shadowed_spot_lights = 8;
        std::uint32_t max_shadowed_point_lights = 4;
        /// PCSS-style contact hardening. Off by default: PCSS can amplify grazing-angle depth disagreement.
        bool contact_hardening = false;
        /// Screen-space short-range sun occlusion for detail smaller than a shadow texel (feet, cables). Clamped in strength
        /// and faded with distance so it never reads as a second, harder shadow.
        bool contact_shadows = true;
        float contact_shadow_distance = 0.5f;
        float contact_shadow_thickness = 0.05f;
        std::uint32_t contact_shadow_steps = 8;
        /// Maximum darkening the contact term may apply, in [0, 1].
        float contact_shadow_intensity = 0.85f;
        /// View-space distance at which the contact term has faded out.
        float contact_shadow_fade_distance = 40.0f;
    };

    // ---- ambient occlusion ------------------------------------------------------------------------------------------

    enum class AmbientOcclusionQuality : std::uint32_t {
        /// 1 slice x 3 steps.
        Low = 0,
        /// 2 x 4.
        Medium = 1,
        /// 3 x 6, the XeGTAO paper's practical configuration.
        High = 2,
        /// 4 x 8.
        Ultra = 3,
    };

    struct AmbientOcclusionSettings {
        bool enabled = true;
        /// World-space radius of the horizon search: the near field (contact darkening, crevices), not long-range occlusion.
        float radius = 1.0f;
        AmbientOcclusionQuality quality = AmbientOcclusionQuality::High;
        /// Blend toward unoccluded. 1 = full strength.
        float intensity = 1.0f;
        /// Fraction of `radius` over which an occluder fades out (no ring at the radius edge).
        float falloff_range = 0.615f;
        /// Thin-occluder compensation in [0, 0.7]. 0 by default: thick assumptions draw black halos around thin geometry.
        float thin_occluder_compensation = 0.0f;
        /// Contrast curve on the visibility term (XeGTAO's FinalValuePower).
        float final_value_power = 2.2f;
        /// Exponent of the sample-distance distribution; 2 concentrates taps near the shaded pixel.
        float sample_distribution_power = 2.0f;
        /// The 5x5 edge-aware spatial denoiser. Part of the algorithm; turn off only to inspect raw output.
        bool denoise = true;
    };

    // ---- anti-aliasing, bloom, tone mapping -------------------------------------------------------------------------

    enum class PostProcessAntiAliasing : std::uint32_t {
        None = 0,
        Fxaa = 1,
        ConservativeMorphological = 2,
    };

    struct AntiAliasingSettings {
        /// 1 = off; 2, 4 or 8 for deferred MSAA.
        std::uint32_t msaa_samples = 1;
        PostProcessAntiAliasing post_process = PostProcessAntiAliasing::Fxaa;
        float subpixel_quality = 0.75f;
        float edge_threshold = 0.125f;
    };

    struct BloomSettings {
        bool enabled = true;
        float threshold = 0.0f;
        float soft_knee = 0.5f;
        float intensity = 0.04f;
        float scatter = 0.7f;
        float downsample_ratio = 1.61803398875f;
        std::uint32_t max_levels = 12;
    };

    enum class ToneMappingOperator : std::uint32_t {
        None = 0,
        Reinhard = 1,
        Exponential = 2,
        Agx = 3,
        HermiteSpline = 4,
        PsychoV = 5,
    };

    enum class AgxLook : std::uint32_t {
        None = 0,
        Punchy = 1,
        Golden = 2,
    };

    struct HermiteSplineSettings {
        float toe_strength = 0.5f;
        float toe_length = 0.5f;
        float shoulder_strength = 2.0f;
        float shoulder_length = 0.5f;
        float shoulder_angle = 1.0f;
    };

    struct PsychoVSettings {
        float highlights = 1.0f;
        float shadows = 1.0f;
        float contrast = 1.0f;
        float purity_scale = 1.0f;
        float gamut_compression = 1.0f;
        bool gamut_compression_use_bt2020 = true;
        float compression = 0.0f;
        float adapted_gray_bt709[3] = {0.18f, 0.18f, 0.18f};
        float background_gray_bt709[3] = {0.18f, 0.18f, 0.18f};
    };

    struct ToneMappingSettings {
        bool enabled = true;
        ToneMappingOperator operation = ToneMappingOperator::Agx;
        float exposure = 1.0f;
        float white_point = 1.0f;
        float saturation = 1.0f;
        float hdr_paper_white_nits = 203.0f;
        float hdr_peak_nits = 1000.0f;
        AgxLook agx_look = AgxLook::None;
        HermiteSplineSettings hermite_spline{};
        PsychoVSettings psycho_v{};
    };

    // ---- global illumination ----------------------------------------------------------------------------------------

    enum class RestirGiQuality : std::uint32_t {
        Low = 0,
        Medium = 1,
        High = 2,
    };

    /// Which denoiser resolves ReSTIR GI's reservoirs. The vendor values are seams for future SDK integrations and fall
    /// back to SVGF (with a one-time warning) until one is implemented.
    enum class RestirGiDenoiser : std::uint32_t {
        None = 0,
        Svgf = 1,
        DlssRayReconstruction = 2,
        FsrRedstone = 3,
    };

    /// ReSTIR GI: one ray-traced indirect-diffuse bounce per traced pixel, resampled across time and neighbours.
    struct RestirGiSettings {
        bool enabled = false;
        /// Low/Medium trace one ray per 2x2 block, High every pixel; Low also caps spatial reuse to 1 tap and SVGF to 2
        /// a-trous iterations (Medium/High: 2 taps, 3 iterations). The explicit settings below can only lower those caps.
        RestirGiQuality quality = RestirGiQuality::Medium;
        std::uint32_t spatial_reuse_samples = 2;
        float spatial_reuse_radius_px = 12.0f;
        std::uint32_t temporal_history_max = 20;
        float max_ray_distance = 60.0f;
        /// Damping (0-1) on last frame's lit colour read back at hit points: multi-bounce over a few frames. 0 disables it.
        float multi_bounce_feedback = 0.5f;
        float intensity = 1.0f;
        RestirGiDenoiser denoiser = RestirGiDenoiser::Svgf;
        std::uint32_t svgf_atrous_iterations = 3;
        float svgf_temporal_alpha = 0.2f;
        float svgf_phi_normal = 128.0f;
        float svgf_phi_depth = 1.0f;
        float svgf_phi_luminance = 4.0f;
        bool show_debug_reservoirs = false;
    };

    /// Screen-space indirect lighting with a visibility bitmask (SSILVB). Runs on every backend; ReSTIR GI is used instead
    /// when it is enabled and available.
    struct ScreenSpaceGiSettings {
        bool enabled = false;
        float intensity = 1.0f;
        /// How far, in world units, a surface can light its neighbours.
        float radius = 2.0f;
        /// Assumed thickness of what the depth buffer shows: light can pass behind thinner things.
        float thickness = 0.25f;
        /// Directions per pixel and depth taps per direction (both ways), at half resolution.
        std::uint32_t slice_count = 2;
        std::uint32_t step_count = 8;
        /// Weight of the newest frame in the temporal accumulation.
        float temporal_alpha = 0.1f;
        /// Brightest scene-linear luminance a texel may contribute (tames sun glints).
        float max_radiance = 64.0f;
    };

    // ---- local lights and participating media -----------------------------------------------------------------------

    /// How point and spot lights reach pixels and fog.
    struct LightingSettings {
        /// Sort lights into a grid of view-space cells ("clusters": screen tiles x exponential depth slices) so each pixel and
        /// fog cell only evaluates the lights that can reach it. Without clustering every pixel loops over every light,
        /// which is why the unclustered path is limited to 8 spot + 8 point lights.
        bool clustered = true;
        /// Screen-space edge of a cluster tile, in pixels of the render resolution.
        std::uint32_t cluster_tile_px = 64;
        /// Depth slices between the near plane and `cluster_max_distance`, distributed exponentially.
        std::uint32_t cluster_depth_slices = 24;
        /// Lights beyond this view distance are not clustered (they still light the scene through the unclustered path's
        /// first 8 + 8 when clustering is off).
        float cluster_max_distance = 300.0f;
        /// Most local lights a frame uploads; the nearest win when there are more.
        std::uint32_t max_lights = 1024;
        /// Most lights one cluster can list; the rest are dropped for that cluster (visible as a hard edge, so raise it
        /// rather than living with it).
        std::uint32_t max_lights_per_cluster = 96;
    };

    /// Froxel volumetric fog: participating media in a camera-aligned grid lit by the sun (with cascade shadows), the sky
    /// ambient and every clustered point/spot light (with their shadow maps), integrated front to back and applied to the
    /// scene. Gives light shafts and flashlight beams.
    /// Indirect specular light: what glossy and metallic surfaces reflect besides the lights themselves.
    ///
    /// Two layers. The *environment* is the sky (the atmosphere's sky-view LUT) prefiltered for every roughness, so every
    /// surface reflects something and metals are never black. *Screen-space reflections* replace it where the reflected
    /// ray hits something on screen: one deterministic ray per pixel along the specular direction, walked through a
    /// min-depth pyramid, with roughness taken from a blurred chain of last frame's lit image (cone tracing). Nothing is
    /// sampled randomly, so the result is stable: no noise, no temporal smear. Off-screen and occluded reflections fall
    /// back to the environment, dimmed by specular occlusion so enclosed spaces do not reflect the sky.
    struct ReflectionSettings {
        bool environment = true;
        /// Scales the environment reflection (1 = the sky as it is rendered).
        float environment_intensity = 1.0f;
        /// Darkens environment reflections where ambient occlusion says the hemisphere is blocked.
        bool specular_occlusion = true;
        bool screen_space = true;
        float screen_space_intensity = 1.0f;
        /// Surfaces rougher than this (perceptual roughness) use only the environment; they fade out over the last 20%.
        float max_roughness = 0.6f;
        /// Hierarchical-depth steps per ray (each step crosses a cell of any pyramid level).
        std::uint32_t max_steps = 48;
        /// How far behind the depth buffer a surface is assumed to extend, as a fraction of its view depth.
        float thickness = 0.02f;
        /// Scales how fast the specular cone widens with distance (1 = the GGX lobe; higher blurs rough reflections more).
        float glossy_blur = 1.0f;
    };

    struct VolumetricFogSettings {
        bool enabled = false;
        /// Extinction coefficient (per world unit) at `base_height`. 0.01 is light haze, 0.1 is thick smoke.
        float density = 0.02f;
        /// How fast density falls off above `base_height` (per world unit; 0 = uniform everywhere).
        float height_falloff = 0.05f;
        float base_height = 0.0f;
        /// Fraction of extinction that is scattering, per channel (1 = white, non-absorbing fog).
        float albedo[3] = {0.9f, 0.9f, 0.9f};
        /// Henyey-Greenstein anisotropy: 0 scatters evenly, towards 1 bright forward halos around lights and the sun.
        float anisotropy = 0.6f;
        /// Light the medium emits on its own (glowing smoke), per unit length.
        float emissive[3] = {0.0f, 0.0f, 0.0f};
        float sun_intensity = 1.0f;
        float ambient_intensity = 1.0f;
        float local_light_intensity = 1.0f;
        /// Fog is computed this far from the camera; beyond it the last slice's transmittance and in-scatter are reused.
        float max_distance = 96.0f;
        /// Screen-space edge of a froxel in pixels, and depth slices up to `max_distance`.
        std::uint32_t tile_px = 8;
        std::uint32_t slice_count = 64;
        /// Exponent of the slice distribution: 1 = linear, larger puts more slices near the camera.
        float slice_distribution = 2.0f;
        /// Weight of the new frame in the temporal reprojection (lower = smoother and slower to react).
        float temporal_blend = 0.1f;
        bool sun_shadows = true;
        bool local_light_shadows = true;
        /// Procedural density variation (0 = smooth fog), its feature size in world units and its drift per second.
        float noise_strength = 0.0f;
        float noise_scale = 6.0f;
        float wind[3] = {0.4f, 0.0f, 0.2f};
    };

    // ---- post: motion, camera, exposure, upscaling ------------------------------------------------------------------

    struct MotionBlurSettings {
        bool enabled = false;
        float intensity = 1.0f;
        float shutter_angle_degrees = 180.0f;
        std::uint32_t tile_size_px = 20;
        std::uint32_t sample_count = 8;
        float max_blur_radius_px = 32.0f;
        float background_foreground_weight_bias = 0.5f;
        bool camera_motion_only = false;
    };

    /// How the fisheye is produced.
    enum class FisheyeMode : std::uint32_t {
        /// A pass after rendering resamples an overscanned frame: exact for any geometry, shades the extra pixels.
        PostProcess = 0,
        /// The camera passes' vertex stage warps clip positions: rendered directly at output resolution, approximate for
        /// large triangles, unsupported for displaced/mesh-shader materials (see sturdy_space.slang).
        VertexWarp = 1,
    };

    /// Body-camera / camcorder look, applied last in display space. One cheap fullscreen pass.
    struct CameraEmulationSettings {
        bool enabled = false;
        /// Barrel distortion 0..1: how much extra field of view the corners see. The post-process fisheye renders the frame
        /// `1 + fisheye_strength` times wider (capped by the 2x render-scale limit) so the centre stays 1:1.
        float fisheye_strength = 0.35f;
        /// Radial red/blue separation as a fraction of the frame half-size at the corners.
        float chromatic_aberration = 0.004f;
        float vignette_strength = 0.35f;
        /// Luminance-dependent temporal sensor noise (stronger in shadows), 0 = none.
        float sensor_noise = 0.03f;
        /// Local-contrast sharpening; camcorder over-sharpening starts around 0.3.
        float sharpen = 0.25f;
        float saturation = 0.9f;
        float contrast = 1.08f;
        /// Multiplies the image (white balance); 1,1,1 = neutral.
        float tint[3] = {1.0f, 1.0f, 1.0f};
        /// Darkens a rounded-rectangle "camera housing" frame around the edges, 0 = none.
        float housing = 0.0f;
        FisheyeMode fisheye_mode = FisheyeMode::PostProcess;
        /// Set by the engine every frame (written values are overwritten): the overscan applied for the post-process fisheye.
        float overscan = 1.0f;
        /// Set by the engine every frame: the strength the vertex-warp lens is applied with.
        float lens_strength = 0.0f;
    };

    /// Histogram auto-exposure: meters the scene-linear image before tone mapping and adapts over time. The tone-mapping
    /// exposure stays a manual multiplier on top.
    struct AutoExposureSettings {
        bool enabled = false;
        /// Metered luminance range, as log2(scene-linear luminance).
        float min_log2_luminance = -9.0f;
        float max_log2_luminance = 5.0f;
        /// The darkest / brightest fraction of pixels ignored, so glints and black regions do not steer exposure.
        float low_percent = 0.40f;
        float high_percent = 0.95f;
        /// Luminance the metered average is mapped to (middle grey).
        float key_value = 0.18f;
        /// Exposure compensation in stops.
        float compensation_ev = 0.0f;
        float min_exposure = 0.03f;
        float max_exposure = 32.0f;
        /// Adaptation rates per second when the scene gets darker / brighter.
        float adapt_up_speed = 2.5f;
        float adapt_down_speed = 1.0f;
        /// 0 = every pixel counts equally, 1 = the frame edges count for nothing.
        float center_weight = 0.4f;
    };

    /// Temporal anti-aliasing and upscaling: a jittered camera accumulated into an output-resolution image, so the scene can
    /// render below output resolution (`FrameSettings::resolution_scale` < 1). The slot a vendor upscaler replaces.
    struct TemporalUpscalerSettings {
        bool enabled = false;
        /// Blend weight of a new frame where a sample lands exactly on the output pixel.
        float current_frame_weight = 0.1f;
        /// History reconstruction: 0 = bilinear (soft), 1 = Catmull-Rom (sharp).
        float sharpness = 0.6f;
        /// Set by the engine every frame: the screen-UV shift of this frame's and last frame's projection jitter.
        float jitter_uv[2] = {0.0f, 0.0f};
        float previous_jitter_uv[2] = {0.0f, 0.0f};
    };

    // ---- the frame --------------------------------------------------------------------------------------------------

    enum class ExecutionMode : std::uint32_t {
        /// The frame is submitted and the CPU moves on.
        FireAndForget = 0,
        /// The CPU waits for the GPU to finish the frame (capture tools, tests, deterministic stepping).
        WaitForCompletion = 1,
    };

    /// Every setting a frame is rendered with.
    struct FrameSettings {
        SceneSettings scene{};
        ShadowSettings shadows{};
        AmbientOcclusionSettings ambient_occlusion{};
        AntiAliasingSettings anti_aliasing{};
        BloomSettings bloom{};
        ToneMappingSettings tone_mapping{};
        RestirGiSettings restir_gi{};
        ScreenSpaceGiSettings screen_space_gi{};
        LightingSettings lighting{};
        VolumetricFogSettings volumetric_fog{};
        ReflectionSettings reflections{};
        MotionBlurSettings motion_blur{};
        CameraEmulationSettings camera_emulation{};
        AutoExposureSettings auto_exposure{};
        TemporalUpscalerSettings temporal_upscaler{};
        /// Collect GPU pass timestamps and CPU stage timings (`Renderer::last_frame_timings`). Costs a query pool.
        bool frame_timings = false;
        ExecutionMode execution_mode = ExecutionMode::FireAndForget;
        /// Render resolution as a fraction of the output resolution (0.25..2).
        float resolution_scale = 1.0f;
    };

} // namespace SFT::RenderSettings
