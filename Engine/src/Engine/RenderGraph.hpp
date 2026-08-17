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


    enum class PostProcessAntiAliasing : u8 {
        None,
        Fxaa,
        ConservativeMorphological,
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

    struct AgxSettings {
        AgxLook look = AgxLook::None;
    };


    struct HermiteSplineSettings {
        f32 toe_strength = 0.5f;
        f32 toe_length = 0.5f;
        f32 shoulder_strength = 2.0f;
        f32 shoulder_length = 0.5f;
        f32 shoulder_angle = 1.0f;
    };


    struct PsychoVSettings {
        f32 highlights = 1.0f;
        f32 shadows = 1.0f;
        f32 contrast = 1.0f;
        f32 purity_scale = 1.0f;
        f32 gamut_compression = 1.0f;
        bool gamut_compression_use_bt2020 = true;


        f32 compression = 0.0f;
        glm::vec3 adapted_gray_bt709{0.18f};
        glm::vec3 background_gray_bt709{0.18f};
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


        std::optional<glm::vec4> background_color;

        f32 background_intensity = 1.0f;
    };


    struct ShadowSettings {
        bool enabled = true;
        u32 atlas_size = 4096;
        u32 cascade_count = 4;
        f32 max_distance = 250.0f;
        f32 cascade_split_lambda = 0.65f;
        f32 cascade_blend = 0.10f;
        f32 depth_bias = 0.75f;
        f32 slope_bias = 1.0f;


        f32 normal_bias = 0.75f;
        u32 max_shadowed_spot_lights = 8;
        u32 max_shadowed_point_lights = 4;
        bool contact_hardening = true;


        bool contact_shadows = true;
        f32 contact_shadow_distance = 0.5f;
        f32 contact_shadow_thickness = 0.05f;
        u32 contact_shadow_steps = 8;
    };

    struct AmbientOcclusionSettings {
        bool enabled = true;
        f32 radius = 1.0f;
        f32 falloff = 0.8f;
        f32 thickness = 0.15f;
        f32 intensity = 1.0f;
        AmbientOcclusionQuality quality = AmbientOcclusionQuality::High;
    };

    struct AntiAliasingSettings {


        u32 msaa_samples = 1;
        PostProcessAntiAliasing post_process = PostProcessAntiAliasing::Fxaa;

        f32 subpixel_quality = 0.75f;

        f32 edge_threshold = 0.125f;
    };


    struct BloomSettings {
        bool enabled = true;


        f32 threshold = 0.0f;
        f32 soft_knee = 0.5f;
        f32 intensity = 0.04f;
        f32 scatter = 0.7f;

        f32 downsample_ratio = 1.61803398875f;


        u32 max_levels = 12;
    };

    struct ToneMappingSettings {
        bool enabled = true;


        ToneMappingOperator operation = ToneMappingOperator::Agx;
        f32 exposure = 1.0f;
        f32 white_point = 1.0f;
        f32 saturation = 1.0f;


        f32 hdr_paper_white_nits = 203.0f;
        f32 hdr_peak_nits = 1000.0f;

        AgxSettings agx{};
        HermiteSplineSettings hermite_spline{};
        PsychoVSettings psycho_v{};
    };

    struct DebugOverlayRenderSettings {
        bool enabled = false;


        bool draw_text = true;
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


    class RenderGraph {
      public:

        /// Constructs a `RenderGraph` in its default state.
        ///
        /// @note This function does not throw exceptions.
        RenderGraph() noexcept;
        /// Constructs a `RenderGraph` from the supplied initialization values.
        ///
        /// @param description Description of the resource or operation to perform.
        ///
        /// @note This function does not throw exceptions.
        explicit RenderGraph(RenderGraphDescription description) noexcept;
        /// Constructs a `RenderGraph` from another instance.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        RenderGraph(const RenderGraph &other);
        /// Assigns a new value to this `RenderGraph`.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        RenderGraph &operator=(const RenderGraph &other);
        /// Constructs a `RenderGraph` from another instance.
        ///
        /// @note This function does not throw exceptions.
        RenderGraph(RenderGraph &&) noexcept = default;
        /// Assigns a new value to this `RenderGraph`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderGraph &operator=(RenderGraph &&) noexcept = default;

        /// Returns the current or globally available standard value.
        ///
        /// @return Returns the current standard value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RenderGraph standard() noexcept;
        /// Returns the current or globally available overlay only value.
        ///
        /// @return Returns the current overlay only value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RenderGraph overlay_only() noexcept;
        /// Reports whether this `RenderGraph` contains no elements or payload.
        ///
        /// @param description Description of the resource or operation to perform.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RenderGraph empty(RenderGraphDescription description = {}) noexcept;

        /// Returns the current or globally available description value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const RenderGraphDescription &description() const noexcept;
        /// Returns the current or globally available description value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RenderGraphDescription &description() noexcept;

        /// Returns the current or globally available scene value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const SceneRenderSettings &scene() const noexcept;
        /// Returns the current or globally available scene value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SceneRenderSettings &scene() noexcept;
        /// Returns the current or globally available shadows value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const ShadowSettings &shadows() const noexcept;
        /// Returns the current or globally available shadows value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ShadowSettings &shadows() noexcept;
        /// Returns the current or globally available ambient occlusion value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const AmbientOcclusionSettings &ambient_occlusion() const noexcept;
        /// Returns the current or globally available ambient occlusion value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] AmbientOcclusionSettings &ambient_occlusion() noexcept;
        /// Returns the current or globally available anti aliasing value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const AntiAliasingSettings &anti_aliasing() const noexcept;
        /// Returns the current or globally available anti aliasing value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] AntiAliasingSettings &anti_aliasing() noexcept;
        /// Returns the current or globally available bloom value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const BloomSettings &bloom() const noexcept;
        /// Returns the current or globally available bloom value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] BloomSettings &bloom() noexcept;
        /// Returns the current or globally available tone mapping value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const ToneMappingSettings &tone_mapping() const noexcept;
        /// Returns the current or globally available tone mapping value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ToneMappingSettings &tone_mapping() noexcept;
        /// Returns the current or globally available debug overlay value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const DebugOverlayRenderSettings &debug_overlay() const noexcept;
        /// Returns the current or globally available debug overlay value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] DebugOverlayRenderSettings &debug_overlay() noexcept;
        /// Returns the current or globally available execution mode value.
        ///
        /// @return Returns the current execution mode value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RenderGraphExecutionMode execution_mode() const noexcept;

        /// Returns the current or globally available textures value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const std::vector<RenderGraphTextureDescription> &textures() const noexcept;
        /// Returns the current or globally available passes value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const std::vector<RenderGraphPassDescription> &passes() const noexcept;
        /// Presents the completed frame to the target surface or swapchain.
        ///
        /// @return Returns the current presented texture value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RenderGraphTextureHandle presented_texture() const noexcept;

        /// Returns the current or globally available selected render target value.
        ///
        /// @return Returns the current selected render target value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RenderTargetHandle selected_render_target() const noexcept;
        /// Reports whether pass holds for this `RenderGraph`.
        ///
        /// @param kind `kind` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool contains_pass(RenderGraphPassKind kind) const noexcept;


        /// Presents the completed frame to the target surface or swapchain.
        ///
        /// @return Returns the current presentation path value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] std::vector<RenderGraphPassHandle> presentation_path() const;
        /// Presents the completed frame to the target surface or swapchain.
        ///
        /// @param kind `kind` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool presentation_contains_pass(RenderGraphPassKind kind) const;


        /// Marks output using the supplied arguments and current state.
        ///
        /// @param texture Texture used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void mark_output(RenderGraphTextureHandle texture);
        /// Returns the current or globally available outputs value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const std::vector<RenderGraphTextureHandle> &outputs() const noexcept;
        /// Returns the current or globally available execution passes value.
        ///
        /// @return Returns the current execution passes value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] std::vector<RenderGraphPassHandle> execution_passes() const;


        /// Returns the current or globally available compose value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename Module>
            requires requires(const Module &module, RenderGraph &graph) { module.build(graph); }
        decltype(auto) compose(const Module &module) {
            return module.build(*this);
        }


        /// Adds fullscreen effect using the supplied arguments and current state.
        ///
        /// @param input `input` value used by the operation.
        /// @param effect `effect` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] RenderGraphTextureHandle add_fullscreen_effect(
            RenderGraphTextureHandle input,
            const FullscreenEffectDescription &effect);
        /// Adds compute effect using the supplied arguments and current state.
        ///
        /// @param input `input` value used by the operation.
        /// @param effect `effect` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] RenderGraphTextureHandle add_compute_effect(
            RenderGraphTextureHandle input,
            const ComputeEffectDescription &effect);
        /// Adds copy using the supplied arguments and current state.
        ///
        /// @param input `input` value used by the operation.
        /// @param copy `copy` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] RenderGraphTextureHandle add_copy(
            RenderGraphTextureHandle input,
            const CopyDescription &copy);

        /// Performs the enabled operation for `RenderGraph` using the supplied arguments.
        ///
        /// @param feature `feature` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool enabled(RenderFeature feature) const noexcept;
        /// Sets the enabled for this `RenderGraph`.
        ///
        /// @param feature `feature` value used by the operation.
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderGraph &set_enabled(RenderFeature feature, bool enabled) noexcept;
        /// Enables the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param feature `feature` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        RenderGraph &enable(RenderFeature feature) noexcept;
        /// Disables the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param feature `feature` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        RenderGraph &disable(RenderFeature feature) noexcept;
        /// Sets the execution mode for this `RenderGraph`.
        ///
        /// @param mode Mode controlling how the operation is performed.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderGraph &set_execution_mode(RenderGraphExecutionMode mode) noexcept;
        /// Sets the resolution scale for this `RenderGraph`.
        ///
        /// @param scale `scale` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderGraph &set_resolution_scale(f32 scale) noexcept;
        /// Sets the background color for this `RenderGraph`.
        ///
        /// @param color `color` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderGraph &set_background_color(glm::vec4 color) noexcept;
        /// Returns the current or globally available inherit camera background value.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderGraph &inherit_camera_background() noexcept;
        /// Sets the tone mapping for this `RenderGraph`.
        ///
        /// @param operation `operation` value used by the operation.
        /// @param exposure `exposure` value used by the operation.
        /// @param white_point `white_point` value used by the operation.
        /// @param saturation `saturation` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderGraph &set_tone_mapping(ToneMappingOperator operation,
                                      f32 exposure = 1.0f,
                                      f32 white_point = 1.0f,
                                      f32 saturation = 1.0f) noexcept;


        /// Configures scene using the supplied arguments and current state.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename Configure>
            requires std::invocable<Configure, SceneRenderSettings &>
        RenderGraph &configure_scene(Configure &&configure) {
            std::invoke(std::forward<Configure>(configure), description_.scene);
            return *this;
        }

        /// Configures shadows using the supplied arguments and current state.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename Configure>
            requires std::invocable<Configure, ShadowSettings &>
        RenderGraph &configure_shadows(Configure &&configure) {
            std::invoke(std::forward<Configure>(configure), description_.shadows);
            return *this;
        }
        /// Configures bloom using the supplied arguments and current state.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename Configure>
            requires std::invocable<Configure, BloomSettings &>
        RenderGraph &configure_bloom(Configure &&configure) {
            std::invoke(std::forward<Configure>(configure), description_.bloom);
            return *this;
        }

        /// Configures tone mapping using the supplied arguments and current state.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename Configure>
            requires std::invocable<Configure, ToneMappingSettings &>
        RenderGraph &configure_tone_mapping(Configure &&configure) {
            std::invoke(std::forward<Configure>(configure), description_.tone_mapping);
            return *this;
        }

        /// Validates the supplied value or current state.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `RenderGraphErrorCode::InvalidResolutionScale`, `RenderGraphErrorCode::InvalidBackgroundColor`, `RenderGraphErrorCode::InvalidSpectralPathTracingSettings`, `RenderGraphErrorCode::InvalidShadowSettings`, `RenderGraphErrorCode::InvalidAmbientOcclusionSettings`, `RenderGraphErrorCode::InvalidAntiAliasingSettings` among others.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RenderGraphResult validate() const noexcept;


        /// Returns the current or globally available normalized value.
        ///
        /// @return Returns the current normalized value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RenderGraph normalized() const noexcept;

      private:
        struct EmptyTag {};
        /// Constructs a `RenderGraph` from the supplied initialization values.
        ///
        /// @param description Description of the resource or operation to perform.
        ///
        /// @note This function does not throw exceptions.
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

        /// Creates a texture from the supplied parameters.
        ///
        /// @param description Description of the resource or operation to perform.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] RenderGraphTextureHandle create_texture(RenderGraphTextureDescription description);
        /// Adds builtin pass using the supplied arguments and current state.
        ///
        /// @param kind `kind` value used by the operation.
        /// @param input `input` value used by the operation.
        /// @param output `output` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] RenderGraphTextureHandle add_builtin_pass(
            RenderGraphPassKind kind,
            RenderGraphTextureHandle input,
            RenderGraphTextureDescription output,
            UString label);
        /// Adds present pass using the supplied arguments and current state.
        ///
        /// @param input `input` value used by the operation.
        /// @param target `target` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] RenderGraphPassHandle add_present_pass(
            RenderGraphTextureHandle input, RenderTargetHandle target);
        /// Builds standard topology.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void build_standard_topology();
        /// Performs the rebase handles operation for `RenderGraph` using the supplied arguments.
        ///
        /// @note This function does not throw exceptions.
        void rebase_handles() noexcept;
        /// Validates topology.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RenderGraphResult validate_topology() const noexcept;

        RenderGraphDescription description_{};
        u32 generation_ = 0;
        std::vector<RenderGraphTextureDescription> textures_;
        std::vector<RenderGraphPassDescription> passes_;
        std::vector<RenderGraphTextureHandle> outputs_;
        RenderGraphTextureHandle presented_texture_{};
    };

} // namespace SFT::Engine
