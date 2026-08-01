#pragma once

#include <Foundation/src/Foundation.hpp>

#include <expected>
#include <functional>
#include <optional>
#include <utility>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <Engine/RenderGraphModule.hpp>

namespace SFT::Engine {

    // High-level, built-in operations rather than GPU pass kinds. Engine consumers choose what the
    // frame should do; Renderer still chooses attachments, formats, barriers, queues and allocation.
    // Controls submission synchronization, not graph construction. A graph remains a lazy CPU recipe
    // until Engine::render() records it. FireAndForget submits and returns after presentation is queued;
    // WaitForCompletion additionally waits for that frame's GPU fence.
    enum class RenderGraphExecutionMode : u8 {
        FireAndForget,
        WaitForCompletion,
    };

    enum class RenderFeature : u8 {
        Scene,
        Shadows,
        AmbientOcclusion,
        AntiAliasing,
        Bloom,
        ToneMapping,
        DebugOverlay,
    };

    enum class SceneIntegrator : u8 {
        RasterDeferred,
        ShadowOnly,
        ReflectionOnly,
        AmbientOcclusionOnly,
        ShadowAndTransmission,
        FullPathTracing,
    };

    enum class AmbientOcclusionQuality : u8 {
        Low,
        Medium,
        High,
        Ultra,
    };

    // Deliberately spatial-only. Temporal AA and supersample AA are not part of the built-in
    // renderer contract; MSAA is configured separately below and can be combined with either
    // post-process edge treatment.
    enum class PostProcessAntiAliasing : u8 {
        None,
        Fxaa,
        ConservativeMorphological,
    };

    // ACES is deliberately not offered here. It has well-known highlight/hue-shift problems (see
    // e.g. Troy Sobotka's writeups on ACES RRT/ODT skew) that this engine does not want to steer
    // consumers toward by default. The underlying acesFittedTonemap() shader helper still exists in
    // Shaders/sturdy_common.slang for anyone who wants to wire it up by hand — it just isn't one of
    // the operators this enum can select.
    enum class ToneMappingOperator : u8 {
        None,
        Reinhard,
        Exponential,
        Agx,
        HermiteSpline,
        PsychoV,
    };

    // AgX (Troy Sobotka, shipped as Blender 4.0's default view transform) applies a fixed log2
    // encode + sigmoid through inset/outset matrices; "looks" are the same optional post-curve
    // grades Blender itself offers.
    enum class AgxLook : u8 {
        None,
        Punchy,
        Golden,
    };

    struct AgxSettings {
        AgxLook look = AgxLook::None;
    };

    // Toe/shoulder-parameterized filmic curve evaluated as a cubic Hermite spline through the toe
    // and shoulder segments (the technique behind Unity's legacy "Custom" tonemapping curve and
    // John Hable's piecewise filmic curves) — fully user-shapeable rather than one fixed formula.
    struct HermiteSplineSettings {
        f32 toe_strength = 0.5f;      // [0, 1]: how hard the toe rolls off shadow detail.
        f32 toe_length = 0.5f;        // [0, 1]: how much of the input range the toe occupies.
        f32 shoulder_strength = 2.0f; // Stops of highlight compression above the linear midtone.
        f32 shoulder_length = 0.5f;   // [0, 1]: how much of the input range the shoulder occupies.
        f32 shoulder_angle = 1.0f;    // [0, 1]: blends the shoulder from a hard knee (0) to linear (1).
    };

    // A faithful port of clshortfuse/renodx's "psychov" test22 psychovisual tonemapper — an
    // observer-model operator: converts to Stockman-Sharpe LMS cone space, applies adaptive
    // MacLeod-Boynton-space highlight/shadow grading and hue-preserving log-domain compression, then
    // gamut-compresses against a chosen RGB hull. See Shaders/sturdy_tonemap_psychov.slang.
    struct PsychoVSettings {
        f32 highlights = 1.0f;      // >1 lifts, <1 rolls off highlights (per-cone grade).
        f32 shadows = 1.0f;         // >1 lifts, <1 rolls off shadows (per-cone grade).
        f32 contrast = 1.0f;        // Slope-normalized contrast into the compression shoulder.
        f32 purity_scale = 1.0f;    // MacLeod-Boynton purity applied before the contrast curve.
        f32 gamut_compression = 1.0f; // [0, 1]: how strongly out-of-hull colors bend back in-gamut.
        bool gamut_compression_use_bt2020 = true; // false compresses against the BT.709 hull instead.
        // Log-domain compression exponent; 0 auto-derives it from a simultaneous-contrast
        // dynamic-range reference (Kunkel & Reinhard 2010), matching upstream's default behavior.
        f32 compression = 0.0f;
        glm::vec3 adapted_gray_bt709{0.18f};     // Scene state the observer is assumed adapted to.
        glm::vec3 background_gray_bt709{0.18f};  // Display state that adapted gray should map to.
    };

    struct SceneRenderSettings {
        bool enabled = true;
        SceneIntegrator integrator = SceneIntegrator::RasterDeferred;
        u32 path_samples_per_pixel = 1;
        u32 path_max_bounces = 8;
        u32 path_russian_roulette_start_bounce = 3;
        u32 caustic_photon_count = 262144;
        f32 caustic_gather_radius = 0.075f;
        f32 wavelength_min_nm = 380.0f;
        f32 wavelength_max_nm = 780.0f;

        // Empty inherits Camera::clear_color(). This is display-independent linear color.
        std::optional<glm::vec4> background_color;
        // Multiplies the background before it enters the HDR scene target.
        f32 background_intensity = 1.0f;
    };

    // View-level policy for the renderer's rasterized shadow atlas. Defaults provide four stable
    // sun cascades and a bounded set of PCSS-filtered punctual-light shadows without requiring any
    // optional GPU feature. Atlas allocation and light prioritization remain renderer-owned.
    struct ShadowSettings {
        bool enabled = true;
        u32 atlas_size = 4096;
        u32 cascade_count = 4;
        f32 max_distance = 250.0f;
        f32 cascade_split_lambda = 0.65f;
        f32 cascade_blend = 0.10f;
        f32 depth_bias = 0.75f;
        f32 slope_bias = 1.0f;
        // Receiver offset in shadow texels, not world units. Keeping this scale-relative prevents
        // thin geometry from losing contact shadows as cascade/local-light coverage changes.
        f32 normal_bias = 0.75f;
        u32 max_shadowed_spot_lights = 8;
        u32 max_shadowed_point_lights = 4;
        bool contact_hardening = true;
    };

    struct AmbientOcclusionSettings {
        bool enabled = true;
        f32 radius = 1.0f;       // View/world-space hemisphere radius.
        f32 falloff = 0.8f;      // Fraction of radius at which distance attenuation begins.
        f32 thickness = 0.15f;   // Thin-occluder compensation in view-space units.
        f32 intensity = 1.0f;
        AmbientOcclusionQuality quality = AmbientOcclusionQuality::High;
    };

    struct AntiAliasingSettings {
        // Supported values are 1, 2, 4, and 8. Renderer clamps this to the color/depth sample
        // counts exposed by the active GPU.
        u32 msaa_samples = 1;
        PostProcessAntiAliasing post_process = PostProcessAntiAliasing::Fxaa;
        // FXAA sub-pixel reconstruction strength. 0 preserves maximum sharpness; 1 is smoothest.
        f32 subpixel_quality = 0.75f;
        // Local-luminance edge threshold shared by the two spatial post-AA modes.
        f32 edge_threshold = 0.125f;
    };

    // Fractional-resolution pyramid bloom based on the downsample/upsample approach popularized by the
    // Call of Duty: Advanced Warfare post-processing presentation. Bright pixels are
    // soft-thresholded into a progressively filtered pyramid, then accumulated back upward.
    struct BloomSettings {
        bool enabled = true;
        // Jimenez-style physical mode defaults to no threshold and a low energy-preserving mix.
        // Set threshold above zero to opt into the conventional artistic additive bright-pass mode.
        f32 threshold = 0.0f;
        f32 soft_knee = 0.5f; // [0, 1]: smooth transition below a positive threshold.
        f32 intensity = 0.04f;
        f32 scatter = 0.7f;   // [0, 1]: coarse-vs-fine reconstruction interpolation.
        // Irrational by default so successive level grids do not repeatedly align. Valid [1.25, 2.0].
        f32 downsample_ratio = 1.61803398875f;
        // Fractional golden-ratio reductions need more stages than power-of-two mips to cover a similar
        // radius. Renderer stops before either axis falls below four texels.
        u32 max_levels = 12;  // [1, 12], additionally limited by render resolution.
    };

    struct ToneMappingSettings {
        bool enabled = true;
        // AgX is the default now that ACES is no longer offered — see ToneMappingOperator's doc
        // comment above.
        ToneMappingOperator operation = ToneMappingOperator::Agx;
        f32 exposure = 1.0f;
        f32 white_point = 1.0f;
        f32 saturation = 1.0f;

        // HDR output nits. Whether the current frame actually *presents* in HDR depends on the
        // surface's own PresentationSettings::hdr_enabled (Engine::set_presentation_settings) — the
        // Renderer downgrades to an SDR-encoded output automatically when that isn't set, the same
        // capability-aware "request vs. what actually happened" pattern used elsewhere in the engine
        // (e.g. Window::enable_window_effect's Degraded outcome). These two nits values only shape
        // *how* an operator uses the room it's given once HDR output is active.
        f32 hdr_paper_white_nits = 203.0f; // ITU-R BT.2408 HDR reference white.
        f32 hdr_peak_nits = 1000.0f;

        AgxSettings agx{};
        HermiteSplineSettings hermite_spline{};
        PsychoVSettings psycho_v{};
    };

    struct DebugOverlayRenderSettings {
        bool enabled = false;
    };

    struct RenderGraphDescription {
        SceneRenderSettings scene{};
        ShadowSettings shadows{};
        AmbientOcclusionSettings ambient_occlusion{};
        AntiAliasingSettings anti_aliasing{};
        BloomSettings bloom{};
        ToneMappingSettings tone_mapping{};
        DebugOverlayRenderSettings debug_overlay{};
        RenderGraphExecutionMode execution_mode = RenderGraphExecutionMode::FireAndForget;

        // Scales scene targets, not the presentation surface or UI. The final presentation pass
        // automatically samples at output resolution, so dynamic resolution requires no resource work
        // from the consumer. Valid range is [0.1, 2.0].
        f32 resolution_scale = 1.0f;
    };

    enum class RenderGraphErrorCode : u8 {
        InvalidResolutionScale,
        InvalidBackgroundColor,
        InvalidSpectralPathTracingSettings,
        InvalidShadowSettings,
        InvalidAmbientOcclusionSettings,
        InvalidAntiAliasingSettings,
        InvalidBloomSettings,
        InvalidToneMappingSettings,
        InvalidFeatureCombination,
        InvalidPassGraph,
        InvalidFullscreenEffect,
        InvalidComputeEffect,
        InvalidGraphOutput,
        UnsupportedPassOrder,
    };

    struct RenderGraphError {
        RenderGraphErrorCode code = RenderGraphErrorCode::InvalidFeatureCombination;
        UString message;
    };

    using RenderGraphResult = std::expected<void, RenderGraphError>;

    // A reusable frame recipe. It is deliberately a small value object: keep one on a game camera,
    // copy it into a frame request, or reconstruct it through an eventual C/FFI description API.
    class RenderGraph {
      public:
        // Default construction preserves the existing behavior: an explicit standard module chain.
        RenderGraph() noexcept;
        explicit RenderGraph(RenderGraphDescription description) noexcept;
        RenderGraph(const RenderGraph &other);
        RenderGraph &operator=(const RenderGraph &other);
        RenderGraph(RenderGraph &&) noexcept = default;
        RenderGraph &operator=(RenderGraph &&) noexcept = default;

        [[nodiscard]] static RenderGraph standard() noexcept;
        [[nodiscard]] static RenderGraph overlay_only() noexcept;
        [[nodiscard]] static RenderGraph empty(RenderGraphDescription description = {}) noexcept;

        [[nodiscard]] const RenderGraphDescription &description() const noexcept;
        [[nodiscard]] RenderGraphDescription &description() noexcept;

        [[nodiscard]] const SceneRenderSettings &scene() const noexcept;
        [[nodiscard]] SceneRenderSettings &scene() noexcept;
        [[nodiscard]] const ShadowSettings &shadows() const noexcept;
        [[nodiscard]] ShadowSettings &shadows() noexcept;
        [[nodiscard]] const AmbientOcclusionSettings &ambient_occlusion() const noexcept;
        [[nodiscard]] AmbientOcclusionSettings &ambient_occlusion() noexcept;
        [[nodiscard]] const AntiAliasingSettings &anti_aliasing() const noexcept;
        [[nodiscard]] AntiAliasingSettings &anti_aliasing() noexcept;
        [[nodiscard]] const BloomSettings &bloom() const noexcept;
        [[nodiscard]] BloomSettings &bloom() noexcept;
        [[nodiscard]] const ToneMappingSettings &tone_mapping() const noexcept;
        [[nodiscard]] ToneMappingSettings &tone_mapping() noexcept;
        [[nodiscard]] const DebugOverlayRenderSettings &debug_overlay() const noexcept;
        [[nodiscard]] DebugOverlayRenderSettings &debug_overlay() noexcept;
        [[nodiscard]] RenderGraphExecutionMode execution_mode() const noexcept;

        [[nodiscard]] const std::vector<RenderGraphTextureDescription> &textures() const noexcept;
        [[nodiscard]] const std::vector<RenderGraphPassDescription> &passes() const noexcept;
        [[nodiscard]] RenderGraphTextureHandle presented_texture() const noexcept;
        // Stable destination selected by the unique Present node. Invalid means the frame's surface.
        [[nodiscard]] RenderTargetHandle selected_render_target() const noexcept;
        [[nodiscard]] bool contains_pass(RenderGraphPassKind kind) const noexcept;

        // Producer ancestry of the texture consumed by Present, in execution order and including the
        // Present node itself. Branches not contributing to presentation are intentionally omitted so
        // current Renderer lowering can ignore dead/diagnostic alternatives without linearizing them.
        [[nodiscard]] std::vector<RenderGraphPassHandle> presentation_path() const;
        [[nodiscard]] bool presentation_contains_pass(RenderGraphPassKind kind) const;

        // Explicit outputs keep non-presentation branches executable for GPU-side work. They do not
        // export a persistent Engine texture or expose an RHI object; graph transients still retire with
        // the frame. execution_passes() is the union of presentation and explicit-output ancestry.
        void mark_output(RenderGraphTextureHandle texture);
        [[nodiscard]] const std::vector<RenderGraphTextureHandle> &outputs() const noexcept;
        [[nodiscard]] std::vector<RenderGraphPassHandle> execution_passes() const;

        // Modules are ordinary CPU objects whose build() method appends declarative resources/passes.
        // No callback is retained or invoked by render workers; the resulting graph remains plain data.
        template <typename Module>
            requires requires(const Module &module, RenderGraph &graph) { module.build(graph); }
        decltype(auto) compose(const Module &module) {
            return module.build(*this);
        }

        // Safe custom-pass primitive for application-defined modules. The description and constants are
        // copied immediately into graph-owned CPU data; no application callback or RHI object crosses the
        // Engine/Renderer boundary. The current lowering supports scene-linear HDR fullscreen effects.
        [[nodiscard]] RenderGraphTextureHandle add_fullscreen_effect(
            RenderGraphTextureHandle input,
            const FullscreenEffectDescription &effect);
        [[nodiscard]] RenderGraphTextureHandle add_compute_effect(
            RenderGraphTextureHandle input,
            const ComputeEffectDescription &effect);
        [[nodiscard]] RenderGraphTextureHandle add_copy(
            RenderGraphTextureHandle input,
            const CopyDescription &copy);

        [[nodiscard]] bool enabled(RenderFeature feature) const noexcept;
        RenderGraph &set_enabled(RenderFeature feature, bool enabled) noexcept;
        RenderGraph &enable(RenderFeature feature) noexcept;
        RenderGraph &disable(RenderFeature feature) noexcept;
        RenderGraph &set_execution_mode(RenderGraphExecutionMode mode) noexcept;
        RenderGraph &set_resolution_scale(f32 scale) noexcept;
        RenderGraph &set_background_color(glm::vec4 color) noexcept;
        RenderGraph &inherit_camera_background() noexcept;
        RenderGraph &set_tone_mapping(ToneMappingOperator operation,
                                      f32 exposure = 1.0f,
                                      f32 white_point = 1.0f,
                                      f32 saturation = 1.0f) noexcept;

        // Native consumers can configure a typed section with a concise lambda while the ordinary
        // struct accessors above remain straightforward to expose through C.
        template <typename Configure>
            requires std::invocable<Configure, SceneRenderSettings &>
        RenderGraph &configure_scene(Configure &&configure) {
            std::invoke(std::forward<Configure>(configure), description_.scene);
            return *this;
        }

        template <typename Configure>
            requires std::invocable<Configure, ShadowSettings &>
        RenderGraph &configure_shadows(Configure &&configure) {
            std::invoke(std::forward<Configure>(configure), description_.shadows);
            return *this;
        }
        template <typename Configure>
            requires std::invocable<Configure, BloomSettings &>
        RenderGraph &configure_bloom(Configure &&configure) {
            std::invoke(std::forward<Configure>(configure), description_.bloom);
            return *this;
        }

        template <typename Configure>
            requires std::invocable<Configure, ToneMappingSettings &>
        RenderGraph &configure_tone_mapping(Configure &&configure) {
            std::invoke(std::forward<Configure>(configure), description_.tone_mapping);
            return *this;
        }

        [[nodiscard]] RenderGraphResult validate() const noexcept;

        // Produces a safe executable recipe from untrusted/serialized settings. Engine uses this at
        // frame extraction after logging validate()'s diagnostic, so malformed settings never leak to
        // target allocation or shader constants.
        [[nodiscard]] RenderGraph normalized() const noexcept;

      private:
        struct EmptyTag {};
        RenderGraph(EmptyTag, RenderGraphDescription description) noexcept;

        friend struct RenderModules::DeferredScene;
        friend struct RenderModules::AntiAliasing;
        friend struct RenderModules::Bloom;
        friend struct RenderModules::FullscreenEffect;
        friend struct RenderModules::RasterEffect;
        friend struct RenderModules::ComputeEffect;
        friend struct RenderModules::Copy;
        friend struct RenderModules::ToneMapping;
        friend struct RenderModules::DebugOverlay;
        friend struct RenderModules::Present;

        [[nodiscard]] RenderGraphTextureHandle create_texture(RenderGraphTextureDescription description);
        [[nodiscard]] RenderGraphTextureHandle add_builtin_pass(
            RenderGraphPassKind kind,
            RenderGraphTextureHandle input,
            RenderGraphTextureDescription output,
            UString label);
        [[nodiscard]] RenderGraphPassHandle add_present_pass(
            RenderGraphTextureHandle input, RenderTargetHandle target);
        void build_standard_topology();
        void rebase_handles() noexcept;
        [[nodiscard]] RenderGraphResult validate_topology() const noexcept;

        RenderGraphDescription description_{};
        u32 generation_ = 0;
        std::vector<RenderGraphTextureDescription> textures_;
        std::vector<RenderGraphPassDescription> passes_;
        std::vector<RenderGraphTextureHandle> outputs_;
        RenderGraphTextureHandle presented_texture_{};
    };

} // namespace SFT::Engine
