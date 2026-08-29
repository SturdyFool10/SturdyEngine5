/// C ABI implementation of the fine-grained render-graph settings setters.
///
/// Each entry point resolves the caller's scope-bound `SturdyFrame`, translates and lightly
/// validates the caller-supplied settings struct, and replaces the matching
/// `Engine::RenderGraph` settings group wholesale. Deeper cross-field validation
/// (`RenderGraphDescription::validate`) runs later in the engine's own frame pipeline, the same as
/// it does for a C++ caller — this layer only rejects what a C caller could get wrong that a
/// same-language caller could not (NaN floats, an enum value that does not exist).

#include <Foundation/Foundation.hpp>

#include <cmath>
#include <initializer_list>

#include <glm/vec3.hpp>

#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::HandleKind;
    using SFT::Ffi::guarded;
    using SFT::Ffi::resolve_handle;
    using SFT::Ffi::set_error;
    using SFT::f32;

    [[nodiscard]] SturdyResult resolve_frame(SturdyFrame frame,
                                             SFT::Engine::RenderFrameParameters **out_parameters) noexcept {
        void *pointer = nullptr;
        const SturdyResult result = resolve_handle(frame.token, HandleKind::Frame, &pointer);
        if (result != STURDY_OK) {
            return result;
        }
        *out_parameters = static_cast<SFT::Engine::RenderFrameParameters *>(pointer);
        return STURDY_OK;
    }

    [[nodiscard]] bool all_finite(std::initializer_list<f32> values) noexcept {
        for (const f32 value : values) {
            if (!std::isfinite(value)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool translate_integrator(SturdySceneIntegrator integrator,
                                            SFT::Engine::SceneIntegrator *out) noexcept {
        switch (integrator) {
        case STURDY_SCENE_INTEGRATOR_RASTER_DEFERRED:
            *out = SFT::Engine::SceneIntegrator::RasterDeferred;
            return true;
        case STURDY_SCENE_INTEGRATOR_SHADOW_ONLY:
            *out = SFT::Engine::SceneIntegrator::ShadowOnly;
            return true;
        case STURDY_SCENE_INTEGRATOR_REFLECTION_ONLY:
            *out = SFT::Engine::SceneIntegrator::ReflectionOnly;
            return true;
        case STURDY_SCENE_INTEGRATOR_AMBIENT_OCCLUSION_ONLY:
            *out = SFT::Engine::SceneIntegrator::AmbientOcclusionOnly;
            return true;
        case STURDY_SCENE_INTEGRATOR_SHADOW_AND_TRANSMISSION:
            *out = SFT::Engine::SceneIntegrator::ShadowAndTransmission;
            return true;
        case STURDY_SCENE_INTEGRATOR_FULL_PATH_TRACING:
            *out = SFT::Engine::SceneIntegrator::FullPathTracing;
            return true;
        case STURDY_SCENE_INTEGRATOR_FORCE_U32:
        default:
            return false;
        }
    }

    [[nodiscard]] bool translate_shadow_debug_view(SturdyShadowDebugView view,
                                                    SFT::Engine::ShadowDebugView *out) noexcept {
        const auto raw = static_cast<uint32_t>(view);
        if (raw > static_cast<uint32_t>(SFT::Engine::ShadowDebugView::ScreenSpaceAmbientOcclusion)) {
            return false;
        }
        *out = static_cast<SFT::Engine::ShadowDebugView>(raw);
        return true;
    }

    [[nodiscard]] bool translate_ao_quality(SturdyAmbientOcclusionQuality quality,
                                            SFT::Engine::AmbientOcclusionQuality *out) noexcept {
        switch (quality) {
        case STURDY_AMBIENT_OCCLUSION_QUALITY_LOW:
            *out = SFT::Engine::AmbientOcclusionQuality::Low;
            return true;
        case STURDY_AMBIENT_OCCLUSION_QUALITY_MEDIUM:
            *out = SFT::Engine::AmbientOcclusionQuality::Medium;
            return true;
        case STURDY_AMBIENT_OCCLUSION_QUALITY_HIGH:
            *out = SFT::Engine::AmbientOcclusionQuality::High;
            return true;
        case STURDY_AMBIENT_OCCLUSION_QUALITY_ULTRA:
            *out = SFT::Engine::AmbientOcclusionQuality::Ultra;
            return true;
        case STURDY_AMBIENT_OCCLUSION_QUALITY_FORCE_U32:
        default:
            return false;
        }
    }

    [[nodiscard]] bool translate_post_process_aa(SturdyPostProcessAntiAliasing value,
                                                 SFT::Engine::PostProcessAntiAliasing *out) noexcept {
        switch (value) {
        case STURDY_POST_PROCESS_ANTI_ALIASING_NONE:
            *out = SFT::Engine::PostProcessAntiAliasing::None;
            return true;
        case STURDY_POST_PROCESS_ANTI_ALIASING_FXAA:
            *out = SFT::Engine::PostProcessAntiAliasing::Fxaa;
            return true;
        case STURDY_POST_PROCESS_ANTI_ALIASING_CONSERVATIVE_MORPHOLOGICAL:
            *out = SFT::Engine::PostProcessAntiAliasing::ConservativeMorphological;
            return true;
        case STURDY_POST_PROCESS_ANTI_ALIASING_FORCE_U32:
        default:
            return false;
        }
    }

    [[nodiscard]] bool translate_tone_mapping_op(SturdyToneMapping operation,
                                                 SFT::Engine::ToneMappingOperator *out) noexcept {
        switch (operation) {
        case STURDY_TONE_MAPPING_NONE:
            *out = SFT::Engine::ToneMappingOperator::None;
            return true;
        case STURDY_TONE_MAPPING_REINHARD:
            *out = SFT::Engine::ToneMappingOperator::Reinhard;
            return true;
        case STURDY_TONE_MAPPING_EXPONENTIAL:
            *out = SFT::Engine::ToneMappingOperator::Exponential;
            return true;
        case STURDY_TONE_MAPPING_AGX:
            *out = SFT::Engine::ToneMappingOperator::Agx;
            return true;
        case STURDY_TONE_MAPPING_HERMITE_SPLINE:
            *out = SFT::Engine::ToneMappingOperator::HermiteSpline;
            return true;
        case STURDY_TONE_MAPPING_PSYCHO_V:
            *out = SFT::Engine::ToneMappingOperator::PsychoV;
            return true;
        case STURDY_TONE_MAPPING_FORCE_U32:
        default:
            return false;
        }
    }

    [[nodiscard]] bool translate_agx_look(SturdyAgxLook look, SFT::Engine::AgxLook *out) noexcept {
        switch (look) {
        case STURDY_AGX_LOOK_NONE:
            *out = SFT::Engine::AgxLook::None;
            return true;
        case STURDY_AGX_LOOK_PUNCHY:
            *out = SFT::Engine::AgxLook::Punchy;
            return true;
        case STURDY_AGX_LOOK_GOLDEN:
            *out = SFT::Engine::AgxLook::Golden;
            return true;
        case STURDY_AGX_LOOK_FORCE_U32:
        default:
            return false;
        }
    }

    [[nodiscard]] bool translate_restir_quality(SturdyRestirGiQuality quality,
                                                SFT::Engine::RestirGiQuality *out) noexcept {
        switch (quality) {
        case STURDY_RESTIR_GI_QUALITY_LOW:
            *out = SFT::Engine::RestirGiQuality::Low;
            return true;
        case STURDY_RESTIR_GI_QUALITY_MEDIUM:
            *out = SFT::Engine::RestirGiQuality::Medium;
            return true;
        case STURDY_RESTIR_GI_QUALITY_HIGH:
            *out = SFT::Engine::RestirGiQuality::High;
            return true;
        case STURDY_RESTIR_GI_QUALITY_FORCE_U32:
        default:
            return false;
        }
    }

    [[nodiscard]] bool translate_restir_denoiser(SturdyRestirGiDenoiser denoiser,
                                                 SFT::Engine::RestirGiDenoiser *out) noexcept {
        switch (denoiser) {
        case STURDY_RESTIR_GI_DENOISER_NONE:
            *out = SFT::Engine::RestirGiDenoiser::None;
            return true;
        case STURDY_RESTIR_GI_DENOISER_SVGF:
            *out = SFT::Engine::RestirGiDenoiser::Svgf;
            return true;
        case STURDY_RESTIR_GI_DENOISER_DLSS_RAY_RECONSTRUCTION:
            *out = SFT::Engine::RestirGiDenoiser::DlssRayReconstruction;
            return true;
        case STURDY_RESTIR_GI_DENOISER_FSR_REDSTONE:
            *out = SFT::Engine::RestirGiDenoiser::FsrRedstone;
            return true;
        case STURDY_RESTIR_GI_DENOISER_FORCE_U32:
        default:
            return false;
        }
    }

} // namespace

extern "C" {

// ─── Scene ──────────────────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_scene_settings_init(SturdySceneSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        const SFT::Engine::SceneRenderSettings defaults{};
        *settings = SturdySceneSettings{};
        settings->struct_size = static_cast<uint32_t>(sizeof(SturdySceneSettings));
        settings->enabled = defaults.enabled ? STURDY_TRUE : STURDY_FALSE;
        settings->integrator = STURDY_SCENE_INTEGRATOR_RASTER_DEFERRED;
        settings->path_samples_per_pixel = defaults.path_samples_per_pixel;
        settings->path_max_bounces = defaults.path_max_bounces;
        settings->path_russian_roulette_start_bounce = defaults.path_russian_roulette_start_bounce;
        settings->caustic_photon_count = defaults.caustic_photon_count;
        settings->caustic_gather_radius = defaults.caustic_gather_radius;
        settings->wavelength_min_nm = defaults.wavelength_min_nm;
        settings->wavelength_max_nm = defaults.wavelength_max_nm;
        settings->background_intensity = defaults.background_intensity;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_frame_set_scene_settings(SturdyFrame frame, const SturdySceneSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "settings must not be null");
        }
        SFT::Engine::SceneIntegrator integrator{};
        if (!translate_integrator(settings->integrator, &integrator)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized scene integrator");
        }
        if (!all_finite({settings->caustic_gather_radius, settings->wavelength_min_nm,
                         settings->wavelength_max_nm, settings->background_intensity})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "scene settings must be finite");
        }
        SFT::Engine::RenderFrameParameters *parameters = nullptr;
        const SturdyResult resolved = resolve_frame(frame, &parameters);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        SFT::Engine::SceneRenderSettings &scene = parameters->render_graph.scene();
        scene.enabled = settings->enabled != STURDY_FALSE;
        scene.integrator = integrator;
        scene.path_samples_per_pixel = settings->path_samples_per_pixel;
        scene.path_max_bounces = settings->path_max_bounces;
        scene.path_russian_roulette_start_bounce = settings->path_russian_roulette_start_bounce;
        scene.caustic_photon_count = settings->caustic_photon_count;
        scene.caustic_gather_radius = settings->caustic_gather_radius;
        scene.wavelength_min_nm = settings->wavelength_min_nm;
        scene.wavelength_max_nm = settings->wavelength_max_nm;
        scene.background_intensity = settings->background_intensity;
        return STURDY_OK;
    });
}

// ─── Shadows ────────────────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_shadow_settings_init(SturdyShadowSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        const SFT::Engine::ShadowSettings defaults{};
        *settings = SturdyShadowSettings{};
        settings->struct_size = static_cast<uint32_t>(sizeof(SturdyShadowSettings));
        settings->enabled = defaults.enabled ? STURDY_TRUE : STURDY_FALSE;
        settings->atlas_size = defaults.atlas_size;
        settings->cascade_count = defaults.cascade_count;
        settings->max_distance = defaults.max_distance;
        settings->cascade_split_lambda = defaults.cascade_split_lambda;
        settings->cascade_blend = defaults.cascade_blend;
        settings->depth_bias = defaults.depth_bias;
        settings->slope_bias = defaults.slope_bias;
        for (SFT::usize i = 0; i < defaults.cascade_resolutions.size(); ++i) {
            settings->cascade_resolutions[i] = defaults.cascade_resolutions[i];
        }
        settings->filter_radius_texels = defaults.filter_radius_texels;
        settings->normal_bias = defaults.normal_bias;
        settings->debug_view = STURDY_SHADOW_DEBUG_VIEW_NONE;
        settings->max_shadowed_spot_lights = defaults.max_shadowed_spot_lights;
        settings->max_shadowed_point_lights = defaults.max_shadowed_point_lights;
        settings->contact_hardening = defaults.contact_hardening ? STURDY_TRUE : STURDY_FALSE;
        settings->contact_shadows = defaults.contact_shadows ? STURDY_TRUE : STURDY_FALSE;
        settings->contact_shadow_distance = defaults.contact_shadow_distance;
        settings->contact_shadow_thickness = defaults.contact_shadow_thickness;
        settings->contact_shadow_steps = defaults.contact_shadow_steps;
        settings->contact_shadow_intensity = defaults.contact_shadow_intensity;
        settings->contact_shadow_fade_distance = defaults.contact_shadow_fade_distance;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_frame_set_shadow_settings(SturdyFrame frame,
                                                               const SturdyShadowSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "settings must not be null");
        }
        SFT::Engine::ShadowDebugView debug_view{};
        if (!translate_shadow_debug_view(settings->debug_view, &debug_view)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized shadow debug view");
        }
        if (!all_finite({settings->max_distance, settings->cascade_split_lambda, settings->cascade_blend,
                         settings->depth_bias, settings->slope_bias, settings->filter_radius_texels,
                         settings->normal_bias, settings->contact_shadow_distance,
                         settings->contact_shadow_thickness, settings->contact_shadow_intensity,
                         settings->contact_shadow_fade_distance})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "shadow settings must be finite");
        }
        if (settings->cascade_count == 0 || settings->cascade_count > 4) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "cascade_count must be within [1, 4]");
        }
        SFT::Engine::RenderFrameParameters *parameters = nullptr;
        const SturdyResult resolved = resolve_frame(frame, &parameters);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        SFT::Engine::ShadowSettings &shadows = parameters->render_graph.shadows();
        shadows.enabled = settings->enabled != STURDY_FALSE;
        shadows.atlas_size = settings->atlas_size;
        shadows.cascade_count = settings->cascade_count;
        shadows.max_distance = settings->max_distance;
        shadows.cascade_split_lambda = settings->cascade_split_lambda;
        shadows.cascade_blend = settings->cascade_blend;
        shadows.depth_bias = settings->depth_bias;
        shadows.slope_bias = settings->slope_bias;
        for (SFT::usize i = 0; i < shadows.cascade_resolutions.size(); ++i) {
            shadows.cascade_resolutions[i] = settings->cascade_resolutions[i];
        }
        shadows.filter_radius_texels = settings->filter_radius_texels;
        shadows.normal_bias = settings->normal_bias;
        shadows.debug_view = debug_view;
        shadows.max_shadowed_spot_lights = settings->max_shadowed_spot_lights;
        shadows.max_shadowed_point_lights = settings->max_shadowed_point_lights;
        shadows.contact_hardening = settings->contact_hardening != STURDY_FALSE;
        shadows.contact_shadows = settings->contact_shadows != STURDY_FALSE;
        shadows.contact_shadow_distance = settings->contact_shadow_distance;
        shadows.contact_shadow_thickness = settings->contact_shadow_thickness;
        shadows.contact_shadow_steps = settings->contact_shadow_steps;
        shadows.contact_shadow_intensity = settings->contact_shadow_intensity;
        shadows.contact_shadow_fade_distance = settings->contact_shadow_fade_distance;
        return STURDY_OK;
    });
}

// ─── Ambient occlusion ──────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_ambient_occlusion_settings_init(SturdyAmbientOcclusionSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        const SFT::Engine::AmbientOcclusionSettings defaults{};
        *settings = SturdyAmbientOcclusionSettings{};
        settings->struct_size = static_cast<uint32_t>(sizeof(SturdyAmbientOcclusionSettings));
        settings->enabled = defaults.enabled ? STURDY_TRUE : STURDY_FALSE;
        settings->radius = defaults.radius;
        settings->quality = STURDY_AMBIENT_OCCLUSION_QUALITY_HIGH;
        settings->intensity = defaults.intensity;
        settings->falloff_range = defaults.falloff_range;
        settings->thin_occluder_compensation = defaults.thin_occluder_compensation;
        settings->final_value_power = defaults.final_value_power;
        settings->sample_distribution_power = defaults.sample_distribution_power;
        settings->denoise = defaults.denoise ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_frame_set_ambient_occlusion_settings(
    SturdyFrame frame, const SturdyAmbientOcclusionSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "settings must not be null");
        }
        SFT::Engine::AmbientOcclusionQuality quality{};
        if (!translate_ao_quality(settings->quality, &quality)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized ambient occlusion quality");
        }
        if (!all_finite({settings->radius, settings->intensity, settings->falloff_range,
                         settings->thin_occluder_compensation, settings->final_value_power,
                         settings->sample_distribution_power})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "ambient occlusion settings must be finite");
        }
        SFT::Engine::RenderFrameParameters *parameters = nullptr;
        const SturdyResult resolved = resolve_frame(frame, &parameters);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        SFT::Engine::AmbientOcclusionSettings &ao = parameters->render_graph.ambient_occlusion();
        ao.enabled = settings->enabled != STURDY_FALSE;
        ao.radius = settings->radius;
        ao.quality = quality;
        ao.intensity = settings->intensity;
        ao.falloff_range = settings->falloff_range;
        ao.thin_occluder_compensation = settings->thin_occluder_compensation;
        ao.final_value_power = settings->final_value_power;
        ao.sample_distribution_power = settings->sample_distribution_power;
        ao.denoise = settings->denoise != STURDY_FALSE;
        return STURDY_OK;
    });
}

// ─── Anti-aliasing ──────────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_anti_aliasing_settings_init(SturdyAntiAliasingSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        const SFT::Engine::AntiAliasingSettings defaults{};
        *settings = SturdyAntiAliasingSettings{};
        settings->struct_size = static_cast<uint32_t>(sizeof(SturdyAntiAliasingSettings));
        settings->msaa_samples = defaults.msaa_samples;
        settings->post_process = STURDY_POST_PROCESS_ANTI_ALIASING_FXAA;
        settings->subpixel_quality = defaults.subpixel_quality;
        settings->edge_threshold = defaults.edge_threshold;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_frame_set_anti_aliasing_settings(SturdyFrame frame,
                                                                      const SturdyAntiAliasingSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "settings must not be null");
        }
        SFT::Engine::PostProcessAntiAliasing post_process{};
        if (!translate_post_process_aa(settings->post_process, &post_process)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized post-process anti-aliasing mode");
        }
        if (!all_finite({settings->subpixel_quality, settings->edge_threshold})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "anti-aliasing settings must be finite");
        }
        if (settings->msaa_samples == 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "msaa_samples must be nonzero");
        }
        SFT::Engine::RenderFrameParameters *parameters = nullptr;
        const SturdyResult resolved = resolve_frame(frame, &parameters);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        SFT::Engine::AntiAliasingSettings &aa = parameters->render_graph.anti_aliasing();
        aa.msaa_samples = settings->msaa_samples;
        aa.post_process = post_process;
        aa.subpixel_quality = settings->subpixel_quality;
        aa.edge_threshold = settings->edge_threshold;
        return STURDY_OK;
    });
}

// ─── Bloom ──────────────────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_bloom_settings_init(SturdyBloomSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        const SFT::Engine::BloomSettings defaults{};
        *settings = SturdyBloomSettings{};
        settings->struct_size = static_cast<uint32_t>(sizeof(SturdyBloomSettings));
        settings->enabled = defaults.enabled ? STURDY_TRUE : STURDY_FALSE;
        settings->threshold = defaults.threshold;
        settings->soft_knee = defaults.soft_knee;
        settings->intensity = defaults.intensity;
        settings->scatter = defaults.scatter;
        settings->downsample_ratio = defaults.downsample_ratio;
        settings->max_levels = defaults.max_levels;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_frame_set_bloom_settings(SturdyFrame frame, const SturdyBloomSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "settings must not be null");
        }
        if (!all_finite({settings->threshold, settings->soft_knee, settings->intensity, settings->scatter,
                         settings->downsample_ratio})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "bloom settings must be finite");
        }
        SFT::Engine::RenderFrameParameters *parameters = nullptr;
        const SturdyResult resolved = resolve_frame(frame, &parameters);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        SFT::Engine::BloomSettings &bloom = parameters->render_graph.bloom();
        bloom.enabled = settings->enabled != STURDY_FALSE;
        bloom.threshold = settings->threshold;
        bloom.soft_knee = settings->soft_knee;
        bloom.intensity = settings->intensity;
        bloom.scatter = settings->scatter;
        bloom.downsample_ratio = settings->downsample_ratio;
        bloom.max_levels = settings->max_levels;
        return STURDY_OK;
    });
}

// ─── Tone mapping ───────────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_tone_mapping_settings_init(SturdyToneMappingSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        const SFT::Engine::ToneMappingSettings defaults{};
        *settings = SturdyToneMappingSettings{};
        settings->struct_size = static_cast<uint32_t>(sizeof(SturdyToneMappingSettings));
        settings->enabled = defaults.enabled ? STURDY_TRUE : STURDY_FALSE;
        settings->operation = STURDY_TONE_MAPPING_AGX;
        settings->exposure = defaults.exposure;
        settings->white_point = defaults.white_point;
        settings->saturation = defaults.saturation;
        settings->hdr_paper_white_nits = defaults.hdr_paper_white_nits;
        settings->hdr_peak_nits = defaults.hdr_peak_nits;
        settings->agx_look = STURDY_AGX_LOOK_NONE;
        settings->hermite_toe_strength = defaults.hermite_spline.toe_strength;
        settings->hermite_toe_length = defaults.hermite_spline.toe_length;
        settings->hermite_shoulder_strength = defaults.hermite_spline.shoulder_strength;
        settings->hermite_shoulder_length = defaults.hermite_spline.shoulder_length;
        settings->hermite_shoulder_angle = defaults.hermite_spline.shoulder_angle;
        settings->psychov_highlights = defaults.psycho_v.highlights;
        settings->psychov_shadows = defaults.psycho_v.shadows;
        settings->psychov_contrast = defaults.psycho_v.contrast;
        settings->psychov_purity_scale = defaults.psycho_v.purity_scale;
        settings->psychov_gamut_compression = defaults.psycho_v.gamut_compression;
        settings->psychov_gamut_compression_use_bt2020 =
            defaults.psycho_v.gamut_compression_use_bt2020 ? STURDY_TRUE : STURDY_FALSE;
        settings->psychov_compression = defaults.psycho_v.compression;
        settings->psychov_adapted_gray_bt709[0] = defaults.psycho_v.adapted_gray_bt709.x;
        settings->psychov_adapted_gray_bt709[1] = defaults.psycho_v.adapted_gray_bt709.y;
        settings->psychov_adapted_gray_bt709[2] = defaults.psycho_v.adapted_gray_bt709.z;
        settings->psychov_background_gray_bt709[0] = defaults.psycho_v.background_gray_bt709.x;
        settings->psychov_background_gray_bt709[1] = defaults.psycho_v.background_gray_bt709.y;
        settings->psychov_background_gray_bt709[2] = defaults.psycho_v.background_gray_bt709.z;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_frame_set_tone_mapping_settings(SturdyFrame frame,
                                                                     const SturdyToneMappingSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "settings must not be null");
        }
        SFT::Engine::ToneMappingOperator operation{};
        if (!translate_tone_mapping_op(settings->operation, &operation)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized tone mapping operator");
        }
        SFT::Engine::AgxLook agx_look{};
        if (!translate_agx_look(settings->agx_look, &agx_look)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized AGX look");
        }
        if (!all_finite({settings->exposure, settings->white_point, settings->saturation,
                         settings->hdr_paper_white_nits, settings->hdr_peak_nits,
                         settings->hermite_toe_strength, settings->hermite_toe_length,
                         settings->hermite_shoulder_strength, settings->hermite_shoulder_length,
                         settings->hermite_shoulder_angle, settings->psychov_highlights,
                         settings->psychov_shadows, settings->psychov_contrast, settings->psychov_purity_scale,
                         settings->psychov_gamut_compression, settings->psychov_compression,
                         settings->psychov_adapted_gray_bt709[0], settings->psychov_adapted_gray_bt709[1],
                         settings->psychov_adapted_gray_bt709[2], settings->psychov_background_gray_bt709[0],
                         settings->psychov_background_gray_bt709[1], settings->psychov_background_gray_bt709[2]})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "tone mapping settings must be finite");
        }
        if (settings->exposure <= 0.0f || settings->white_point <= 0.0f || settings->saturation < 0.0f) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "exposure and white point must be positive, saturation non-negative");
        }
        SFT::Engine::RenderFrameParameters *parameters = nullptr;
        const SturdyResult resolved = resolve_frame(frame, &parameters);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        SFT::Engine::ToneMappingSettings &tone_mapping = parameters->render_graph.tone_mapping();
        tone_mapping.enabled = settings->enabled != STURDY_FALSE;
        tone_mapping.operation = operation;
        tone_mapping.exposure = settings->exposure;
        tone_mapping.white_point = settings->white_point;
        tone_mapping.saturation = settings->saturation;
        tone_mapping.hdr_paper_white_nits = settings->hdr_paper_white_nits;
        tone_mapping.hdr_peak_nits = settings->hdr_peak_nits;
        tone_mapping.agx.look = agx_look;
        tone_mapping.hermite_spline.toe_strength = settings->hermite_toe_strength;
        tone_mapping.hermite_spline.toe_length = settings->hermite_toe_length;
        tone_mapping.hermite_spline.shoulder_strength = settings->hermite_shoulder_strength;
        tone_mapping.hermite_spline.shoulder_length = settings->hermite_shoulder_length;
        tone_mapping.hermite_spline.shoulder_angle = settings->hermite_shoulder_angle;
        tone_mapping.psycho_v.highlights = settings->psychov_highlights;
        tone_mapping.psycho_v.shadows = settings->psychov_shadows;
        tone_mapping.psycho_v.contrast = settings->psychov_contrast;
        tone_mapping.psycho_v.purity_scale = settings->psychov_purity_scale;
        tone_mapping.psycho_v.gamut_compression = settings->psychov_gamut_compression;
        tone_mapping.psycho_v.gamut_compression_use_bt2020 =
            settings->psychov_gamut_compression_use_bt2020 != STURDY_FALSE;
        tone_mapping.psycho_v.compression = settings->psychov_compression;
        tone_mapping.psycho_v.adapted_gray_bt709 = glm::vec3{
            settings->psychov_adapted_gray_bt709[0], settings->psychov_adapted_gray_bt709[1],
            settings->psychov_adapted_gray_bt709[2]};
        tone_mapping.psycho_v.background_gray_bt709 = glm::vec3{
            settings->psychov_background_gray_bt709[0], settings->psychov_background_gray_bt709[1],
            settings->psychov_background_gray_bt709[2]};
        return STURDY_OK;
    });
}

// ─── ReSTIR GI ──────────────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_restir_gi_settings_init(SturdyRestirGiSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        const SFT::Engine::RestirGiSettings defaults{};
        *settings = SturdyRestirGiSettings{};
        settings->struct_size = static_cast<uint32_t>(sizeof(SturdyRestirGiSettings));
        settings->enabled = defaults.enabled ? STURDY_TRUE : STURDY_FALSE;
        settings->quality = STURDY_RESTIR_GI_QUALITY_MEDIUM;
        settings->spatial_reuse_samples = defaults.spatial_reuse_samples;
        settings->spatial_reuse_radius_px = defaults.spatial_reuse_radius_px;
        settings->temporal_history_max = defaults.temporal_history_max;
        settings->max_ray_distance = defaults.max_ray_distance;
        settings->multi_bounce_feedback = defaults.multi_bounce_feedback;
        settings->intensity = defaults.intensity;
        settings->denoiser = STURDY_RESTIR_GI_DENOISER_SVGF;
        settings->svgf_atrous_iterations = defaults.svgf_atrous_iterations;
        settings->svgf_temporal_alpha = defaults.svgf_temporal_alpha;
        settings->svgf_phi_normal = defaults.svgf_phi_normal;
        settings->svgf_phi_depth = defaults.svgf_phi_depth;
        settings->svgf_phi_luminance = defaults.svgf_phi_luminance;
        settings->show_debug_reservoirs = defaults.show_debug_reservoirs ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_frame_set_restir_gi_settings(SturdyFrame frame,
                                                                  const SturdyRestirGiSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "settings must not be null");
        }
        SFT::Engine::RestirGiQuality quality{};
        if (!translate_restir_quality(settings->quality, &quality)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized ReSTIR GI quality");
        }
        SFT::Engine::RestirGiDenoiser denoiser{};
        if (!translate_restir_denoiser(settings->denoiser, &denoiser)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized ReSTIR GI denoiser");
        }
        if (!all_finite({settings->spatial_reuse_radius_px, settings->max_ray_distance,
                         settings->multi_bounce_feedback, settings->intensity, settings->svgf_temporal_alpha,
                         settings->svgf_phi_normal, settings->svgf_phi_depth, settings->svgf_phi_luminance})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "ReSTIR GI settings must be finite");
        }
        SFT::Engine::RenderFrameParameters *parameters = nullptr;
        const SturdyResult resolved = resolve_frame(frame, &parameters);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        SFT::Engine::RestirGiSettings &restir_gi = parameters->render_graph.restir_gi();
        restir_gi.enabled = settings->enabled != STURDY_FALSE;
        restir_gi.quality = quality;
        restir_gi.spatial_reuse_samples = settings->spatial_reuse_samples;
        restir_gi.spatial_reuse_radius_px = settings->spatial_reuse_radius_px;
        restir_gi.temporal_history_max = settings->temporal_history_max;
        restir_gi.max_ray_distance = settings->max_ray_distance;
        restir_gi.multi_bounce_feedback = settings->multi_bounce_feedback;
        restir_gi.intensity = settings->intensity;
        restir_gi.denoiser = denoiser;
        restir_gi.svgf_atrous_iterations = settings->svgf_atrous_iterations;
        restir_gi.svgf_temporal_alpha = settings->svgf_temporal_alpha;
        restir_gi.svgf_phi_normal = settings->svgf_phi_normal;
        restir_gi.svgf_phi_depth = settings->svgf_phi_depth;
        restir_gi.svgf_phi_luminance = settings->svgf_phi_luminance;
        restir_gi.show_debug_reservoirs = settings->show_debug_reservoirs != STURDY_FALSE;
        return STURDY_OK;
    });
}

// ─── Motion blur ────────────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_motion_blur_settings_init(SturdyMotionBlurSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        const SFT::Engine::MotionBlurSettings defaults{};
        *settings = SturdyMotionBlurSettings{};
        settings->struct_size = static_cast<uint32_t>(sizeof(SturdyMotionBlurSettings));
        settings->enabled = defaults.enabled ? STURDY_TRUE : STURDY_FALSE;
        settings->intensity = defaults.intensity;
        settings->shutter_angle_degrees = defaults.shutter_angle_degrees;
        settings->tile_size_px = defaults.tile_size_px;
        settings->sample_count = defaults.sample_count;
        settings->max_blur_radius_px = defaults.max_blur_radius_px;
        settings->background_foreground_weight_bias = defaults.background_foreground_weight_bias;
        settings->camera_motion_only = defaults.camera_motion_only ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_frame_set_motion_blur_settings(SturdyFrame frame,
                                                                    const SturdyMotionBlurSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "settings must not be null");
        }
        if (!all_finite({settings->intensity, settings->shutter_angle_degrees, settings->max_blur_radius_px,
                         settings->background_foreground_weight_bias})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "motion blur settings must be finite");
        }
        SFT::Engine::RenderFrameParameters *parameters = nullptr;
        const SturdyResult resolved = resolve_frame(frame, &parameters);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        SFT::Engine::MotionBlurSettings &motion_blur = parameters->render_graph.motion_blur();
        motion_blur.enabled = settings->enabled != STURDY_FALSE;
        motion_blur.intensity = settings->intensity;
        motion_blur.shutter_angle_degrees = settings->shutter_angle_degrees;
        motion_blur.tile_size_px = settings->tile_size_px;
        motion_blur.sample_count = settings->sample_count;
        motion_blur.max_blur_radius_px = settings->max_blur_radius_px;
        motion_blur.background_foreground_weight_bias = settings->background_foreground_weight_bias;
        motion_blur.camera_motion_only = settings->camera_motion_only != STURDY_FALSE;
        return STURDY_OK;
    });
}

} // extern "C"
