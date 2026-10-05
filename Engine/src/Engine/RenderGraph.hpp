#pragma once

#include <Foundation/Foundation.hpp>

#include <array>
#include <expected>
#include <functional>
#include <optional>
#include <utility>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <Engine/RenderGraphModule.hpp>
#include <RenderSettings/RenderSettings.hpp>

namespace SFT::Engine {


    enum class RenderFeature : u8 {
        Scene,
        Shadows,
        AmbientOcclusion,
        AntiAliasing,
        Bloom,
        ToneMapping,
        /// Collect GPU/CPU frame timings for `Renderer::last_frame_timings` (what an application's own
        /// diagnostics overlay reads). Formerly `DebugOverlay`, before the overlay itself left the engine.
        FrameTimings,
        RestirGi,
        MotionBlur,
    };

    // Every per-frame setting is defined once, in RenderSettings/RenderSettings.hpp (plain data the renderer, the C ABI
    // and the CxxApi share). These names are the Engine-side spelling of the same types.
    using RenderGraphExecutionMode = RenderSettings::ExecutionMode;
    using SceneIntegrator = RenderSettings::SceneIntegrator;
    using AmbientOcclusionQuality = RenderSettings::AmbientOcclusionQuality;
    using PostProcessAntiAliasing = RenderSettings::PostProcessAntiAliasing;
    using ToneMappingOperator = RenderSettings::ToneMappingOperator;
    using AgxLook = RenderSettings::AgxLook;
    using HermiteSplineSettings = RenderSettings::HermiteSplineSettings;
    using PsychoVSettings = RenderSettings::PsychoVSettings;
    using SceneRenderSettings = RenderSettings::SceneSettings;
    using ShadowDebugView = RenderSettings::ShadowDebugView;
    using ShadowSettings = RenderSettings::ShadowSettings;
    using AmbientOcclusionSettings = RenderSettings::AmbientOcclusionSettings;
    using AntiAliasingSettings = RenderSettings::AntiAliasingSettings;
    using BloomSettings = RenderSettings::BloomSettings;
    using ToneMappingSettings = RenderSettings::ToneMappingSettings;
    using RestirGiQuality = RenderSettings::RestirGiQuality;
    using RestirGiDenoiser = RenderSettings::RestirGiDenoiser;
    using RestirGiSettings = RenderSettings::RestirGiSettings;
    using ScreenSpaceGiSettings = RenderSettings::ScreenSpaceGiSettings;
    using LightingSettings = RenderSettings::LightingSettings;
    using VolumetricFogSettings = RenderSettings::VolumetricFogSettings;
    using ReflectionSettings = RenderSettings::ReflectionSettings;
    using MotionBlurSettings = RenderSettings::MotionBlurSettings;
    using FisheyeMode = RenderSettings::FisheyeMode;
    using CameraEmulationSettings = RenderSettings::CameraEmulationSettings;
    using AutoExposureSettings = RenderSettings::AutoExposureSettings;
    using TemporalUpscalerSettings = RenderSettings::TemporalUpscalerSettings;
    /// Everything a frame is rendered with.
    using RenderGraphDescription = RenderSettings::FrameSettings;

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
        /// Frame timing collection (off by default); see `RenderFeature::FrameTimings`.
        [[nodiscard]] const LightingSettings &lighting() const noexcept;
        [[nodiscard]] LightingSettings &lighting() noexcept;
        [[nodiscard]] const VolumetricFogSettings &volumetric_fog() const noexcept;
        [[nodiscard]] VolumetricFogSettings &volumetric_fog() noexcept;
        [[nodiscard]] const ReflectionSettings &reflections() const noexcept;
        [[nodiscard]] ReflectionSettings &reflections() noexcept;
        /// Returns the current or globally available ReSTIR GI value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const RestirGiSettings &restir_gi() const noexcept;
        /// Returns the current or globally available ReSTIR GI value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RestirGiSettings &restir_gi() noexcept;
        /// Returns the current or globally available motion blur value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const MotionBlurSettings &motion_blur() const noexcept;
        /// Returns the current or globally available motion blur value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] MotionBlurSettings &motion_blur() noexcept;
        /// Returns the camera-emulation (body-cam look) settings.
        [[nodiscard]] const CameraEmulationSettings &camera_emulation() const noexcept;
        [[nodiscard]] CameraEmulationSettings &camera_emulation() noexcept;
        [[nodiscard]] const AutoExposureSettings &auto_exposure() const noexcept;
        [[nodiscard]] AutoExposureSettings &auto_exposure() noexcept;
        [[nodiscard]] const ScreenSpaceGiSettings &screen_space_gi() const noexcept;
        [[nodiscard]] ScreenSpaceGiSettings &screen_space_gi() noexcept;
        [[nodiscard]] const TemporalUpscalerSettings &temporal_upscaler() const noexcept;
        [[nodiscard]] TemporalUpscalerSettings &temporal_upscaler() noexcept;
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

        /// Configures ReSTIR GI using the supplied arguments and current state.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename Configure>
            requires std::invocable<Configure, RestirGiSettings &>
        RenderGraph &configure_restir_gi(Configure &&configure) {
            std::invoke(std::forward<Configure>(configure), description_.restir_gi);
            return *this;
        }

        /// Configures motion blur using the supplied arguments and current state.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename Configure>
            requires std::invocable<Configure, MotionBlurSettings &>
        RenderGraph &configure_motion_blur(Configure &&configure) {
            std::invoke(std::forward<Configure>(configure), description_.motion_blur);
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
